#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <thread>
#include <string>
#include <sys/stat.h>

#include "nsp-plugin.hpp"

using namespace std;
using json = nlohmann::json;

// Recursive mkdir -p.
static void nsp_mkdir_p(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur != "/" && !cur.empty()) ::mkdir(cur.c_str(), 0755);
        }
    }
}

nspPlugin::nspPlugin(const string &tag)
    : ndPluginDetection(tag) {
    Reload();
}

nspPlugin::~nspPlugin() { Join(); }

void nspPlugin::Reload() {
    auto r = nsp::loadConfig();
    for (auto &w : r.warnings) nd_printf("%s: config warning: %s\n", GetTag().c_str(), w.c_str());
    {
        lock_guard<mutex> lg(config_mutex);
        config = std::move(r.config);
    }
    OpenStores();
    LoadCategoryNames(config.categories_path);
    nd_printf("%s: loaded config; store_path=%s top_n=%u interval=%us\n",
        GetTag().c_str(), config.store_path.c_str(), config.top_n_apps, config.sample_interval);
}

void nspPlugin::OpenStores() {
    std::vector<std::string> monitor_ifs;
    std::string store_path;
    std::vector<nsp::TierSpec> tiers;
    uint32_t cap_apps, cap_cats;
    {
        lock_guard<mutex> lcfg(config_mutex);
        monitor_ifs = config.monitor_ifs;
        store_path  = config.store_path;
        tiers       = config.tiers;
        cap_apps    = config.series_capacity_apps;
        cap_cats    = config.series_capacity_cats;
    }

    lock_guard<mutex> lifaces(ifaces_mutex);
    ifaces_.clear();
    flow_iface_.clear();

    for (const auto &iface : monitor_ifs) {
        std::string iface_dir = store_path + "/" + iface;
        nsp_mkdir_p(iface_dir);
        IfaceState &is = ifaces_[iface];
        std::string err;
        bool ok =
            is.apps_store.Open(iface_dir, "apps", tiers, cap_apps, err) &&
            is.cats_store.Open(iface_dir, "cats", tiers, cap_cats, err);
        if (!ok) {
            stat_store_errors++;
            nd_printf("%s: store open failed for %s: %s (continuing in-RAM only)\n",
                GetTag().c_str(), iface.c_str(), err.c_str());
        }
    }
    nd_printf("%s: opened stores for %zu interface(s)\n", GetTag().c_str(), ifaces_.size());
}

// Stable per-flow key: first 8 bytes of the SHA1 lower digest.
uint64_t nspPlugin::FlowKey(ndFlow *flow) {
    uint64_t key = 0;
    memcpy(&key, flow->digest_lower, sizeof(key));
    return key;
}

void nspPlugin::LoadCategoryNames(const std::string &path) {
    std::unordered_map<unsigned, std::string> names;
    std::ifstream f(path);
    if (f.is_open()) {
        try {
            auto j = json::parse(f);
            for (auto it = j.at("application_tag_index").begin();
                 it != j.at("application_tag_index").end(); ++it)
                names[(unsigned)it.value()] = it.key();
            nd_printf("%s: loaded %zu category names from %s\n",
                GetTag().c_str(), names.size(), path.c_str());
        } catch (const std::exception &e) {
            nd_printf("%s: failed to parse %s: %s\n", GetTag().c_str(), path.c_str(), e.what());
        }
    } else {
        nd_printf("%s: categories file not found: %s\n", GetTag().c_str(), path.c_str());
    }
    lock_guard<mutex> lg(config_mutex);
    cat_names = std::move(names);
}

std::string nspPlugin::CategoryName(unsigned cat_id) {
    lock_guard<mutex> lg(config_mutex);
    auto it = cat_names.find(cat_id);
    return (it != cat_names.end()) ? it->second : ("cat-" + std::to_string(cat_id));
}

void nspPlugin::ProcessEvent(ndPluginEvent event, void *) {
    if (event == ndPlugin::EVENT_RELOAD) reload_pending = true;
}

void nspPlugin::ProcessFlow(ndDetectionEvent event, ndFlow *flow) {
    if (event == ndPluginDetection::EVENT_EXPIRING) {
        uint64_t fkey = FlowKey(flow);
        {
            lock_guard<mutex> lg(ifaces_mutex);
            auto fit = flow_iface_.find(fkey);
            if (fit != flow_iface_.end()) {
                auto iit = ifaces_.find(fit->second);
                if (iit != ifaces_.end())
                    iit->second.accum.ForgetFlow(fit->first);
                flow_iface_.erase(fit);
            }
        }
        {
            lock_guard<mutex> llg(live_mutex_);
            live_flow_snap_.erase(fkey);
        }
        return;
    }
    if (event != ndPluginDetection::EVENT_NEW &&
        event != ndPluginDetection::EVENT_UPDATED)
        return;

    std::string iface_name = flow->iface.ifname;

    nsp::Accumulator::FlowSample s;
    s.flow_id      = FlowKey(flow);
    s.app_id       = (unsigned)flow->detected_application;
    s.app_name     = (flow->detected_application_name != NULL && flow->detected_application_name[0] != '\0')
                     ? flow->detected_application_name
                     : ("app-" + std::to_string(s.app_id));
    s.cat_id       = (unsigned)flow->category.application;
    s.cat_name     = CategoryName(s.cat_id);   // takes config_mutex briefly; must be OUTSIDE ifaces_mutex
    s.total_tx_bytes = flow->lower_bytes;
    s.total_rx_bytes = flow->upper_bytes;
    s.total_tx_pkts  = flow->lower_packets;
    s.total_rx_pkts  = flow->upper_packets;
    s.is_new       = (event == ndPluginDetection::EVENT_NEW);
    s.iface_name   = iface_name;

    {
        lock_guard<mutex> lg(ifaces_mutex);
        flow_iface_[s.flow_id] = iface_name;
        auto it = ifaces_.find(iface_name);
        if (it != ifaces_.end())
            it->second.accum.Observe(s);
        // Flows on unconfigured interfaces are counted but not accumulated.
    }

    // ── Live accumulation (separate lock, never held with ifaces_mutex) ──────
    {
        lock_guard<mutex> llg(live_mutex_);

        // Compute per-flow delta from cumulative counters.
        uint64_t cur_upper = flow->upper_bytes;
        uint64_t cur_lower = flow->lower_bytes;

        LiveSnap &snap = live_flow_snap_[s.flow_id];
        uint64_t delta_upper = (cur_upper >= snap.upper) ? (cur_upper - snap.upper) : 0;
        uint64_t delta_lower = (cur_lower >= snap.lower) ? (cur_lower - snap.lower) : 0;
        snap.upper = cur_upper;
        snap.lower = cur_lower;

        // Determine local device perspective.
        // If lower side is local: local RX = upper delta, local TX = lower delta.
        bool lower_is_local = (flow->lower_map == ndFlow::LOWER_LOCAL);
        uint64_t host_rx = lower_is_local ? delta_upper : delta_lower;
        uint64_t host_tx = lower_is_local ? delta_lower : delta_upper;

        // Determine local MAC and IP.
        std::string local_mac = lower_is_local
            ? flow->lower_mac.GetString()
            : flow->upper_mac.GetString();
        std::string local_ip  = lower_is_local
            ? flow->lower_addr.GetString()
            : flow->upper_addr.GetString();

        if (!local_mac.empty() && (host_rx > 0 || host_tx > 0)) {
            // Per-interface global app/cat totals.
            live_iface_[iface_name].apps[s.app_name].rx_bytes += host_rx;
            live_iface_[iface_name].apps[s.app_name].tx_bytes += host_tx;
            live_iface_[iface_name].cats[s.cat_name].rx_bytes += host_rx;
            live_iface_[iface_name].cats[s.cat_name].tx_bytes += host_tx;

            // Per-host totals (aggregated across interfaces).
            auto &h = live_hosts_[local_mac];
            h.ip = local_ip;
            h.apps[s.app_name].rx_bytes += host_rx;
            h.apps[s.app_name].tx_bytes += host_tx;
            h.total.rx_bytes += host_rx;
            h.total.tx_bytes += host_tx;
        }
    }

    stat_events++;
}

void *nspPlugin::Entry(void) {
    using namespace std::chrono;
    auto next = steady_clock::now();
    while (!ShouldTerminate()) {
        if (reload_pending.exchange(false)) Reload();

        unsigned interval; size_t top_n;
        {
            lock_guard<mutex> lg(config_mutex);
            interval = config.sample_interval;
            top_n    = config.top_n_apps;
        }
        next += seconds(interval);
        while (!ShouldTerminate() && steady_clock::now() < next)
            this_thread::sleep_for(milliseconds(200));
        if (ShouldTerminate()) break;

        int64_t epoch = (int64_t)time(nullptr);
        {
            lock_guard<mutex> lg(ifaces_mutex);
            for (auto &kv : ifaces_) {
                auto snap = kv.second.accum.SampleAndReset(top_n);
                stat_series_dropped += kv.second.apps_store.AppendSample(epoch, snap.apps);
                stat_series_dropped += kv.second.cats_store.AppendSample(epoch, snap.cats);
            }
        }
        stat_samples++;
    }
    return nullptr;
}

void nspPlugin::GetVersion(string &version) { version = string(PACKAGE_VERSION); }

void nspPlugin::GetStatus(json &status) {
    status["plugin_version"] = _NSP_PLUGIN_VER;
    status["events"]         = stat_events.load();
    status["samples"]        = stat_samples.load();
    status["store_errors"]   = stat_store_errors.load();
    status["series_dropped"] = stat_series_dropped.load();
    {
        lock_guard<mutex> lg(config_mutex);
        status["store_path"] = config.store_path;
    }
    {
        lock_guard<mutex> lg(ifaces_mutex);
        auto arr = nlohmann::json::array();
        for (const auto &kv : ifaces_) arr.push_back(kv.first);
        status["interfaces"] = arr;
    }
}

ndPluginInit(nspPlugin);

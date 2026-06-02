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
    lock_guard<mutex> lg(config_mutex);
    nsp_mkdir_p(config.store_path);
    string err;
    store_ok =
        apps_store.Open(config.store_path, "apps", config.tiers, config.series_capacity_apps, err) &&
        cats_store.Open(config.store_path, "cats", config.tiers, config.series_capacity_cats, err);
    if (!store_ok) {
        stat_store_errors++;
        nd_printf("%s: store open failed: %s (continuing in-RAM only)\n", GetTag().c_str(), err.c_str());
    }
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
        lock_guard<mutex> lg(accum_mutex);
        accum.ForgetFlow(FlowKey(flow));
        return;
    }
    if (event != ndPluginDetection::EVENT_NEW &&
        event != ndPluginDetection::EVENT_UPDATED)
        return;

    nsp::Accumulator::FlowSample s;
    s.flow_id  = FlowKey(flow);
    s.app_id   = (unsigned)flow->detected_application;
    s.app_name = (flow->detected_application_name != NULL && flow->detected_application_name[0] != '\0')
        ? flow->detected_application_name
        : ("app-" + std::to_string(s.app_id));
    s.cat_id   = (unsigned)flow->category.application;
    s.cat_name = CategoryName(s.cat_id);
    s.total_tx_bytes = flow->lower_bytes;
    s.total_rx_bytes = flow->upper_bytes;
    s.total_tx_pkts  = flow->lower_packets;
    s.total_rx_pkts  = flow->upper_packets;
    s.is_new = (event == ndPluginDetection::EVENT_NEW);

    {
        lock_guard<mutex> lg(accum_mutex);
        accum.Observe(s);
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
            top_n = config.top_n_apps;
        }
        next += seconds(interval);
        while (!ShouldTerminate() && steady_clock::now() < next)
            this_thread::sleep_for(milliseconds(200));
        if (ShouldTerminate()) break;

        nsp::Accumulator::Snapshot snap;
        {
            lock_guard<mutex> lg(accum_mutex);
            snap = accum.SampleAndReset(top_n);
        }
        int64_t epoch = (int64_t)time(nullptr);
        if (store_ok) {
            lock_guard<mutex> lg(config_mutex);
            stat_series_dropped += apps_store.AppendSample(epoch, snap.apps);
            stat_series_dropped += cats_store.AppendSample(epoch, snap.cats);
        }
        stat_samples++;
    }
    return nullptr;
}

void nspPlugin::GetVersion(string &version) { version = string(PACKAGE_VERSION); }

void nspPlugin::GetStatus(json &status) {
    status["plugin_version"] = _NSP_PLUGIN_VER;
    status["events"] = stat_events.load();
    status["samples"] = stat_samples.load();
    status["store_errors"] = stat_store_errors.load();
    status["series_dropped"] = stat_series_dropped.load();
    lock_guard<mutex> lg(config_mutex);
    status["store_path"] = config.store_path;
    status["store_ok"] = store_ok.load();
}

ndPluginInit(nspPlugin);

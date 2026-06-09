#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>
#include <string>
#include <sys/stat.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>

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

    {
        lock_guard<mutex> lifaces(ifaces_mutex);
        ifaces_.clear();
    }

    {
        lock_guard<mutex> llg(live_mutex_);
        live_iface_.clear();
        live_hosts_.clear();
        live_flow_snap_.clear();
        live_start_ = 0;
    }

    {
        lock_guard<mutex> lifaces(ifaces_mutex);
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

void nspPlugin::ReadArpTable() {
    std::ifstream f("/proc/net/arp");
    if (!f.is_open()) return;
    std::string line;
    std::getline(f, line);  // discard header
    lock_guard<mutex> lg(ct_mutex_);
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string ip, hw_type, flags, mac, mask, dev;
        if (!(iss >> ip >> hw_type >> flags >> mac >> mask >> dev)) continue;
        if (flags != "0x2") continue;  // 0x2 = ATF_COM (complete, reachable)
        mac_map_[ip] = mac;
    }
}

// Context struct passed to the nfct C callback.
struct CtDumpCtx {
    nspPlugin *self;
    const nsp::Config &cfg;
    std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_apps;
    std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_cats;
};

static int ct_cb(enum nf_conntrack_msg_type /*type*/,
                 struct nf_conntrack *ct, void *data) {
    auto *ctx = static_cast<CtDumpCtx *>(data);
    ctx->self->ProcessCtEntry(ct, ctx->cfg, ctx->tick_apps, ctx->tick_cats);
    return NFCT_CB_CONTINUE;
}

void nspPlugin::ConntrackDump(
    const nsp::Config &cfg,
    std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_apps,
    std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_cats)
{
    nfct_handle *h = nfct_open(CONNTRACK, 0);
    if (!h) {
        nd_printf("%s: nfct_open failed: %s\n", GetTag().c_str(), strerror(errno));
        return;
    }
    CtDumpCtx ctx{this, cfg, tick_apps, tick_cats};
    nfct_callback_register(h, NFCT_T_ALL, ct_cb, &ctx);
    uint32_t family = AF_INET;
    if (nfct_query(h, NFCT_Q_DUMP, &family) < 0)
        nd_printf("%s: nfct_query failed: %s\n", GetTag().c_str(), strerror(errno));
    nfct_close(h);
}

void nspPlugin::ProcessCtEntry(
    struct nf_conntrack *ct,
    const nsp::Config &cfg,
    std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_apps,
    std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_cats)
{
    // IPv4 only; skip if byte counters unavailable (needs CONFIG_NF_CT_ACCT=y).
    if (!nfct_attr_is_set(ct, ATTR_ORIG_IPV4_SRC)) return;
    if (!nfct_attr_is_set(ct, ATTR_ORIG_COUNTER_BYTES)) return;

    // ── Build lookup key ─────────────────────────────────────────────────────
    ConntrackKey ck;
    ck.proto = nfct_get_attr_u8(ct, ATTR_PROTO_NUM);

    struct in_addr a;
    a.s_addr = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_SRC);
    ck.orig_src_ip = inet_ntoa(a);
    a.s_addr = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_DST);
    ck.orig_dst_ip = inet_ntoa(a);

    ck.orig_sport = nfct_attr_is_set(ct, ATTR_ORIG_PORT_SRC)
        ? ntohs(nfct_get_attr_u16(ct, ATTR_ORIG_PORT_SRC)) : 0;
    ck.orig_dport = nfct_attr_is_set(ct, ATTR_ORIG_PORT_DST)
        ? ntohs(nfct_get_attr_u16(ct, ATTR_ORIG_PORT_DST)) : 0;

    uint64_t orig_bytes = nfct_get_attr_u64(ct, ATTR_ORIG_COUNTER_BYTES);
    uint64_t repl_bytes = nfct_attr_is_set(ct, ATTR_REPL_COUNTER_BYTES)
        ? nfct_get_attr_u64(ct, ATTR_REPL_COUNTER_BYTES) : 0;

    uint32_t repl_dst_nbo = nfct_attr_is_set(ct, ATTR_REPL_IPV4_DST)
        ? nfct_get_attr_u32(ct, ATTR_REPL_IPV4_DST) : 0;

    // ── Delta + classification (under ct_mutex_) ─────────────────────────────
    uint64_t delta_orig = 0, delta_repl = 0;
    std::string app_name, cat_name, client_ip, client_mac, iface_name;
    bool nat_flow = false;
    std::string wan_ip, wan_key;

    {
        lock_guard<mutex> lg(ct_mutex_);

        auto &snap    = ct_snap_[ck];
        delta_orig = orig_bytes >= snap.orig_bytes ? orig_bytes - snap.orig_bytes : orig_bytes;
        delta_repl = repl_bytes >= snap.repl_bytes ? repl_bytes - snap.repl_bytes : repl_bytes;
        snap.orig_bytes = orig_bytes;
        snap.repl_bytes = repl_bytes;

        auto it = ct_flow_map_.find(ck);
        if (it != ct_flow_map_.end()) {
            app_name   = it->second.app_name;
            cat_name   = it->second.cat_name;
            client_ip  = it->second.client_ip;
            client_mac = it->second.client_mac;
            iface_name = it->second.iface_name;
            if (client_mac.empty() && mac_map_.count(client_ip))
                client_mac = mac_map_.at(client_ip);
        } else {
            app_name = cat_name = "Unidentified";
            struct in_addr sa; sa.s_addr = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_SRC);
            client_ip  = inet_ntoa(sa);
            client_mac = mac_map_.count(client_ip) ? mac_map_.at(client_ip) : "";
        }

        uint32_t orig_src_nbo = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_SRC);
        nat_flow = (repl_dst_nbo != 0 && repl_dst_nbo != orig_src_nbo);
        if (nat_flow) {
            struct in_addr wa; wa.s_addr = repl_dst_nbo;
            wan_ip  = inet_ntoa(wa);
            wan_key = mac_map_.count(wan_ip) ? mac_map_.at(wan_ip) : wan_ip;
        }
    }

    if (delta_orig == 0 && delta_repl == 0) return;

    const uint64_t client_tx = delta_orig;  // client→server
    const uint64_t client_rx = delta_repl;  // server→client

    // ── Ring buffer tick buckets ──────────────────────────────────────────────
    if (!iface_name.empty()) {
        tick_apps[iface_name][app_name].tx_bytes += client_tx;
        tick_apps[iface_name][app_name].rx_bytes += client_rx;
        tick_cats[iface_name][cat_name].tx_bytes += client_tx;
        tick_cats[iface_name][cat_name].rx_bytes += client_rx;
    }
    if (nat_flow) {
        for (const auto &iface : cfg.monitor_ifs) {
            if (iface == iface_name) continue;
            tick_apps[iface][app_name].tx_bytes += client_tx;
            tick_apps[iface][app_name].rx_bytes += client_rx;
            tick_cats[iface][cat_name].tx_bytes += client_tx;
            tick_cats[iface][cat_name].rx_bytes += client_rx;
        }
    }

    // ── Live layer ────────────────────────────────────────────────────────────
    {
        lock_guard<mutex> llg(live_mutex_);

        if (!iface_name.empty()) {
            live_iface_[iface_name].apps[app_name].rx_bytes += client_rx;
            live_iface_[iface_name].apps[app_name].tx_bytes += client_tx;
            live_iface_[iface_name].cats[cat_name].rx_bytes += client_rx;
            live_iface_[iface_name].cats[cat_name].tx_bytes += client_tx;
        }

        const std::string &host_key = client_mac.empty() ? client_ip : client_mac;
        if (!host_key.empty()) {
            auto &h = live_hosts_[host_key];
            h.ip = client_ip;
            h.apps[app_name].rx_bytes += client_rx;
            h.apps[app_name].tx_bytes += client_tx;
            h.total.rx_bytes += client_rx;
            h.total.tx_bytes += client_tx;
        }

        if (nat_flow && !wan_key.empty()) {
            auto &wh = live_hosts_[wan_key];
            wh.ip = wan_ip;
            wh.apps[app_name].rx_bytes += client_rx;
            wh.apps[app_name].tx_bytes += client_tx;
            wh.total.rx_bytes += client_rx;
            wh.total.tx_bytes += client_tx;
        }
    }
}

void nspPlugin::ProcessEvent(ndPluginEvent event, void *) {
    if (event == ndPlugin::EVENT_RELOAD) reload_pending = true;
}

void nspPlugin::ProcessFlow(ndDetectionEvent event, ndFlow *flow) {
    if (event == ndPluginDetection::EVENT_EXPIRING) {
        uint64_t fkey = FlowKey(flow);

        // Derive names needed by the live layer.
        // CategoryName() takes config_mutex — call before any other lock.
        unsigned app_id  = (unsigned)flow->detected_application;
        std::string app_name = (flow->detected_application_name != NULL && flow->detected_application_name[0] != '\0')
                               ? flow->detected_application_name
                               : ("app-" + std::to_string(app_id));
        std::string cat_name  = CategoryName((unsigned)flow->category.application);
        std::string iface_name = flow->iface.ifname;

        // Final live-layer delta: capture bytes since last live snapshot.
        {
            lock_guard<mutex> llg(live_mutex_);
            if (flow->lower_map != ndFlow::LOWER_UNKNOWN) {
                uint64_t cur_upper = flow->upper_bytes;
                uint64_t cur_lower = flow->lower_bytes;
                auto snap_it = live_flow_snap_.find(fkey);
                if (snap_it != live_flow_snap_.end()) {
                    uint64_t du = (cur_upper >= snap_it->second.upper) ? (cur_upper - snap_it->second.upper) : 0;
                    uint64_t dl = (cur_lower >= snap_it->second.lower) ? (cur_lower - snap_it->second.lower) : 0;
                    bool lower_is_local = (flow->lower_map == ndFlow::LOWER_LOCAL);
                    uint64_t host_rx = lower_is_local ? du : dl;
                    uint64_t host_tx = lower_is_local ? dl : du;
                    std::string local_mac = lower_is_local
                        ? flow->lower_mac.GetString() : flow->upper_mac.GetString();
                    static const std::string kZeroMac = "00:00:00:00:00:00";
                    if (!local_mac.empty() && local_mac != kZeroMac && (host_rx > 0 || host_tx > 0)) {
                        live_iface_[iface_name].apps[app_name].rx_bytes += host_rx;
                        live_iface_[iface_name].apps[app_name].tx_bytes += host_tx;
                        live_iface_[iface_name].cats[cat_name].rx_bytes += host_rx;
                        live_iface_[iface_name].cats[cat_name].tx_bytes += host_tx;
                        auto &h = live_hosts_[local_mac];
                        h.ip = lower_is_local
                            ? flow->lower_addr.GetString() : flow->upper_addr.GetString();
                        h.apps[app_name].rx_bytes += host_rx;
                        h.apps[app_name].tx_bytes += host_tx;
                        h.total.rx_bytes += host_rx;
                        h.total.tx_bytes += host_tx;
                    }
                }
            }
            live_flow_snap_.erase(fkey);
        }

        // Remove from conntrack classification and snap maps.
        if (flow->lower_map != ndFlow::LOWER_UNKNOWN) {
            ConntrackKey ck;
            ck.proto       = flow->ip_protocol;
            ck.orig_src_ip = flow->lower_addr.GetString();
            ck.orig_dst_ip = flow->upper_addr.GetString();
            ck.orig_sport  = (uint16_t)flow->lower_addr.GetPort();
            ck.orig_dport  = (uint16_t)flow->upper_addr.GetPort();
            lock_guard<mutex> lg(ct_mutex_);
            ct_flow_map_.erase(ck);
            ct_snap_.erase(ck);
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

    // ── Build conntrack classification map ────────────────────────────────────
    {
        ConntrackKey ck;
        ck.proto       = flow->ip_protocol;
        ck.orig_src_ip = flow->lower_addr.GetString();
        ck.orig_dst_ip = flow->upper_addr.GetString();
        ck.orig_sport  = (uint16_t)flow->lower_addr.GetPort();
        ck.orig_dport  = (uint16_t)flow->upper_addr.GetPort();

        bool lower_is_local = (flow->lower_map == ndFlow::LOWER_LOCAL);
        std::string client_ip = lower_is_local
            ? flow->lower_addr.GetString() : flow->upper_addr.GetString();

        lock_guard<mutex> lg(ct_mutex_);
        auto &fc = ct_flow_map_[ck];
        fc.app_name   = s.app_name;
        fc.cat_name   = s.cat_name;
        fc.client_ip  = client_ip;
        fc.iface_name = iface_name;
        if (fc.client_mac.empty() && mac_map_.count(client_ip))
            fc.client_mac = mac_map_.at(client_ip);
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

        // Skip if local side is indeterminate (tunnels, loopback, VPN).
        if (flow->lower_map != ndFlow::LOWER_UNKNOWN) {
            bool lower_is_local = (flow->lower_map == ndFlow::LOWER_LOCAL);
            uint64_t host_rx = lower_is_local ? delta_upper : delta_lower;
            uint64_t host_tx = lower_is_local ? delta_lower : delta_upper;

            std::string local_mac = lower_is_local
                ? flow->lower_mac.GetString()
                : flow->upper_mac.GetString();
            std::string local_ip  = lower_is_local
                ? flow->lower_addr.GetString()
                : flow->upper_addr.GetString();

            // Filter zero MACs (virtual interfaces, TAP tunnels).
            static const std::string kZeroMac = "00:00:00:00:00:00";
            if (!local_mac.empty() && local_mac != kZeroMac && (host_rx > 0 || host_tx > 0)) {
                live_iface_[iface_name].apps[s.app_name].rx_bytes += host_rx;
                live_iface_[iface_name].apps[s.app_name].tx_bytes += host_tx;
                live_iface_[iface_name].cats[s.cat_name].rx_bytes += host_rx;
                live_iface_[iface_name].cats[s.cat_name].tx_bytes += host_tx;

                auto &h = live_hosts_[local_mac];
                h.ip = local_ip;
                h.apps[s.app_name].rx_bytes += host_rx;
                h.apps[s.app_name].tx_bytes += host_tx;
                h.total.rx_bytes += host_rx;
                h.total.tx_bytes += host_tx;
            }
        }
    }

    stat_events++;
}

// Rank NamedMetrics by total bytes, keep top_n, fold remainder into __other__.
static std::vector<nsp::NamedMetrics> applyTopN(
    std::map<std::string, nsp::Metrics> &src, size_t top_n)
{
    std::vector<nsp::NamedMetrics> ranked;
    ranked.reserve(src.size());
    for (auto &[name, m] : src) ranked.push_back({name, m});
    std::sort(ranked.begin(), ranked.end(), [](const nsp::NamedMetrics &a, const nsp::NamedMetrics &b) {
        return (a.m.tx_bytes + a.m.rx_bytes) > (b.m.tx_bytes + b.m.rx_bytes);
    });
    nsp::Metrics other; bool have_other = false;
    std::vector<nsp::NamedMetrics> out;
    for (size_t i = 0; i < ranked.size(); ++i) {
        if (i < top_n) { out.push_back(ranked[i]); continue; }
        have_other = true;
        other.tx_bytes += ranked[i].m.tx_bytes; other.rx_bytes += ranked[i].m.rx_bytes;
        other.tx_pkts  += ranked[i].m.tx_pkts;  other.rx_pkts  += ranked[i].m.rx_pkts;
        other.flows    += ranked[i].m.flows;
    }
    if (have_other) out.push_back({nsp::OTHER_SERIES, other});
    return out;
}

void *nspPlugin::Entry(void) {
    using namespace std::chrono;
    auto next = steady_clock::now();
    while (!ShouldTerminate()) {
        if (reload_pending.exchange(false)) Reload();

        nsp::Config cfg;
        unsigned interval; size_t top_n;
        {
            lock_guard<mutex> lg(config_mutex);
            cfg      = config;
            interval = config.sample_interval;
            top_n    = config.top_n_apps;
        }
        next += seconds(interval);
        while (!ShouldTerminate() && steady_clock::now() < next)
            this_thread::sleep_for(milliseconds(200));
        if (ShouldTerminate()) break;

        int64_t epoch = (int64_t)time(nullptr);

        // Update ARP table then poll conntrack for per-tick byte deltas.
        ReadArpTable();
        std::map<std::string, std::map<std::string, nsp::Metrics>> tick_apps, tick_cats;
        ConntrackDump(cfg, tick_apps, tick_cats);

        // Flush tick accumulators to ring buffer stores.
        {
            lock_guard<mutex> lg(ifaces_mutex);
            for (const auto &iface : cfg.monitor_ifs) {
                auto it = ifaces_.find(iface);
                if (it == ifaces_.end()) continue;
                auto &is = it->second;
                if (tick_apps.count(iface))
                    stat_series_dropped += is.apps_store.AppendSample(
                        epoch, applyTopN(tick_apps[iface], top_n));
                if (tick_cats.count(iface))
                    stat_series_dropped += is.cats_store.AppendSample(
                        epoch, applyTopN(tick_cats[iface], static_cast<size_t>(-1)));
            }
        }
        stat_samples++;

        // Live layer: sentinel reset + auto-reset + write JSON snapshot.
        nsp::Config live_cfg;
        { lock_guard<mutex> lg(config_mutex); live_cfg = config; }
        std::map<std::string, LiveIfaceEntry>  snap_iface;
        std::map<std::string, LiveHostEntry>   snap_hosts;
        int64_t snap_start = 0;
        {
            lock_guard<mutex> llg(live_mutex_);
            std::string sentinel = live_cfg.store_path + "/.reset-live";
            struct stat sst;
            if (::stat(sentinel.c_str(), &sst) == 0) {
                live_iface_.clear(); live_hosts_.clear(); live_flow_snap_.clear();
                live_start_ = time(nullptr);
                ::unlink(sentinel.c_str());
                nd_printf("%s: live data reset (sentinel)\n", GetTag().c_str());
            }
            if (live_start_ == 0) live_start_ = time(nullptr);
            if (live_cfg.live_duration > 0 &&
                (time(nullptr) - live_start_) >= (time_t)live_cfg.live_duration) {
                live_iface_.clear(); live_hosts_.clear(); live_flow_snap_.clear();
                live_start_ = time(nullptr);
                nd_printf("%s: live data auto-reset\n", GetTag().c_str());
            }
            snap_iface = live_iface_;
            snap_hosts = live_hosts_;
            snap_start = live_start_;
        }
        WriteLiveJson(snap_iface, snap_hosts, snap_start, live_cfg);
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

// Helper: strip "netify." prefix for display in live JSON.
static std::string live_clean(const std::string &n) {
    return (n.rfind("netify.", 0) == 0) ? n.substr(7) : n;
}

void nspPlugin::WriteLiveJson(
    const std::map<std::string, LiveIfaceEntry> &iface_data,
    const std::map<std::string, LiveHostEntry>  &host_data,
    int64_t start,
    const nsp::Config &cfg)
{
    // Builds query_live response from snapshot data and writes atomically.
    using njson = nlohmann::json;

    njson out;
    out["start"]    = start;
    out["duration"] = cfg.live_duration;

    // ── Apps (sorted by total, with per-interface breakdown) ────────────────
    std::map<std::string, std::pair<uint64_t, std::map<std::string, LiveMetrics>>> app_agg;
    for (const auto &[iface, ie] : iface_data) {
        for (const auto &[aname, m] : ie.apps) {
            auto &a = app_agg[aname];
            a.first += m.rx_bytes + m.tx_bytes;
            auto &im = a.second[iface];
            im.rx_bytes += m.rx_bytes;
            im.tx_bytes += m.tx_bytes;
        }
    }
    std::vector<std::pair<uint64_t, std::string>> app_sorted;
    app_sorted.reserve(app_agg.size());
    for (const auto &[n, v] : app_agg) app_sorted.push_back({v.first, n});
    std::sort(app_sorted.rbegin(), app_sorted.rend());

    njson apps_arr = njson::array();
    for (const auto &[tot, raw_name] : app_sorted) {
        njson ao;
        ao["name"] = live_clean(raw_name);
        uint64_t trx = 0, ttx = 0;
        njson ifo = njson::object();
        for (const auto &[iface, m] : app_agg[raw_name].second) {
            trx += m.rx_bytes; ttx += m.tx_bytes;
            ifo[iface] = {{"rx", (int64_t)m.rx_bytes}, {"tx", (int64_t)m.tx_bytes}};
        }
        ao["rx"] = (int64_t)trx; ao["tx"] = (int64_t)ttx;
        ao["interfaces"] = ifo;
        apps_arr.push_back(std::move(ao));
    }
    out["apps"] = std::move(apps_arr);

    // ── Cats (sorted by total, with per-interface breakdown) ────────────────
    std::map<std::string, std::pair<uint64_t, std::map<std::string, LiveMetrics>>> cat_agg;
    for (const auto &[iface, ie] : iface_data) {
        for (const auto &[cname, m] : ie.cats) {
            auto &c = cat_agg[cname];
            c.first += m.rx_bytes + m.tx_bytes;
            auto &im = c.second[iface];
            im.rx_bytes += m.rx_bytes;
            im.tx_bytes += m.tx_bytes;
        }
    }
    std::vector<std::pair<uint64_t, std::string>> cat_sorted;
    cat_sorted.reserve(cat_agg.size());
    for (const auto &[n, v] : cat_agg) cat_sorted.push_back({v.first, n});
    std::sort(cat_sorted.rbegin(), cat_sorted.rend());

    njson cats_arr = njson::array();
    for (const auto &[tot, raw_name] : cat_sorted) {
        njson co;
        co["name"] = live_clean(raw_name);
        uint64_t trx = 0, ttx = 0;
        njson ifo = njson::object();
        for (const auto &[iface, m] : cat_agg[raw_name].second) {
            trx += m.rx_bytes; ttx += m.tx_bytes;
            ifo[iface] = {{"rx", (int64_t)m.rx_bytes}, {"tx", (int64_t)m.tx_bytes}};
        }
        co["rx"] = (int64_t)trx; co["tx"] = (int64_t)ttx;
        co["interfaces"] = ifo;
        cats_arr.push_back(std::move(co));
    }
    out["cats"] = std::move(cats_arr);

    // ── Hosts (sorted by total, truncated to top_n_hosts) ───────────────────
    std::vector<std::pair<uint64_t, std::string>> host_sorted;
    host_sorted.reserve(host_data.size());
    for (const auto &[mac, h] : host_data)
        host_sorted.push_back({h.total.rx_bytes + h.total.tx_bytes, mac});
    std::sort(host_sorted.rbegin(), host_sorted.rend());
    if (host_sorted.size() > cfg.top_n_hosts)
        host_sorted.resize(cfg.top_n_hosts);

    njson hosts_arr = njson::array();
    for (const auto &[tot, mac] : host_sorted) {
        const auto &h = host_data.at(mac);
        njson ho;
        ho["mac"] = mac;
        ho["ip"]  = h.ip;
        ho["rx"]  = (int64_t)h.total.rx_bytes;
        ho["tx"]  = (int64_t)h.total.tx_bytes;

        std::vector<std::pair<uint64_t, std::string>> happ_sorted;
        happ_sorted.reserve(h.apps.size());
        for (const auto &[an, m] : h.apps)
            happ_sorted.push_back({m.rx_bytes + m.tx_bytes, an});
        std::sort(happ_sorted.rbegin(), happ_sorted.rend());
        if (happ_sorted.size() > cfg.top_n_apps)
            happ_sorted.resize(cfg.top_n_apps);

        njson happs = njson::array();
        for (const auto &[t, raw_an] : happ_sorted) {
            const auto &m = h.apps.at(raw_an);
            happs.push_back({{"name", live_clean(raw_an)},
                             {"rx",   (int64_t)m.rx_bytes},
                             {"tx",   (int64_t)m.tx_bytes}});
        }
        ho["apps"] = std::move(happs);
        hosts_arr.push_back(std::move(ho));
    }
    out["hosts"] = std::move(hosts_arr);

    // Write atomically via temp file + rename.
    std::string path     = cfg.store_path + "/.live.json";
    std::string tmp_path = path + ".tmp";
    std::ofstream f(tmp_path);
    if (f.is_open()) {
        f << out.dump();
        f.close();
        if (f.fail()) {
            nd_printf("nsp: WriteLiveJson write failed, discarding\n");
            ::unlink(tmp_path.c_str());
        } else if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
            nd_printf("nsp: WriteLiveJson rename failed\n");
            ::unlink(tmp_path.c_str());
        }
    }
}

ndPluginInit(nspPlugin);

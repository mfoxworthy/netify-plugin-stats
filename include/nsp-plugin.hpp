// netify-plugin-stats — plugin class (netifyd 4.4.7 API)
#pragma once

// ── STL ──────────────────────────────────────────────────────────────────────
// Must precede all netifyd 4.4.7 headers; they use bare 'string', 'map' etc.
// and rely on these being in scope.
#include <atomic>
#include <bitset>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── System ───────────────────────────────────────────────────────────────────
#include <dlfcn.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// ── pcap (required by several nd-*.h) ────────────────────────────────────────
#include <pcap/pcap.h>

// ── Third-party bundled with netifyd ─────────────────────────────────────────
#include <nlohmann/json.hpp>
#include <radix/radix_tree.hpp>

// 4.4.7 headers use unqualified identifiers and rely on these.
using namespace std;
using json = nlohmann::json;

// ── netifyd headers — exact order from nd-plugin.cpp (confirmed working) ─────
#include <netifyd.h>
#include <nd-sha1.h>
#include <nd-ndpi.h>
#include <nd-thread.h>
#include <nd-risks.h>
#include <nd-serializer.h>
#include <nd-packet.h>
#include <nd-json.h>
#include <nd-util.h>
#include <nd-addr.h>
#include <nd-apps.h>
#include <nd-protos.h>
#include <nd-category.h>
#include <nd-flow.h>
#include <nd-flow-map.h>
#include <nd-plugin.h>

// ── Plugin internals ──────────────────────────────────────────────────────────
#include "nsp-config.hpp"
#include "nsp-accum.hpp"
#include "nsp-store.hpp"

constexpr unsigned _NSP_PLUGIN_VER = 0x20260531;

// ── Live (in-memory, cross-dimensional) data model ───────────────────────────

// Raw cumulative bytes snapshot per flow, for live delta computation.
struct LiveSnap {
    uint64_t upper = 0;   // flow->upper_bytes at last observation
    uint64_t lower = 0;   // flow->lower_bytes at last observation
};

// Accumulated bytes in the live window (from the local host's perspective).
struct LiveMetrics {
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
};

// Per-host entry: current IP, per-app breakdown, running total.
struct LiveHostEntry {
    std::string ip;
    std::map<std::string, LiveMetrics> apps;   // app_name → metrics
    LiveMetrics total;
};

// Per-interface global totals (app + cat dimensions).
struct LiveIfaceEntry {
    std::map<std::string, LiveMetrics> apps;   // app_name → metrics
    std::map<std::string, LiveMetrics> cats;   // cat_name → metrics
};

// ── Conntrack accounting ──────────────────────────────────────────────────────

struct nf_conntrack;  // forward declaration — full type in nsp-plugin.cpp only

// 5-tuple key for correlating netifyd flows with conntrack entries.
struct ConntrackKey {
    uint8_t     proto      = 0;
    std::string orig_src_ip;
    std::string orig_dst_ip;
    uint16_t    orig_sport = 0;
    uint16_t    orig_dport = 0;
    bool operator<(const ConntrackKey &o) const {
        if (proto       != o.proto)       return proto       < o.proto;
        if (orig_src_ip != o.orig_src_ip) return orig_src_ip < o.orig_src_ip;
        if (orig_dst_ip != o.orig_dst_ip) return orig_dst_ip < o.orig_dst_ip;
        if (orig_sport  != o.orig_sport)  return orig_sport  < o.orig_sport;
        return orig_dport < o.orig_dport;
    }
};

// DPI classification for a flow (populated by ProcessFlow events).
struct FlowClass {
    std::string app_name;
    std::string cat_name;
    std::string client_ip;
    std::string client_mac;
    std::string iface_name;
};

// Last-seen conntrack byte counts for delta computation.
struct ConntrackSnap {
    uint64_t orig_bytes = 0;
    uint64_t repl_bytes = 0;
};

class nspPlugin : public ndPluginDetection {
public:
    nspPlugin(const string &tag);
    virtual ~nspPlugin();

    virtual void *Entry(void) override;
    virtual void ProcessEvent(ndPluginEvent event, void *param = NULL) override;
    virtual void ProcessFlow(ndDetectionEvent event, ndFlow *flow) override;
    virtual void GetVersion(string &version) override;

    void GetStatus(nlohmann::json &status);

protected:
    atomic<bool> reload_pending{true};

    nsp::Config config;
    mutex config_mutex;

    struct IfaceState {
        nsp::TierSet apps_store;
        nsp::TierSet cats_store;
    };
    std::map<std::string, IfaceState> ifaces_;
    mutex ifaces_mutex;

    // Live cross-dimensional accumulation (written to .live.json each tick).
    std::map<std::string,  LiveIfaceEntry>  live_iface_;   // iface  → global totals
    std::map<std::string,  LiveHostEntry>   live_hosts_;   // mac    → per-host totals
    std::map<uint64_t,     LiveSnap>        live_flow_snap_;  // flow_id → cumulative snapshot
    time_t  live_start_ = 0;
    mutex   live_mutex_;

    // Conntrack-based byte accounting
    std::map<ConntrackKey, FlowClass>     ct_flow_map_;
    std::map<ConntrackKey, ConntrackSnap> ct_snap_;
    std::map<std::string, std::string>    mac_map_;   // IP → MAC
    mutex ct_mutex_;

    void ReadArpTable();
    void ConntrackDump(
        const nsp::Config &cfg,
        std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_apps,
        std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_cats);
    void ProcessCtEntry(
        struct nf_conntrack *ct,
        const nsp::Config &cfg,
        std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_apps,
        std::map<std::string, std::map<std::string, nsp::Metrics>> &tick_cats);

    static void WriteLiveJson(
        const std::map<std::string, LiveIfaceEntry> &iface_data,
        const std::map<std::string, LiveHostEntry>  &host_data,
        int64_t start,
        const nsp::Config &cfg);

    atomic<uint64_t> stat_events{0};
    atomic<uint64_t> stat_samples{0};
    atomic<uint64_t> stat_store_errors{0};
    atomic<uint64_t> stat_series_dropped{0};

    void Reload();
    void OpenStores();
    void LoadCategoryNames(const string &path);
    static uint64_t FlowKey(ndFlow *flow);
    string CategoryName(unsigned cat_id);

    unordered_map<unsigned, string> cat_names;
};

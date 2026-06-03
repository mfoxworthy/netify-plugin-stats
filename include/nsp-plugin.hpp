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
        nsp::Accumulator accum;
        nsp::TierSet apps_store;
        nsp::TierSet cats_store;
    };
    std::map<std::string, IfaceState> ifaces_;
    std::map<uint64_t, std::string> flow_iface_;   // flow_id → iface_name
    mutex ifaces_mutex;

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

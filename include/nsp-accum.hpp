// netify-plugin-stats — interval accumulators
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nsp {

constexpr const char *OTHER_SERIES = "__other__";

struct Metrics {
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint32_t rx_pkts  = 0;
    uint32_t tx_pkts  = 0;
    uint32_t flows    = 0;
};

struct NamedMetrics {
    std::string name;
    Metrics m;
};

// Accumulates per-app and per-category metrics for one sample interval,
// with cumulative-counter delta tracking keyed by flow id.
class Accumulator {
public:
    // Cumulative byte/packet snapshot for one flow at one event.
    struct FlowSample {
        uint64_t flow_id;       // stable per-flow key
        unsigned app_id;
        std::string app_name;
        unsigned cat_id;
        std::string cat_name;
        uint64_t total_tx_bytes;   // cumulative (lower)
        uint64_t total_rx_bytes;   // cumulative (upper)
        uint32_t total_tx_pkts;
        uint32_t total_rx_pkts;
        bool is_new;               // first event for this flow this interval
        std::string iface_name;   // flow->iface->ifname
    };

    // Fold one flow event into the current interval (adds only the delta
    // vs. this flow's previously seen cumulative counters).
    void Observe(const FlowSample &s);

    // Mark a flow as gone so its delta state is dropped.
    void ForgetFlow(uint64_t flow_id);

    // Snapshot + clear the interval. Apps are ranked by total bytes; the top
    // `top_n` are returned by name and the remainder folded into "__other__".
    // Categories are returned in full (no top-N).
    struct Snapshot {
        std::vector<NamedMetrics> apps;   // includes __other__ if any remainder
        std::vector<NamedMetrics> cats;
    };
    Snapshot SampleAndReset(size_t top_n);

private:
    // Per-flow last-seen cumulative counters for delta computation.
    struct FlowState {
        uint64_t tx_bytes = 0, rx_bytes = 0;
        uint32_t tx_pkts = 0, rx_pkts = 0;
    };
    std::map<uint64_t, FlowState> flow_state_;

    struct Bucket { std::string name; Metrics m; };
    std::map<unsigned, Bucket> apps_;
    std::map<unsigned, Bucket> cats_;
};

} // namespace nsp

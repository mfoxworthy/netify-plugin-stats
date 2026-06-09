// netify-plugin-stats — core metrics types
// Accumulator class removed; byte accounting now done via conntrack polling.
#pragma once
#include <cstdint>
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

} // namespace nsp

#include "nsp-accum.hpp"

#include <algorithm>

namespace nsp {

void Accumulator::Observe(const FlowSample &s) {
    auto &fs = flow_state_[s.flow_id];
    // Compute deltas vs. last-seen cumulative counters (guard underflow).
    uint64_t dtx = s.total_tx_bytes >= fs.tx_bytes ? s.total_tx_bytes - fs.tx_bytes : s.total_tx_bytes;
    uint64_t drx = s.total_rx_bytes >= fs.rx_bytes ? s.total_rx_bytes - fs.rx_bytes : s.total_rx_bytes;
    uint32_t dtp = s.total_tx_pkts  >= fs.tx_pkts  ? s.total_tx_pkts  - fs.tx_pkts  : s.total_tx_pkts;
    uint32_t drp = s.total_rx_pkts  >= fs.rx_pkts  ? s.total_rx_pkts  - fs.rx_pkts  : s.total_rx_pkts;

    fs.tx_bytes = s.total_tx_bytes;
    fs.rx_bytes = s.total_rx_bytes;
    fs.tx_pkts  = s.total_tx_pkts;
    fs.rx_pkts  = s.total_rx_pkts;

    auto add = [&](std::map<unsigned, Bucket> &m, unsigned id, const std::string &name) {
        auto &b = m[id];
        if (b.name.empty()) b.name = name;
        b.m.tx_bytes += dtx;
        b.m.rx_bytes += drx;
        b.m.tx_pkts  += dtp;
        b.m.rx_pkts  += drp;
        if (s.is_new) b.m.flows += 1;
    };
    add(apps_, s.app_id, s.app_name);
    add(cats_, s.cat_id, s.cat_name);
}

void Accumulator::ForgetFlow(uint64_t flow_id) {
    flow_state_.erase(flow_id);
}

Accumulator::Snapshot Accumulator::SampleAndReset(size_t top_n) {
    Snapshot out;

    // Categories: full, in id order.
    for (auto &kv : cats_)
        out.cats.push_back(NamedMetrics{kv.second.name, kv.second.m});

    // Apps: rank by total bytes (tx+rx), keep top_n, fold rest into __other__.
    std::vector<NamedMetrics> ranked;
    ranked.reserve(apps_.size());
    for (auto &kv : apps_)
        ranked.push_back(NamedMetrics{kv.second.name, kv.second.m});
    std::sort(ranked.begin(), ranked.end(), [](const NamedMetrics &a, const NamedMetrics &b) {
        uint64_t ta = a.m.tx_bytes + a.m.rx_bytes, tb = b.m.tx_bytes + b.m.rx_bytes;
        return ta > tb;
    });
    Metrics other;
    bool have_other = false;
    for (size_t i = 0; i < ranked.size(); ++i) {
        if (i < top_n) { out.apps.push_back(ranked[i]); continue; }
        have_other = true;
        other.tx_bytes += ranked[i].m.tx_bytes;
        other.rx_bytes += ranked[i].m.rx_bytes;
        other.tx_pkts  += ranked[i].m.tx_pkts;
        other.rx_pkts  += ranked[i].m.rx_pkts;
        other.flows    += ranked[i].m.flows;
    }
    if (have_other) out.apps.push_back(NamedMetrics{OTHER_SERIES, other});

    // Clear interval buckets; keep flow_state_ for cross-interval deltas.
    apps_.clear();
    cats_.clear();
    return out;
}

} // namespace nsp

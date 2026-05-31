#include "nsp-query.hpp"

namespace nsp {

bool resolveTier(const std::vector<TierSpec> &tiers, const std::string &range, size_t &tier_out) {
    // Map a human range to the tier whose total span best covers it, by step.
    // Convention: tier index ascends with coarser step. "1h"->0,"1d"->1,"30d"->2.
    size_t want;
    if (range == "1h") want = 0;
    else if (range == "1d") want = 1;
    else if (range == "30d") want = 2;
    else return false;
    if (want >= tiers.size()) return false;
    tier_out = want;
    return true;
}

static uint64_t pick(const Cell &c, Metric m) {
    switch (m) {
        case Metric::RX_BYTES: return c.rx_bytes;
        case Metric::TX_BYTES: return c.tx_bytes;
        case Metric::PKTS:     return (uint64_t)c.rx_pkts + c.tx_pkts;
        case Metric::FLOWS:    return c.flows;
    }
    return 0;
}

nlohmann::json buildResponse(TierSet &ts, const Query &q) {
    nlohmann::json out;
    Store &s = ts.tier(q.tier);
    out["step"] = s.slot_duration();

    std::vector<std::string> names = q.keys.empty() ? s.series_names() : q.keys;

    // start = epoch of the oldest slot (first non-zero), else 0.
    int64_t start = 0;
    out["series"] = nlohmann::json::array();
    for (const auto &name : names) {
        std::vector<int64_t> ep; std::vector<Cell> cells;
        ts.ReadSeries(q.tier, name, ep, cells);
        if (start == 0)
            for (auto e : ep) if (e != 0) { start = e; break; }
        nlohmann::json vals = nlohmann::json::array();
        for (size_t i = 0; i < cells.size(); ++i) {
            if (ep[i] == 0) vals.push_back(nullptr);
            else vals.push_back(pick(cells[i], q.metric));
        }
        out["series"].push_back({ {"name", name}, {"values", std::move(vals)} });
    }
    out["start"] = start;
    return out;
}

} // namespace nsp

#include "nsp-query.hpp"

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

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

std::vector<std::string> listInterfaces(const std::string &store_path) {
    std::vector<std::string> result;
    DIR *d = opendir(store_path.c_str());
    if (!d) return result;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string subdir = store_path + "/" + e->d_name;
        struct stat st;
        if (stat(subdir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        DIR *sd = opendir(subdir.c_str());
        if (!sd) continue;
        bool has_rrb = false;
        struct dirent *se;
        while ((se = readdir(sd)) != nullptr) {
            std::string fname = se->d_name;
            if (fname.size() > 4 && fname.compare(fname.size() - 4, 4, ".rrb") == 0) {
                std::string fp = subdir + "/" + fname;
                struct stat fst;
                if (stat(fp.c_str(), &fst) == 0 && fst.st_size > 0) {
                    has_rrb = true;
                    break;
                }
            }
        }
        closedir(sd);
        if (has_rrb) result.push_back(e->d_name);
    }
    closedir(d);
    std::sort(result.begin(), result.end());
    return result;
}

nlohmann::json buildResponseMerged(const std::vector<TierSet*> &sets,
                                   const Query &q,
                                   size_t top_n) {
    nlohmann::json out;
    if (sets.empty()) {
        out["step"] = 10; out["start"] = 0;
        out["series"] = nlohmann::json::array();
        return out;
    }

    Store &ref    = sets[0]->tier(q.tier);
    uint32_t step = ref.slot_duration();
    size_t nslots = ref.slot_count();

    // merged[name][slot] = summed metric value across all interfaces
    std::map<std::string, std::vector<uint64_t>> merged;
    std::vector<bool> slot_active(nslots, false);
    int64_t start = 0;

    for (TierSet *ts : sets) {
        for (const auto &name : ts->tier(q.tier).series_names()) {
            if (name == std::string(OTHER_SERIES)) continue;
            std::vector<int64_t> ep;
            std::vector<Cell> cells;
            ts->ReadSeries(q.tier, name, ep, cells);
            auto &acc = merged[name];
            if (acc.empty()) acc.assign(nslots, 0);
            for (size_t i = 0; i < cells.size() && i < nslots; ++i) {
                if (ep[i] == 0) continue;
                slot_active[i] = true;
                if (start == 0) start = ep[i];
                acc[i] += pick(cells[i], q.metric);
            }
        }
    }

    // Sort by per-series total descending
    std::vector<std::pair<uint64_t, std::string>> totals;
    totals.reserve(merged.size());
    for (const auto &kv : merged) {
        uint64_t t = 0;
        for (auto v : kv.second) t += v;
        totals.push_back({t, kv.first});
    }
    std::sort(totals.rbegin(), totals.rend());

    out["step"]   = step;
    out["start"]  = start;
    out["series"] = nlohmann::json::array();

    std::vector<uint64_t> other_vals(nslots, 0);
    bool has_other = false;

    for (size_t i = 0; i < totals.size(); ++i) {
        const auto &vals = merged[totals[i].second];
        if (i < top_n) {
            nlohmann::json arr = nlohmann::json::array();
            for (size_t j = 0; j < nslots; ++j)
                arr.push_back(slot_active[j]
                              ? nlohmann::json((int64_t)vals[j])
                              : nlohmann::json(nullptr));
            out["series"].push_back({{"name", totals[i].second}, {"values", std::move(arr)}});
        } else {
            for (size_t j = 0; j < nslots; ++j)
                if (slot_active[j]) other_vals[j] += vals[j];
            has_other = true;
        }
    }

    if (has_other) {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t j = 0; j < nslots; ++j)
            arr.push_back(slot_active[j]
                          ? nlohmann::json((int64_t)other_vals[j])
                          : nlohmann::json(nullptr));
        out["series"].push_back({{"name", std::string(OTHER_SERIES)}, {"values", std::move(arr)}});
    }

    return out;
}

} // namespace nsp

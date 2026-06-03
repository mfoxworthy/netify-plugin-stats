// netify-plugin-stats — query/JSON builder
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nsp-config.hpp"
#include "nsp-store.hpp"

namespace nsp {

enum class Dim { APPS, CATS };
enum class Metric { RX_BYTES, TX_BYTES, PKTS, FLOWS };

struct Query {
    Dim dim = Dim::APPS;
    Metric metric = Metric::RX_BYTES;
    size_t tier = 0;                       // resolved from range string
    std::vector<std::string> keys;         // empty = all series in the tier
};

// Map a range string ("1h"/"1d"/"30d") to a tier index given the configured
// tiers (smallest step = tier 0). Returns false if no tier matches.
bool resolveTier(const std::vector<TierSpec> &tiers, const std::string &range, size_t &tier_out);

// Build the response JSON for `q` against an opened TierSet.
nlohmann::json buildResponse(TierSet &ts, const Query &q);

// Build a merged response from multiple TierSets (combined / all-interface mode).
// Sums values element-wise across all sets, re-applies top_n with __other__ folding.
nlohmann::json buildResponseMerged(const std::vector<TierSet*> &sets,
                                   const Query &q,
                                   size_t top_n);

// Scan store_path for subdirectories containing at least one non-empty .rrb file.
// Returns interface names in sorted order.
std::vector<std::string> listInterfaces(const std::string &store_path);

} // namespace nsp

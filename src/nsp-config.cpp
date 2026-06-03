#include "nsp-config.hpp"

#include <cstdlib>

namespace nsp {

static bool get1(const UciMap &m, const std::string &k, std::string &out) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return false;
    out = it->second.front();
    return true;
}

ConfigResult parseConfig(const UciMap &uci) {
    ConfigResult r;
    Config &c = r.config;

    std::string v;
    if (get1(uci, "global.store_path", v)) c.store_path = v;
    if (get1(uci, "global.categories_path", v)) c.categories_path = v;
    if (get1(uci, "global.sample_interval", v)) c.sample_interval = (unsigned)strtoul(v.c_str(), nullptr, 0);
    if (get1(uci, "global.top_n_apps", v)) c.top_n_apps = (unsigned)strtoul(v.c_str(), nullptr, 0);
    if (c.sample_interval == 0) { r.warnings.push_back("sample_interval=0 invalid; using 10"); c.sample_interval = 10; }

    // Capacity = top_n + __other__ + small headroom.
    c.series_capacity_apps = (uint32_t)c.top_n_apps + 1 + 8;

    // Collect indexed tier sections.
    std::vector<TierSpec> tiers;
    bool any_bad = false;
    for (size_t i = 0; ; ++i) {
        std::string sk = "tier." + std::to_string(i) + ".step";
        std::string ck = "tier." + std::to_string(i) + ".count";
        std::string sv, cv;
        if (!get1(uci, sk, sv) || !get1(uci, ck, cv)) break;
        uint32_t step = (uint32_t)strtoul(sv.c_str(), nullptr, 0);
        uint32_t cnt  = (uint32_t)strtoul(cv.c_str(), nullptr, 0);
        if (step == 0 || cnt == 0) {
            r.warnings.push_back("tier " + std::to_string(i) + " has zero step/count; dropped");
            any_bad = true;
            continue;
        }
        tiers.push_back(TierSpec{step, cnt});
    }
    if (!tiers.empty() && !any_bad) c.tiers = tiers;
    // If any tier was bad, keep defaults (conservative).

    // monitor_ifs: list option — all values in the vector
    auto mit = uci.find("global.monitor_if");
    if (mit != uci.end() && !mit->second.empty())
        c.monitor_ifs = mit->second;

    return r;
}

} // namespace nsp

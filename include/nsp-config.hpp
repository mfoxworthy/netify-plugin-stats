// netify-plugin-stats — configuration (UCI-backed)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nsp-store.hpp"   // TierSpec

namespace nsp {

struct Config {
    std::string store_path   = "/tmp/netify-stats";
    unsigned    sample_interval = 10;   // seconds
    unsigned    top_n_apps   = 50;
    uint32_t    series_capacity_apps = 64;   // top_n + __other__ + headroom
    uint32_t    series_capacity_cats = 64;
    std::vector<TierSpec> tiers = {
        {10, 360}, {60, 1440}, {300, 8640}
    };
};

// UCI is read as map<"section.option", vector<value>>, mirroring ndUci::result.
using UciMap = std::map<std::string, std::vector<std::string>>;

struct ConfigResult {
    Config config;
    std::vector<std::string> warnings;
    bool ok = true;
};

// Pure parser over an already-read UCI map (unit-testable, no agent deps).
ConfigResult parseConfig(const UciMap &uci);

// Live loader: reads package "netify-stats" via ndUci, then parseConfig().
// Implemented in nsp-config-uci.cpp (builds under the OpenWRT SDK).
ConfigResult loadConfig();

} // namespace nsp

// netify-stats-query — read the store, emit the JSON data contract.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nsp-config.hpp"
#include "nsp-query.hpp"

using json = nlohmann::json;
using namespace nsp;

static void emit_error(const std::string &msg) {
    json e; e["error"] = msg;
    printf("%s\n", e.dump().c_str());
}

int main(int argc, char **argv) {
    std::string dim = "apps", metric = "rx_bytes", range = "1h";
    std::string store_path_override;
    std::vector<std::string> keys;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char *p) -> std::string { return a.substr(strlen(p)); };
        if (a.rfind("--dimension=", 0) == 0) dim = val("--dimension=");
        else if (a.rfind("--metric=", 0) == 0) metric = val("--metric=");
        else if (a.rfind("--range=", 0) == 0) range = val("--range=");
        else if (a.rfind("--store-path=", 0) == 0) store_path_override = val("--store-path=");
        else if (a.rfind("--key=", 0) == 0) keys.push_back(val("--key="));
    }

    ConfigResult cr = loadConfig();
    Config &cfg = cr.config;
    if (!store_path_override.empty()) cfg.store_path = store_path_override;

    Query q;
    q.dim = (dim == "cats") ? Dim::CATS : Dim::APPS;
    if (metric == "tx_bytes") q.metric = Metric::TX_BYTES;
    else if (metric == "pkts") q.metric = Metric::PKTS;
    else if (metric == "flows") q.metric = Metric::FLOWS;
    else q.metric = Metric::RX_BYTES;
    q.keys = keys;

    if (!resolveTier(cfg.tiers, range, q.tier)) {
        emit_error("unknown range: " + range);
        return 1;
    }

    TierSet ts; std::string err;
    const char *dimname = (q.dim == Dim::CATS) ? "cats" : "apps";
    uint32_t cap = (q.dim == Dim::CATS) ? cfg.series_capacity_cats : cfg.series_capacity_apps;
    if (!ts.Open(cfg.store_path, dimname, cfg.tiers, cap, err)) {
        emit_error("store open failed: " + err);
        return 1;
    }

    json out = buildResponse(ts, q);
    printf("%s\n", out.dump().c_str());
    return 0;
}

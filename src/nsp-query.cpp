// netify-stats-query — read the store, emit the JSON data contract.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    std::string iface;
    bool list_ifaces = false, query_live = false, reset_live = false;
    std::vector<std::string> keys;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char *p) -> std::string { return a.substr(strlen(p)); };
        if      (a.rfind("--dimension=", 0)  == 0) dim                = val("--dimension=");
        else if (a.rfind("--metric=", 0)     == 0) metric             = val("--metric=");
        else if (a.rfind("--range=", 0)      == 0) range              = val("--range=");
        else if (a.rfind("--store-path=", 0) == 0) store_path_override = val("--store-path=");
        else if (a.rfind("--key=", 0)        == 0) keys.push_back(val("--key="));
        else if (a.rfind("--iface=", 0)      == 0) iface              = val("--iface=");
        else if (a == "--list-interfaces")          list_ifaces        = true;
        else if (a == "--query-live")               query_live         = true;
        else if (a == "--reset-live")               reset_live         = true;
    }

    if (!iface.empty() && iface.find('/') != std::string::npos) {
        emit_error("invalid interface name: " + iface);
        return 1;
    }

    ConfigResult cr = loadConfig();
    Config &cfg = cr.config;
    if (!store_path_override.empty()) cfg.store_path = store_path_override;

    // ── list_interfaces mode ──────────────────────────────────────────────────
    if (list_ifaces) {
        auto ifaces = listInterfaces(cfg.store_path);
        json j;
        j["interfaces"] = ifaces;
        printf("%s\n", j.dump().c_str());
        return 0;
    }

    // ── query-live mode ───────────────────────────────────────────────────────
    if (query_live) {
        std::string path = cfg.store_path + "/.live.json";
        std::ifstream lf(path);
        if (!lf.is_open()) {
            json empty;
            empty["start"] = 0; empty["duration"] = (int64_t)cfg.live_duration;
            empty["apps"] = json::array(); empty["cats"] = json::array();
            empty["hosts"] = json::array();
            printf("%s\n", empty.dump().c_str());
            return 0;
        }
        std::string content((std::istreambuf_iterator<char>(lf)),
                             std::istreambuf_iterator<char>());
        printf("%s\n", content.c_str());
        return 0;
    }

    // ── reset-live mode ───────────────────────────────────────────────────────
    if (reset_live) {
        std::string sentinel = cfg.store_path + "/.reset-live";
        int fd = ::open(sentinel.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) ::close(fd);
        json r; r["ok"] = true;
        printf("%s\n", r.dump().c_str());
        return 0;
    }

    // ── build query descriptor ────────────────────────────────────────────────
    Query q;
    q.dim    = (dim == "cats") ? Dim::CATS : Dim::APPS;
    q.metric = (metric == "tx_bytes") ? Metric::TX_BYTES
             : (metric == "pkts")     ? Metric::PKTS
             : (metric == "flows")    ? Metric::FLOWS
             :                         Metric::RX_BYTES;
    q.keys   = keys;

    if (!resolveTier(cfg.tiers, range, q.tier)) {
        emit_error("unknown range: " + range);
        return 1;
    }

    const char *dimname = (q.dim == Dim::CATS) ? "cats" : "apps";
    uint32_t    cap     = (q.dim == Dim::CATS) ? cfg.series_capacity_cats
                                               : cfg.series_capacity_apps;
    std::string err;

    // ── single-interface mode ─────────────────────────────────────────────────
    if (!iface.empty()) {
        std::string iface_dir = cfg.store_path + "/" + iface;
        struct stat st;
        if (stat(iface_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            emit_error("unknown interface: " + iface);
            return 1;
        }
        TierSet ts;
        if (!ts.Open(iface_dir, dimname, cfg.tiers, cap, err)) {
            emit_error("store open failed: " + err);
            return 1;
        }
        printf("%s\n", buildResponse(ts, q).dump().c_str());
        return 0;
    }

    // ── combined mode (no iface flag) ─────────────────────────────────────────
    auto ifaces = listInterfaces(cfg.store_path);
    if (ifaces.empty()) {
        json out;
        out["step"]   = (cfg.tiers.size() > q.tier) ? cfg.tiers[q.tier].step : 10;
        out["start"]  = 0;
        out["series"] = json::array();
        printf("%s\n", out.dump().c_str());
        return 0;
    }

    std::vector<std::unique_ptr<TierSet>> owned;
    std::vector<TierSet*> sets;
    for (const auto &f : ifaces) {
        auto ts = std::make_unique<TierSet>();
        std::string iface_dir = cfg.store_path + "/" + f;
        if (ts->Open(iface_dir, dimname, cfg.tiers, cap, err)) {
            sets.push_back(ts.get());
            owned.push_back(std::move(ts));
        }
    }
    if (sets.empty()) {
        emit_error("all interface stores failed to open; last error: " + err);
        return 1;
    }
    printf("%s\n", buildResponseMerged(sets, q, cfg.top_n_apps).dump().c_str());
    return 0;
}

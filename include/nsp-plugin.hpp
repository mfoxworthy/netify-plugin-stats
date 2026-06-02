// netify-plugin-stats — plugin class
#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>

#include <nd-plugin.h>
#include <nd-flow.h>

#include "nsp-config.hpp"
#include "nsp-accum.hpp"
#include "nsp-store.hpp"

constexpr unsigned _NSP_PLUGIN_VER = 0x20260531;

class nspPlugin : public ndPluginDetection {
public:
    nspPlugin(const std::string &tag);
    virtual ~nspPlugin();

    virtual void *Entry(void) override;
    virtual void ProcessEvent(ndPluginEvent event, void *param = NULL) override;
    virtual void ProcessFlow(ndDetectionEvent event, ndFlow *flow) override;

    virtual void GetVersion(std::string &version) override;

    // Non-virtual status/identity helpers (called by Entry for logging).
    void GetStatus(nlohmann::json &status);

protected:
    std::atomic<bool> reload_pending{true};

    nsp::Config config;
    std::mutex config_mutex;

    nsp::Accumulator accum;
    std::mutex accum_mutex;

    nsp::TierSet apps_store;
    nsp::TierSet cats_store;
    std::atomic<bool> store_ok{false};

    std::atomic<uint64_t> stat_events{0};
    std::atomic<uint64_t> stat_samples{0};
    std::atomic<uint64_t> stat_store_errors{0};
    std::atomic<uint64_t> stat_series_dropped{0};

    void Reload();
    void OpenStores();
    void LoadCategoryNames(const std::string &path);
    static uint64_t FlowKey(ndFlow *flow);
    std::string CategoryName(unsigned cat_id);

    std::unordered_map<unsigned, std::string> cat_names;
};

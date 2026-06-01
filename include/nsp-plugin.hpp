// netify-plugin-stats — plugin class
#pragma once

#include <atomic>
#include <mutex>

#include <nd-plugin.hpp>
#include <nd-flow.hpp>

#include "nsp-config.hpp"
#include "nsp-accum.hpp"
#include "nsp-store.hpp"

constexpr unsigned _NSP_PLUGIN_VER = 0x20260531;

class nspPlugin : public ndPluginProcessor {
public:
    nspPlugin(const std::string &tag, const ndPlugin::Params &params);
    virtual ~nspPlugin();

    virtual void *Entry(void) override;
    virtual void DispatchEvent(ndPlugin::Event event, void *param = nullptr) override;
    virtual void DispatchProcessorEvent(
        ndPluginProcessor::Event event, ndFlow::Ptr &flow) override;

    virtual void GetLibrary(std::string &library) override;
    virtual void GetName(std::string &name) override;
    virtual void GetVersion(std::string &version) override;
    virtual void GetStatus(nlohmann::json &status) override;

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
    static uint64_t FlowKey(const ndFlow::Ptr &flow);
    std::string CategoryName(unsigned cat_id);
};

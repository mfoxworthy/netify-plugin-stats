#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <chrono>
#include <ctime>
#include <thread>
#include <string>
#include <sys/stat.h>

#include <nd-util.hpp>

#include "nsp-plugin.hpp"

using namespace std;
using json = nlohmann::json;

// Recursive mkdir -p (the agent has no nd_mkdir helper).
static void nsp_mkdir_p(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur != "/" && !cur.empty()) ::mkdir(cur.c_str(), 0755);
        }
    }
}

nspPlugin::nspPlugin(const string &tag, const ndPlugin::Params &params)
    : ndPluginProcessor(tag, params) {
    auto i = params.find("conf_filename");
    if (i != params.end()) conf_filename = i->second;
    Reload();
}

nspPlugin::~nspPlugin() { Join(); }

void nspPlugin::Reload() {
    auto r = nsp::loadConfig();
    for (auto &w : r.warnings) nd_printf("%s: config warning: %s\n", tag.c_str(), w.c_str());
    {
        lock_guard<mutex> lg(config_mutex);
        config = std::move(r.config);
    }
    OpenStores();
    nd_printf("%s: loaded config; store_path=%s top_n=%u interval=%us\n",
        tag.c_str(), config.store_path.c_str(), config.top_n_apps, config.sample_interval);
}

void nspPlugin::OpenStores() {
    lock_guard<mutex> lg(config_mutex);
    nsp_mkdir_p(config.store_path);
    string err;
    store_ok =
        apps_store.Open(config.store_path, "apps", config.tiers, config.series_capacity_apps, err) &&
        cats_store.Open(config.store_path, "cats", config.tiers, config.series_capacity_cats, err);
    if (!store_ok) {
        stat_store_errors++;
        nd_printf("%s: store open failed: %s (continuing in-RAM only)\n", tag.c_str(), err.c_str());
    }
}

// Stable per-flow key: the flow's xxh64 lower digest (set by ndFlow::Hash()).
uint64_t nspPlugin::FlowKey(const ndFlow::Ptr &flow) {
    return (uint64_t)flow->digest_lower;
}

std::string nspPlugin::CategoryName(unsigned cat_id) {
    // v0.1: numeric fallback. Real category tags require capturing the
    // ndInstanceStatus->categories table at the UPDATE_INIT event and calling
    // ndCategories::GetTag(Type::APP, id); left as a future enhancement.
    return "cat-" + std::to_string(cat_id);
}

void nspPlugin::DispatchEvent(ndPlugin::Event event, void *) {
    if (event == ndPlugin::Event::RELOAD) reload_pending = true;
}

void nspPlugin::DispatchProcessorEvent(
    ndPluginProcessor::Event event, ndFlow::Ptr &flow) {
    if (event == ndPluginProcessor::Event::FLOW_EXPIRE) {
        lock_guard<mutex> lg(accum_mutex);
        accum.ForgetFlow(FlowKey(flow));
        return;
    }
    if (event != ndPluginProcessor::Event::DPI_UPDATE &&
        event != ndPluginProcessor::Event::DPI_COMPLETE)
        return;

    nsp::Accumulator::FlowSample s;
    s.flow_id  = FlowKey(flow);
    s.app_id   = (unsigned)flow->detected_application;
    s.app_name = flow->detected_application_name.empty()
        ? ("app-" + std::to_string(s.app_id)) : flow->detected_application_name;
    s.cat_id   = (unsigned)flow->category.application;
    s.cat_name = CategoryName(s.cat_id);
    // tx = lower (egress from local), rx = upper (ingress). Confirm orientation
    // on-device in Task 13; swap these two pairs if reversed.
    s.total_tx_bytes = flow->stats.total_lower_bytes.load();
    s.total_rx_bytes = flow->stats.total_upper_bytes.load();
    s.total_tx_pkts  = flow->stats.total_lower_packets.load();
    s.total_rx_pkts  = flow->stats.total_upper_packets.load();
    s.is_new = (event == ndPluginProcessor::Event::DPI_COMPLETE);

    {
        lock_guard<mutex> lg(accum_mutex);
        accum.Observe(s);
    }
    stat_events++;
}

void *nspPlugin::Entry(void) {
    using namespace std::chrono;
    auto next = steady_clock::now();
    while (!ShouldTerminate()) {
        if (reload_pending.exchange(false)) Reload();

        unsigned interval; size_t top_n;
        {
            lock_guard<mutex> lg(config_mutex);
            interval = config.sample_interval;
            top_n = config.top_n_apps;
        }
        next += seconds(interval);
        while (!ShouldTerminate() && steady_clock::now() < next)
            this_thread::sleep_for(milliseconds(200));
        if (ShouldTerminate()) break;

        nsp::Accumulator::Snapshot snap;
        {
            lock_guard<mutex> lg(accum_mutex);
            snap = accum.SampleAndReset(top_n);
        }
        int64_t epoch = (int64_t)time(nullptr);
        if (store_ok) {
            lock_guard<mutex> lg(config_mutex);
            stat_series_dropped += apps_store.AppendSample(epoch, snap.apps);
            stat_series_dropped += cats_store.AppendSample(epoch, snap.cats);
        }
        stat_samples++;
    }
    return nullptr;
}

void nspPlugin::GetLibrary(string &library) { library = PACKAGE; }
void nspPlugin::GetName(string &name) { name = PACKAGE_NAME; }
void nspPlugin::GetVersion(string &version) { version = PACKAGE_VERSION; }

void nspPlugin::GetStatus(json &status) {
    status["plugin_version"] = _NSP_PLUGIN_VER;
    status["events"] = stat_events.load();
    status["samples"] = stat_samples.load();
    status["store_errors"] = stat_store_errors.load();
    status["series_dropped"] = stat_series_dropped.load();
    lock_guard<mutex> lg(config_mutex);
    status["store_path"] = config.store_path;
    status["store_ok"] = store_ok.load();
}

ndPluginInit(nspPlugin);

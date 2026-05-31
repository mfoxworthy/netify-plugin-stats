#include "doctest.h"
#include "nsp-config.hpp"
using namespace nsp;

TEST_CASE("config: defaults when uci is empty") {
    ConfigResult r = parseConfig({});
    CHECK(r.ok);
    CHECK(r.config.store_path == "/tmp/netify-stats");
    CHECK(r.config.sample_interval == 10);
    CHECK(r.config.top_n_apps == 50);
    REQUIRE(r.config.tiers.size() == 3);
    CHECK(r.config.tiers[0].step == 10);
    CHECK(r.config.tiers[0].count == 360);
}

TEST_CASE("config: global options override defaults") {
    UciMap m;
    m["global.store_path"] = {"/srv/netify-stats"};
    m["global.sample_interval"] = {"30"};
    m["global.top_n_apps"] = {"10"};
    ConfigResult r = parseConfig(m);
    CHECK(r.config.store_path == "/srv/netify-stats");
    CHECK(r.config.sample_interval == 30);
    CHECK(r.config.top_n_apps == 10);
    CHECK(r.config.series_capacity_apps >= 11);
}

TEST_CASE("config: explicit tiers replace defaults in order") {
    UciMap m;
    m["tier.0.step"] = {"5"};   m["tier.0.count"] = {"100"};
    m["tier.1.step"] = {"60"};  m["tier.1.count"] = {"200"};
    ConfigResult r = parseConfig(m);
    REQUIRE(r.config.tiers.size() == 2);
    CHECK(r.config.tiers[0].step == 5);
    CHECK(r.config.tiers[0].count == 100);
    CHECK(r.config.tiers[1].step == 60);
    CHECK(r.config.tiers[1].count == 200);
}

TEST_CASE("config: rejects zero step/count with a warning, keeps default tiers") {
    UciMap m;
    m["tier.0.step"] = {"0"}; m["tier.0.count"] = {"100"};
    ConfigResult r = parseConfig(m);
    CHECK(!r.warnings.empty());
    REQUIRE(r.config.tiers.size() == 3);   // bad tier dropped -> fall back to defaults
}

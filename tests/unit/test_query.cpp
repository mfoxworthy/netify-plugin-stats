#include "doctest.h"
#include "nsp-query.hpp"
#include <unistd.h>
#include <sys/stat.h>
using namespace nsp;

TEST_CASE("query: builds aligned values with nulls for empty slots") {
    std::string dir = "/tmp/nsp_q_" + std::to_string(getpid());
    ::mkdir(dir.c_str(), 0755);
    TierSet ts; std::string err;
    std::vector<TierSpec> tiers = { {10, 4} };
    REQUIRE(ts.Open(dir, "apps", tiers, 8, err));
    std::vector<NamedMetrics> s = { { "netflix", Metrics{200, 100, 2, 1, 1} } };
    ts.AppendSample(1000, s);
    s = { { "netflix", Metrics{260, 150, 4, 2, 0} } };
    ts.AppendSample(1010, s);

    Query q; q.dim = Dim::APPS; q.metric = Metric::TX_BYTES; q.tier = 0;
    auto j = buildResponse(ts, q);
    CHECK(j["step"] == 10);
    REQUIRE(j["series"].size() == 1);
    CHECK(j["series"][0]["name"] == "netflix");
    auto vals = j["series"][0]["values"];
    REQUIRE(vals.size() == 4);
    CHECK(vals[0].is_null());        // empty slot
    CHECK(vals[2] == 100);
    CHECK(vals[3] == 150);
    ts.Close();
}

TEST_CASE("query: resolveTier maps range to step") {
    std::vector<TierSpec> tiers = { {10,360}, {60,1440}, {300,8640} };
    size_t t;
    REQUIRE(resolveTier(tiers, "1h", t)); CHECK(t == 0);
    REQUIRE(resolveTier(tiers, "1d", t)); CHECK(t == 1);
    REQUIRE(resolveTier(tiers, "30d", t)); CHECK(t == 2);
    CHECK(!resolveTier(tiers, "bogus", t));
}

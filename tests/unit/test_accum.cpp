#include "doctest.h"
#include "nsp-accum.hpp"
using namespace nsp;

static Accumulator::FlowSample mk(uint64_t fid, unsigned app, const char *an,
        unsigned cat, const char *cn, uint64_t tx, uint64_t rx,
        uint32_t txp, uint32_t rxp, bool is_new) {
    return Accumulator::FlowSample{fid, app, an, cat, cn, tx, rx, txp, rxp, is_new};
}

TEST_CASE("accum: tracks deltas against cumulative counters") {
    Accumulator a;
    a.Observe(mk(1, 10, "netflix", 2, "media", 100, 200, 1, 2, true));
    a.Observe(mk(1, 10, "netflix", 2, "media", 150, 260, 2, 4, false)); // +50tx,+60rx
    auto snap = a.SampleAndReset(50);
    REQUIRE(snap.apps.size() == 1);
    CHECK(snap.apps[0].name == "netflix");
    CHECK(snap.apps[0].m.tx_bytes == 150);  // 100 + 50
    CHECK(snap.apps[0].m.rx_bytes == 260);  // 200 + 60
    CHECK(snap.apps[0].m.tx_pkts == 2);
    CHECK(snap.apps[0].m.rx_pkts == 4);
    CHECK(snap.apps[0].m.flows == 1);       // one new flow
}

TEST_CASE("accum: SampleAndReset clears interval but keeps flow delta base") {
    Accumulator a;
    a.Observe(mk(1, 10, "netflix", 2, "media", 100, 0, 1, 0, true));
    a.SampleAndReset(50);
    a.Observe(mk(1, 10, "netflix", 2, "media", 130, 0, 2, 0, false)); // +30 only
    auto snap = a.SampleAndReset(50);
    REQUIRE(snap.apps.size() == 1);
    CHECK(snap.apps[0].m.tx_bytes == 30);
    CHECK(snap.apps[0].m.flows == 0);       // not new in 2nd interval
}

TEST_CASE("accum: top-N rolls remainder into __other__") {
    Accumulator a;
    a.Observe(mk(1, 1, "big",   0, "c", 1000, 0, 1, 0, true));
    a.Observe(mk(2, 2, "mid",   0, "c",  500, 0, 1, 0, true));
    a.Observe(mk(3, 3, "small", 0, "c",  100, 0, 1, 0, true));
    auto snap = a.SampleAndReset(2);
    REQUIRE(snap.apps.size() == 3);          // big, mid, __other__
    CHECK(snap.apps[0].name == "big");
    CHECK(snap.apps[1].name == "mid");
    CHECK(snap.apps[2].name == OTHER_SERIES);
    CHECK(snap.apps[2].m.tx_bytes == 100);   // small rolled into __other__
    CHECK(snap.apps[2].m.flows == 1);
}

TEST_CASE("accum: categories are kept in full (no top-N)") {
    Accumulator a;
    a.Observe(mk(1, 1, "a", 10, "media",   100, 0, 1, 0, true));
    a.Observe(mk(2, 2, "b", 11, "social",  200, 0, 1, 0, true));
    a.Observe(mk(3, 3, "c", 12, "gaming",   50, 0, 1, 0, true));
    auto snap = a.SampleAndReset(1);         // top_n=1 affects apps only
    CHECK(snap.cats.size() == 3);
}

TEST_CASE("accum: ForgetFlow drops delta base so reuse counts fresh") {
    Accumulator a;
    a.Observe(mk(1, 10, "x", 0, "c", 500, 0, 5, 0, true));
    a.SampleAndReset(50);
    a.ForgetFlow(1);
    a.Observe(mk(1, 10, "x", 0, "c", 80, 0, 1, 0, true)); // flow id reused, cumulative restarts at 80
    auto snap = a.SampleAndReset(50);
    REQUIRE(snap.apps.size() == 1);
    CHECK(snap.apps[0].m.tx_bytes == 80);    // full 80, not underflow
}

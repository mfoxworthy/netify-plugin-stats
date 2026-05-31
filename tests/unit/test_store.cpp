#include "doctest.h"
#include "nsp-store.hpp"
#include <sys/stat.h>

TEST_CASE("store: file_size is fixed and matches layout") {
    using namespace nsp;
    uint32_t cap = 51, slots = 360;
    size_t expected =
        sizeof(Header)
        + (size_t)cap * NSP_NAME_MAX
        + (size_t)slots * sizeof(SlotMeta)
        + (size_t)slots * cap * sizeof(Cell);
    CHECK(file_size(cap, slots) == expected);
    CHECK(sizeof(Cell) == 28);
}

TEST_CASE("store: cell_index is row-major by slot") {
    using namespace nsp;
    CHECK(cell_index(0, 0, 51) == 0);
    CHECK(cell_index(0, 5, 51) == 5);
    CHECK(cell_index(1, 0, 51) == 51);
    CHECK(cell_index(2, 3, 51) == 105);
}

#include <cstdio>
#include <unistd.h>

static std::string tmp_store_path(const char *tag) {
    std::string p = "/tmp/nsp_test_";
    p += tag;
    p += "_";
    p += std::to_string(getpid());
    p += ".rrb";
    ::unlink(p.c_str());
    return p;
}

TEST_CASE("store: append then read back aligned values") {
    using namespace nsp;
    auto path = tmp_store_path("rw");
    Store s; std::string err;
    REQUIRE(s.Open(path, 10, 4, 8, err));
    uint32_t idx;
    REQUIRE(s.SeriesIndex("netflix", idx));
    CHECK(idx == 0);

    std::vector<Cell> cells(8, Cell{});
    cells[0] = Cell{ /*rx*/200, /*tx*/100, 2, 1, 1 };
    s.Append(1000, cells);
    cells[0] = Cell{ 260, 150, 4, 2, 0 };
    s.Append(1010, cells);

    std::vector<int64_t> ep; std::vector<Cell> out;
    s.ReadSeries("netflix", ep, out);
    REQUIRE(out.size() == 4);                 // slot_count
    // Newest two slots populated, oldest two empty (epoch 0).
    CHECK(ep[2] == 1000);
    CHECK(ep[3] == 1010);
    CHECK(out[2].rx_bytes == 200);
    CHECK(out[3].tx_bytes == 150);
    CHECK(ep[0] == 0);
    CHECK(out[0].rx_bytes == 0);
    s.Close();
    ::unlink(path.c_str());
}

TEST_CASE("store: ring wraps and overwrites oldest") {
    using namespace nsp;
    auto path = tmp_store_path("wrap");
    Store s; std::string err;
    REQUIRE(s.Open(path, 10, 3, 4, err));     // 3 slots
    uint32_t idx; REQUIRE(s.SeriesIndex("a", idx));
    for (int k = 0; k < 5; ++k) {             // write 5 into 3 slots
        std::vector<Cell> c(4, Cell{});
        c[0] = Cell{ (uint64_t)(k+1)*10, 0, 0, 0, 0 };
        s.Append(1000 + k*10, c);
    }
    std::vector<int64_t> ep; std::vector<Cell> out;
    s.ReadSeries("a", ep, out);
    REQUIRE(out.size() == 3);
    // Oldest two (k=0,1) overwritten; remaining oldest->newest = k=2,3,4.
    CHECK(out[0].rx_bytes == 30);
    CHECK(out[1].rx_bytes == 40);
    CHECK(out[2].rx_bytes == 50);
    CHECK(ep[2] == 1000 + 4*10);
    s.Close();
    ::unlink(path.c_str());
}

TEST_CASE("store: geometry mismatch recreates and discards data") {
    using namespace nsp;
    auto path = tmp_store_path("geo");
    { Store s; std::string err;
      REQUIRE(s.Open(path, 10, 4, 8, err));
      uint32_t idx; s.SeriesIndex("a", idx);
      std::vector<Cell> c(8, Cell{}); c[0] = Cell{99,0,0,0,0};
      s.Append(1000, c); s.Close(); }
    // Reopen with different slot_count -> must recreate.
    Store s2; std::string err;
    REQUIRE(s2.Open(path, 10, 8, 8, err));     // slot_count 4 -> 8
    CHECK(s2.slot_count() == 8);
    std::vector<int64_t> ep; std::vector<Cell> out;
    s2.ReadSeries("a", ep, out);
    // "a" was discarded; series table empty -> all zero / no series.
    CHECK(out.size() == 8);
    CHECK(out[7].rx_bytes == 0);
    s2.Close();
    ::unlink(path.c_str());
}

TEST_CASE("store: appends in the same slot window consolidate in place") {
    using namespace nsp;
    auto path = tmp_store_path("cons");
    Store s; std::string err;
    REQUIRE(s.Open(path, 100, 4, 4, err));   // 100s slots
    uint32_t idx; REQUIRE(s.SeriesIndex("a", idx));

    std::vector<Cell> c(4, Cell{});
    c[0] = Cell{100, 50, 1, 1, 1};
    s.Append(1000, c);                       // window 1000/100 = 10
    c[0] = Cell{200, 80, 3, 2, 1};
    s.Append(1050, c);                       // same window (1050/100 = 10) -> consolidate

    std::vector<int64_t> ep; std::vector<Cell> out;
    s.ReadSeries("a", ep, out);
    REQUIRE(out.size() == 4);
    // Exactly one slot populated; it holds the SUM of both writes.
    int populated = 0; size_t pi = 0;
    for (size_t i = 0; i < ep.size(); ++i) if (ep[i] != 0) { populated++; pi = i; }
    CHECK(populated == 1);
    CHECK(out[pi].rx_bytes == 300);          // 100 + 200
    CHECK(out[pi].tx_bytes == 130);          // 50 + 80
    CHECK(out[pi].rx_pkts == 4);             // 1 + 3
    CHECK(out[pi].flows == 2);               // 1 + 1
    CHECK(ep[pi] == 1000);                   // stored epoch = slot-window floor
    s.Close();
    ::unlink(path.c_str());
}

TEST_CASE("tierset: tier1 consolidates multiple tier0 samples") {
    using namespace nsp;
    std::string dir = "/tmp/nsp_tierset_" + std::to_string(getpid());
    ::mkdir(dir.c_str(), 0755);
    TierSet ts; std::string err;
    std::vector<TierSpec> tiers = { {10, 6}, {30, 4} };  // t0=10s, t1=30s
    REQUIRE(ts.Open(dir, "apps", tiers, 4, err));

    auto sample = [&](int64_t epoch, uint64_t tx) {
        std::vector<NamedMetrics> s = { { "netflix", Metrics{0, tx, 0, 0, 1} } };
        ts.AppendSample(epoch, s);
    };
    sample(1000, 100);   // t1 window [990,1020) since 1000/30*30=990
    sample(1010, 100);   // same t1 window
    sample(1020, 100);   // new t1 window [1020,1050)

    std::vector<int64_t> ep; std::vector<Cell> out;
    ts.ReadSeries(1, "netflix", ep, out);   // tier 1
    // Two t1 slots written: first sums two samples (200, 2 flows), second 100.
    uint64_t total = 0; uint32_t flows = 0;
    for (size_t i = 0; i < out.size(); ++i) { total += out[i].tx_bytes; flows += out[i].flows; }
    CHECK(total == 300);
    CHECK(flows == 3);
    // The consolidated slot holds the summed value.
    bool saw_200 = false;
    for (auto &c : out) if (c.tx_bytes == 200) saw_200 = true;
    CHECK(saw_200);
    ts.Close();
}

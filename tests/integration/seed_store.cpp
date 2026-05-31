// Seeds a store dir with known apps samples for the integration test.
#include <cstdio>
#include <string>
#include <vector>
#include "nsp-store.hpp"
using namespace nsp;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: seed_store <dir>\n"); return 2; }
    std::string dir = argv[1];
    std::vector<TierSpec> tiers = { {10, 360}, {60, 1440}, {300, 8640} };
    TierSet ts; std::string err;
    if (!ts.Open(dir, "apps", tiers, 64, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::vector<NamedMetrics> s = {
        { "netflix", Metrics{ 5000, 1000, 50, 10, 1 } },
        { "youtube", Metrics{ 3000, 800, 30, 8, 1 } },
    };
    ts.AppendSample(1748707200, s);
    ts.Close();
    return 0;
}

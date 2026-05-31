// Reads the seeded store and prints the JSON query response (no UCI/flags).
#include <cstdio>
#include <string>
#include <vector>
#include "nsp-store.hpp"
#include "nsp-query.hpp"
using namespace nsp;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: query_driver <dir>\n"); return 2; }
    std::string dir = argv[1];
    std::vector<TierSpec> tiers = { {10, 360}, {60, 1440}, {300, 8640} };
    TierSet ts; std::string err;
    if (!ts.Open(dir, "apps", tiers, 64, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    Query q; q.dim = Dim::APPS; q.metric = Metric::RX_BYTES; q.tier = 0;
    auto j = buildResponse(ts, q);
    printf("%s\n", j.dump().c_str());
    return 0;
}

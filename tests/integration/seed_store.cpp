// Seeds per-interface store subdirs for integration tests.
// Creates $DIR/br-lan/ with netflix/youtube and $DIR/wan/ with zoom/youtube.
#include <cstdio>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "nsp-store.hpp"
using namespace nsp;

static bool seed(const std::string &dir, const std::vector<NamedMetrics> &series) {
    ::mkdir(dir.c_str(), 0755);
    std::vector<TierSpec> tiers = { {10, 360}, {60, 1440}, {300, 8640} };
    TierSet ts; std::string err;
    if (!ts.Open(dir, "apps", tiers, 64, err)) {
        fprintf(stderr, "seed: open failed for %s: %s\n", dir.c_str(), err.c_str());
        return false;
    }
    ts.AppendSample(1748707200, series);
    ts.Close();
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: seed_store <dir>\n"); return 2; }
    std::string root = argv[1];

    bool ok =
        seed(root + "/br-lan", {
            { "netflix", Metrics{ 5000, 1000, 50, 10, 1 } },
            { "youtube", Metrics{ 3000,  800, 30,  8, 1 } },
        }) &&
        seed(root + "/wan", {
            { "zoom",    Metrics{ 4100,  900, 40,  9, 1 } },
            { "youtube", Metrics{ 1000,  200, 10,  2, 1 } },
        });
    return ok ? 0 : 1;
}

// loadConfig(): reads UCI package "netify-stats" via the agent's ndUci and
// delegates to the pure parseConfig(). Built under the OpenWRT SDK (needs
// <uci.h> via <nd-uci.hpp>), not in the native unit-test build.
#include "nsp-config.hpp"

#include <nd-uci.hpp>

namespace nsp {

ConfigResult loadConfig() {
    UciMap m;
    ndUci uci("netify-stats");
    ndUci::result res;
    if (uci.Get("netify-stats", res)) {
        for (auto &kv : res) m[kv.first] = kv.second;
    }
    // NOTE (verify on the OpenWRT target): confirm the exact key shape ndUci::Get
    // returns for package-wide and anonymous `tier` sections, and that it keys by
    // "section.option" / "tier.<i>.step". Adjust this mapping if the live shape
    // differs; the pure parseConfig() contract (keys global.* and tier.<i>.*) is
    // what the unit tests pin.
    return parseConfig(m);
}

} // namespace nsp

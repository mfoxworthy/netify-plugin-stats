// loadConfig(): read the UCI package "netify-stats" and delegate to the pure
// parseConfig(). libnetifyd does NOT export its internal ndUci helper, so we
// use the libuci C API directly (the package DEPENDS on libuci). Built only
// under --enable-package (needs <uci.h>); the native unit-test build compiles
// the pure parseConfig() in nsp-config.cpp instead.
#include "nsp-config.hpp"

extern "C" {
#include <uci.h>
}

namespace nsp {

ConfigResult loadConfig() {
    UciMap m;

    struct uci_context *ctx = uci_alloc_context();
    if (ctx) {
        struct uci_package *pkg = nullptr;
        if (uci_load(ctx, "netify-stats", &pkg) == UCI_OK && pkg) {
            size_t tier_idx = 0;
            struct uci_element *se;
            uci_foreach_element(&pkg->sections, se) {
                struct uci_section *s = uci_to_section(se);
                // Named sections (e.g. 'global') key by their name; anonymous
                // 'tier' sections key by their order: tier.<i>.<option>.
                std::string prefix;
                if (s->type && std::string(s->type) == "tier")
                    prefix = "tier." + std::to_string(tier_idx++) + ".";
                else
                    prefix = std::string(s->e.name ? s->e.name : "") + ".";

                struct uci_element *oe;
                uci_foreach_element(&s->options, oe) {
                    struct uci_option *o = uci_to_option(oe);
                    if (o->type == UCI_TYPE_STRING && o->v.string)
                        m[prefix + o->e.name] = { std::string(o->v.string) };
                }
            }
        }
        uci_free_context(ctx);
    }

    return parseConfig(m);
}

} // namespace nsp

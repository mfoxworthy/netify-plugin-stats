AUTOMAKE_OPTIONS = foreign subdir-objects

AM_CPPFLAGS = -D_GNU_SOURCE \
    -I$(top_srcdir)/include \
    $(LIBNETIFYD_CFLAGS)

# The plugin .so, query CLI, and shared core link the ndUci loader (needs
# <uci.h>) and only build under --enable-package (target/SDK). The native
# unit-test build compiles the pure sources directly, so src/ builds nothing.
if BUILD_PACKAGE

# Shared core: store, accumulator, query builder, config (pure parser +
# the ndUci loader). Compiled into both the plugin .so and the query CLI so
# the on-disk binary format can never drift between writer and reader.
noinst_LTLIBRARIES = libnspcore.la
libnspcore_la_SOURCES = \
    nsp-store.cpp \
    nsp-query-build.cpp \
    nsp-config.cpp \
    nsp-config-uci.cpp
# nsp-config-uci.cpp reads UCI via libuci directly (libnetifyd does not export
# its internal ndUci), so the core lib and both consumers link -luci.
libnspcore_la_LIBADD = $(LIBNETIFYD_LIBS) -luci

# The Netify Agent processor plugin (loaded by netifyd from /usr/lib).
lib_LTLIBRARIES = netify-plugin-stats.la
netify_plugin_stats_la_SOURCES = nsp-plugin.cpp
netify_plugin_stats_la_LDFLAGS = -module -avoid-version -shared
netify_plugin_stats_la_LIBADD = libnspcore.la $(LIBNETIFYD_LIBS) -luci

# The query CLI (invoked by the rpcd backend).
bin_PROGRAMS = netify-stats-query
netify_stats_query_SOURCES = nsp-query.cpp
netify_stats_query_LDADD = libnspcore.la $(LIBNETIFYD_LIBS) -luci

endif # BUILD_PACKAGE

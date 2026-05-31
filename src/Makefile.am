AUTOMAKE_OPTIONS = foreign subdir-objects

AM_CPPFLAGS = -D_GNU_SOURCE \
    -I$(top_srcdir)/include \
    $(LIBNETIFYD_CFLAGS)

# Targets are added incrementally as tasks create their sources:
#   - libnspcore.la  (store, accum, query builder, config)   [Tasks 3-8]
#   - netify-plugin-stats.la  (the plugin .so)                [Task 9]
#   - netify-stats-query      (the query CLI)                 [Task 10]

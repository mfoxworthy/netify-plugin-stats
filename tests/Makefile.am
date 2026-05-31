AUTOMAKE_OPTIONS = foreign subdir-objects

AM_CPPFLAGS = -D_GNU_SOURCE \
    -DNSP_FIXTURE_DIR=\"$(abs_top_srcdir)/tests/unit/fixtures\" \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/tests \
    $(LIBNETIFYD_CFLAGS)

check_PROGRAMS = unit_tests

# Test and core sources are added incrementally per task. Task 2 starts with
# the doctest main + smoke test only.
unit_tests_SOURCES = \
    unit/test_smoke.cpp \
    unit/test_store.cpp \
    unit/test_accum.cpp \
    unit/test_config.cpp \
    unit/test_query.cpp \
    $(top_srcdir)/src/nsp-store.cpp \
    $(top_srcdir)/src/nsp-accum.cpp \
    $(top_srcdir)/src/nsp-config.cpp \
    $(top_srcdir)/src/nsp-query-build.cpp

unit_tests_LDADD = $(LIBNETIFYD_LIBS)

TESTS = unit_tests

AC_PREREQ([2.69])
AC_INIT([netify-plugin-stats],
    m4_esyscmd_s([cat VERSION]),
    [support@example.com])
AC_CONFIG_AUX_DIR([automake])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([foreign subdir-objects 1.13])
AC_CONFIG_HEADERS([config.h])

AC_PROG_CXX
AC_PROG_CC
LT_INIT([disable-static])

AX_CXX_COMPILE_STDCXX([17], [noext], [mandatory])

PKG_CHECK_MODULES([LIBNETIFYD], [libnetifyd])

AC_ARG_ENABLE([tests],
    [AS_HELP_STRING([--disable-tests], [do not build unit tests])],
    [enable_tests=$enableval], [enable_tests=yes])
AM_CONDITIONAL([ENABLE_TESTS], [test "x$enable_tests" = "xyes"])

# The plugin .so, query CLI, and shared core library link against the agent's
# ndUci (which needs <uci.h>) and only build in a full target/SDK environment.
# Default off so the native unit-test build (which compiles the pure sources
# directly) does not require uci.h. The OpenWRT package passes --enable-package.
AC_ARG_ENABLE([package],
    [AS_HELP_STRING([--enable-package], [build the plugin .so and query CLI (needs libuci)])],
    [enable_package=$enableval], [enable_package=no])
AM_CONDITIONAL([BUILD_PACKAGE], [test "x$enable_package" = "xyes"])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT

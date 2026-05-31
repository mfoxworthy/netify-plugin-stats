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

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT

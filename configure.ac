AC_INIT(
    [prime_server],
    [m4_esyscmd([./version.sh])],
    [https://github.com/kevinkreiser/prime_server/issues],
    [prime_server-m4_esyscmd([./version.sh])],
    [https://github.com/kevinkreiser/prime_server])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([-Wall -Werror subdir-objects parallel-tests])
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
LT_INIT
AC_SUBST([LIBTOOL_DEPS])

AM_SILENT_RULES([yes])
AC_CONFIG_HEADERS([prime_server/config.h])
AC_CONFIG_MACRO_DIR([m4])

# set pkgconfigdir, allow override
AC_ARG_WITH(	[pkgconfigdir],
		AS_HELP_STRING([--with-pkgconfigdir=PATH], [Path to the pkgconfig directory [[LIBDIR/pkgconfig]]]),
		[pkgconfigdir="$withval"],
		[pkgconfigdir='${libdir}/pkgconfig'])
AC_SUBST([pkgconfigdir])

AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_MAKE_SET

AC_CHECK_INCLUDES_DEFAULT
AC_PROG_EGREP

AC_LANG([C++])

# require c++11
AX_CXX_COMPILE_STDCXX_11([noext],[mandatory])

# check zmq version
PKG_CHECK_MODULES([DEPS], [libzmq >= 4.1.4 libczmq >= 3.0 libcurl >= 7.22.0])

# require pthread as a regular dep because we need it everywhere
AX_PTHREAD(, [AC_MSG_ERROR([cannot find libpthread])])
DEPS_CFLAGS="${DEPS_CFLAGS} ${PTHREAD_CFLAGS}"
DEPS_LIBS="${DEPS_LIBS} ${PTHREAD_LIBS}"

# optionally enable coverage information
CHECK_COVERAGE

AC_CONFIG_FILES([Makefile libprime_server.pc])

# Debian resets this to no, but this breaks both Spot and the libtool
# test suite itself.  Instead of requiring developer to install a
# non-patched version of Libtool on any Debian they use, we just
# cancel the effect of Debian's patch here.
# see: http://git.lrde.epita.fr/?p=spot.git;a=commitdiff;h=0e74b76521341f670f6b76f8ef24a6dcf6e3813b
link_all_deplibs=yes
link_all_deplibs_CXX=yes

AC_OUTPUT


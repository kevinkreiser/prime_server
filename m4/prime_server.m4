AC_DEFUN([CHECK_PRIME_SERVER],
[
	AC_ARG_WITH([prime_server],
		[AS_HELP_STRING([--with-prime_server@<:@=ARG@:>@],
			[use the system prime_server (ARG=yes), or from a specified location (ARG=<path>).])],
	[
		if test "$withval" = "yes"; then
			PRIME_SERVER_CPPFLAGS=""
			PRIME_SERVER_LDFLAGS=""
		else
			PRIME_SERVER_CPPFLAGS="-I$withval/include -I$withval"
			PRIME_SERVER_LDFLAGS="-L$withval/lib"
		fi
	],
	[
		PRIME_SERVER_CPPFLAGS=""
		PRIME_SERVER_LDFLAGS=""
	])

	CPPFLAGS_SAVED="$CPPFLAGS"
	CPPFLAGS="$CPPFLAGS $PRIME_SERVER_CPPFLAGS"
	export CPPFLAGS
	LDFLAGS_SAVED="$LDFLAGS"
	LDFLAGS="$LDFLAGS $PRIME_SERVER_LDFLAGS"
	export LDFLAGS

	AC_REQUIRE([AC_PROG_CC])

        AC_CACHE_CHECK(whether the prime_server library is available, ax_cv_prime_server,
        	[AC_LANG_PUSH([C++])
		AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[@%:@include <prime_server/prime_server.hpp>]],
			[[zmq::context_t c;
			 prime_server::proxy_t(c,"ipc://up","ipc://down");]])],
			ax_cv_prime_server=yes, ax_cv_prime_server=no)
		AC_LANG_POP([C++])
	])

	if test "x$ax_cv_prime_server" = "xyes"; then
		AC_DEFINE(HAVE_PRIME_SERVER,,[define if the prime_server library is available])
	else
		AC_MSG_ERROR(Could not find prime_server!)
	fi

	AC_CHECK_LIB(prime_server, exit, [PRIME_SERVER_LIB="-lprime_server"; AC_SUBST(PRIME_SERVER_LIB) link_prime_server="yes"; break], [link_prime_server="no"])

	if test "x$link_prime_server" = "xyes"; then
		AC_SUBST(PRIME_SERVER_CPPFLAGS)
		AC_SUBST(PRIME_SERVER_LDFLAGS)
		PRIME_SERVER_LIBS="-lprime_server"
		AC_SUBST(PRIME_SERVER_LIBS)
		CPPFLAGS="$CPPFLAGS_SAVED"
		export CPPFLAGS
		LDFLAGS="$LDFLAGS_SAVED"
		export LDFLAGS
	else
		AC_MSG_ERROR(Could not link against prime_server!)
	fi
])

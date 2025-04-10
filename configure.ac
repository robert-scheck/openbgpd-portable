#
# Copyright (c) 2019 Brent Cook
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

AC_INIT([OpenBGPD], m4_esyscmd([tr -d '\n' < VERSION]))

AC_CANONICAL_HOST
AM_INIT_AUTOMAKE([subdir-objects foreign])
AC_CONFIG_MACRO_DIR([m4])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AC_PROG_CC([cc gcc])

case $host_os in
	*linux*)
		AC_USE_SYSTEM_EXTENSIONS
		;;
	*) ;;
esac

case $host_os in
	*darwin*)
		HOST_OS=darwin
		AC_DEFINE([SPT_TYPE], [SPT_REUSEARGV])
		#
		# Don't use arc4random on systems before 10.12 because of
		# weak seed on failure to open /dev/random, based on latest
		# public source:
		# http://www.opensource.apple.com/source/Libc/Libc-997.90.3/gen/FreeBSD/arc4random.c
		#
		# We use the presence of getentropy() to detect 10.12. The
		# following check take into account that:
		#
		#   - iOS <= 10.1 fails because of missing getentropy and
		#     hence they miss sys/random.h
		#
		#   - in macOS 10.12 getentropy is not tagged as introduced in
		#     10.12 so we cannot use it for target < 10.12
		#
		AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <AvailabilityMacros.h>
#include <unistd.h>
#include <sys/random.h>  /* Systems without getentropy() should die here */

/* Based on: https://gitweb.torproject.org/tor.git/commit/?id=16fcbd21 */
#ifndef MAC_OS_X_VERSION_10_12
#  define MAC_OS_X_VERSION_10_12 101200
#endif
#if defined(MAC_OS_X_VERSION_MIN_REQUIRED)
#  if MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_12
#    error "Running on Mac OSX 10.11 or earlier"
#  endif
#endif
                       ]], [[
char buf[1]; getentropy(buf, 1);
					   ]])],
                       [ USE_BUILTIN_ARC4RANDOM=no ],
                       [ USE_BUILTIN_ARC4RANDOM=yes ]
		)
		AC_MSG_CHECKING([whether to use builtin arc4random])
		AC_MSG_RESULT([$USE_BUILTIN_ARC4RANDOM])
		;;
	*freebsd*)
		HOST_OS=freebsd
		AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/param.h>
#if __FreeBSD_version < 1200000
        undefined
#endif
                       ]], [[]])],
                       [ USE_BUILTIN_ARC4RANDOM=no ],
                       [ USE_BUILTIN_ARC4RANDOM=yes ]
		)
		;;
	*linux*)
		HOST_OS=linux
		AM_CFLAGS="-D_DEFAULT_SOURCE -D_BSD_SOURCE -D_GNU_SOURCE"
		AC_DEFINE([SPT_TYPE], [SPT_REUSEARGV])
		;;
	*netbsd*)
		HOST_OS=netbsd
		AM_CFLAGS="-D_OPENBSD_SOURCE"
		AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/param.h>
#if __NetBSD_Version__ < 700000001
        undefined
#endif
                       ]], [[]])],
                       [ USE_BUILTIN_ARC4RANDOM=no ],
                       [ USE_BUILTIN_ARC4RANDOM=yes ]
		)
		;;
	*openbsd*)
		HOST_OS=openbsd
		AC_DEFINE([HAVE_ATTRIBUTE__BOUNDED__], [1], [OpenBSD has __bounded__])
		AC_DEFINE([HAVE_ATTRIBUTE__DEAD], [1], [OpenBSD has __dead])
		AC_DEFINE([HAVE_ATTRIBUTE__PACKED], [1], [OpenBSD has __packed])
		;;
	*solaris*)
		HOST_OS=solaris
		AM_CFLAGS="-D__EXTENSIONS__ -D_XOPEN_SOURCE=600 -DBSD_COMP"
		;;
	*) ;;
esac

AM_CONDITIONAL([HOST_DARWIN],  [test x$HOST_OS = xdarwin])
AM_CONDITIONAL([HOST_FREEBSD], [test x$HOST_OS = xfreebsd])
AM_CONDITIONAL([HOST_LINUX],   [test x$HOST_OS = xlinux])
AM_CONDITIONAL([HOST_NETBSD],  [test x$HOST_OS = xnetbsd])
AM_CONDITIONAL([HOST_OPENBSD], [test x$HOST_OS = xopenbsd])
AM_CONDITIONAL([HOST_SOLARIS], [test x$HOST_OS = xsolaris])

AC_PROG_SED
AC_PROG_CC
AM_PROG_CC_C_O
LT_INIT
AC_PROG_YACC

AC_ARG_ENABLE(bgplgd,
	AS_HELP_STRING([--disable-bgplgd],
		[ build bgplgd [default=enabled]]),
	[case $enableval in
		yes) enable_bgplgd=yes;;
		no) enable_bgplgd=no;;
		*) enable_bgplgd=yes;; esac],
	enable_bgplgd=yes)

AC_ARG_ENABLE(fib-support,
	AS_HELP_STRING([--disable-fib-support],
		[ build without fib support [default=enabled]]),
	[case $enableval in
		yes) disable_fib=no;;
		no) disable_fib=yes;;
		*) disable_fib=no;; esac],
	disable_fib=no)

AC_ARG_ENABLE(warnings,
	AS_HELP_STRING([--disable-warnings],
		[ enable compiler warnings [default=enabled]]),
	[case $enableval in
		yes) enable_warnings=yes;;
		no) enable_warnings=no;;
		*) enable_warnings=yes;; esac],
	enable_warnings=yes)

if test "$enable_warnings" = yes; then
	AM_CFLAGS="$AM_CFLAGS -Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual -Wsign-compare"

	save_cflags="$CFLAGS"
	CFLAGS=-Wno-pointer-sign
	AC_MSG_CHECKING([whether CC supports -Wno-pointer-sign])
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([])],
		[AC_MSG_RESULT([yes])]
		[WARN_CFLAGS=-Wno-pointer-sign],
		[AC_MSG_RESULT([no])]
	)
	AM_CFLAGS="$AM_CFLAGS $WARN_CFLAGS"
	CFLAGS="$save_cflags"
fi

AC_MSG_CHECKING([if compiling with clang])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [[
#ifndef __clang__
	not clang
#endif
	]])],
	[AC_MSG_RESULT([yes])]
	[CLANG_FLAGS=-Qunused-arguments],
	[AC_MSG_RESULT([no])]
)
AM_CFLAGS="$AM_CFLAGS $CLANG_FLAGS"
AM_LDFLAGS="$AM_LDFLAGS $CLANG_FLAGS"

# check for headers
AC_CHECK_HEADERS([endian.h])
AC_CHECK_HEADERS([netinet/ip_ipsp.h], [], [], [[#include <sys/socket.h>]])
AC_CHECK_HEADERS([linux/in6.h])
AC_CHECK_HEADERS([linux/if.h])

# check functions that are expected to be in libc
AC_CHECK_FUNCS([asprintf explicit_bzero])
AC_CHECK_FUNCS([reallocarray recallocarray freezero])
AC_CHECK_FUNCS([setproctitle setgroups])
AC_CHECK_FUNCS([setregid setresgid setreuid setresuid])
AC_CHECK_FUNCS([strlcat strlcpy strtonum sysconf])
AC_CHECK_FUNCS([pledge unveil])
AC_CHECK_FUNCS([getdtablecount getrtable])
AC_CHECK_FUNCS([strnvis])
AC_CHECK_FUNCS([arc4random])

AC_CACHE_CHECK([for getentropy], ac_cv_func_getentropy, [
	AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <sys/types.h>
#include <unistd.h>

/*
 * Explanation:
 *
 *   - iOS <= 10.1 fails because of missing sys/random.h
 *
 *   - in macOS 10.12 getentropy is not tagged as introduced in
 *     10.12 so we cannot use it for target < 10.12
 */
#ifdef __APPLE__
#  include <AvailabilityMacros.h>
#  include <TargetConditionals.h>

# if (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
#  include <sys/random.h> /* Not available as of iOS <= 10.1 */
# else

#  include <sys/random.h> /* Pre 10.12 systems should die here */

/* Based on: https://gitweb.torproject.org/tor.git/commit/?id=16fcbd21 */
#  ifndef MAC_OS_X_VERSION_10_12
#    define MAC_OS_X_VERSION_10_12 101200 /* Robustness */
#  endif
#  if defined(MAC_OS_X_VERSION_MIN_REQUIRED)
#    if MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_12
#      error "Targeting on Mac OSX 10.11 or earlier"
#    endif
#  endif

# endif
#endif /* __APPLE__ */
		]], [[
	char buffer;
	(void)getentropy(&buffer, sizeof (buffer));
]])],
	[ ac_cv_func_getentropy="yes" ],
	[ ac_cv_func_getentropy="no"
	])
])

# check auxiliary libraries that might contain other functions
AC_SEARCH_LIBS([clock_gettime],[rt posix4])
AC_SEARCH_LIBS([inet_net_pton],[resolv])
AC_CHECK_FUNCS([clock_gettime inet_net_pton])

# check needed libutil functions
AC_SEARCH_LIBS([ibuf_open], [util])
AC_SEARCH_LIBS([fmt_scaled], [util])
AC_CHECK_FUNCS([ibuf_open imsgbuf_init fmt_scaled])

# check if HOST_NAME_MAX is available
AC_MSG_CHECKING([for HOST_NAME_MAX])
AC_COMPILE_IFELSE(
 [AC_LANG_PROGRAM([[
#include <limits.h>
#include <unistd.h>
]], [[
  char buf[HOST_NAME_MAX + 1];
]])],
 [AC_MSG_RESULT([yes])
  AC_DEFINE(HAVE_HOST_NAME_MAX) ],
 [AC_MSG_RESULT([no])]
)

# check if pftable code can be used
AC_CHECK_HEADERS([net/pfvar.h], [
 AC_MSG_CHECKING([for working pftable])
 AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
 #include <sys/socket.h>
 #include <net/if.h>
 #include <sys/ioctl.h>
 #include <net/pfvar.h>
 ]], [[
   struct pfioc_table tio;
   struct pfr_astats dummy;
   struct pfr_addr pfa;
   int a = DIOCRCLRADDRS, b = DIOCRGETASTATS,
       c = DIOCRDELADDRS, d = DIOCRADDADDRS;
 ]])],
  [AC_MSG_RESULT([yes])
   ac_cv_have_pftable="yes" ],
  [AC_MSG_RESULT([no])]
 )], [], [[
#include <sys/socket.h>
#include <net/if.h>
]])

dnl NetBSD added an strnvis and unfortunately made it incompatible with the
dnl existing one in OpenBSD and Linux's libbsd (the former having existed
dnl for over ten years). Despite this incompatibility being reported during
dnl development (see http://gnats.netbsd.org/44977) they still shipped it.
dnl Even more unfortunately FreeBSD and later MacOS picked up this incompatible
dnl implementation.  Try to detect this mess, and assume the only safe option
dnl if we're cross compiling.
dnl
dnl OpenBSD, 2001: strnvis(char *dst, const char *src, size_t dlen, int flag);
dnl NetBSD: 2012,  strnvis(char *dst, size_t dlen, const char *src, int flag);
if test "x$ac_cv_func_strnvis" = "xyes"; then
 AC_MSG_CHECKING([for working strnvis])
 AC_RUN_IFELSE(
  [AC_LANG_PROGRAM([[
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vis.h>
static void sighandler(int sig) { _exit(1); }
  ]], [[
 char dst[16];

 signal(SIGSEGV, sighandler);
 if (strnvis(dst, "src", 4, 0) && strcmp(dst, "src") == 0)
  exit(0);
 exit(1)
  ]])],
  [AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])
   ac_cv_broken_strnvis="yes" ],
  [AC_MSG_WARN([cross compiling: assuming broken])
   ac_cv_broken_strnvis="yes" ]
 )
fi

if test "$disable_fib" = no; then
	# check for libmnl
	AC_CHECK_LIB([mnl], [mnl_socket_recvfrom], [], [])
	if test "x$ac_cv_lib_mnl_mnl_socket_recvfrom" = "xyes"; then
		AC_CHECK_FUNCS([mnl_socket_open2], [],
			[AC_MSG_ERROR([libmnl >= 1.0.4 required])])
	fi
fi

# Share test results with automake
AM_CONDITIONAL([HAVE_ASPRINTF], [test "x$ac_cv_func_asprintf" = xyes])
AM_CONDITIONAL([HAVE_CLOSEFROM], [test "x$ac_cv_func_closefrom" = xyes])
AM_CONDITIONAL([HAVE_CLOCK_GETRES], [test "x$ac_cv_func_clock_getres" = xyes])
AM_CONDITIONAL([HAVE_CLOCK_GETTIME], [test "x$ac_cv_func_clock_gettime" = xyes])
AM_CONDITIONAL([HAVE_EXPLICIT_BZERO], [test "x$ac_cv_func_explicit_bzero" = xyes])
AM_CONDITIONAL([HAVE_FREEZERO], [test "x$ac_cv_func_freezero" = xyes])
AM_CONDITIONAL([HAVE_GETENTROPY], [test "x$ac_cv_func_getentropy" = xyes])
AM_CONDITIONAL([HAVE_IMSG], [test "x$ac_cv_func_ibuf_open" = xyes -a "x$ac_cv_func_imsgbuf_init" = xyes])
AM_CONDITIONAL([HAVE_MD5], [test "x$ac_cv_func_MD5Init" = xyes])
AM_CONDITIONAL([HAVE_MEMMEM], [test "x$ac_cv_func_memmem" = xyes])
AM_CONDITIONAL([HAVE_POLL], [test "x$ac_cv_func_poll" = xyes])
AM_CONDITIONAL([HAVE_REALLOCARRAY], [test "x$ac_cv_func_reallocarray" = xyes])
AM_CONDITIONAL([HAVE_RECALLOCARRAY], [test "x$ac_cv_func_recallocarray" = xyes])
AM_CONDITIONAL([HAVE_SETGROUPS], [test "x$ac_cv_func_setgroups" = xyes])
AM_CONDITIONAL([HAVE_SETRESGID], [test "x$ac_cv_func_setresgid" = xyes])
AM_CONDITIONAL([HAVE_SETRESUID], [test "x$ac_cv_func_setresuid" = xyes])
AM_CONDITIONAL([HAVE_SETPROCTITLE], [test "x$ac_cv_func_setproctitle" = xyes])
AM_CONDITIONAL([HAVE_STRLCAT], [test "x$ac_cv_func_strlcat" = xyes])
AM_CONDITIONAL([HAVE_STRLCPY], [test "x$ac_cv_func_strlcpy" = xyes])
AM_CONDITIONAL([HAVE_STRTONUM], [test "x$ac_cv_func_strtonum" = xyes])
AM_CONDITIONAL([HAVE_SYSCONF], [test "x$ac_cv_func_sysconf" = xyes])
AM_CONDITIONAL([HAVE_PLEDGE], [test "x$ac_cv_func_pledge" = xyes])
AM_CONDITIONAL([HAVE_UNVEIL], [test "x$ac_cv_func_unveil" = xyes])
AM_CONDITIONAL([HAVE_GETDTABLECOUNT], [test "x$ac_cv_func_getdtablecount" = xyes])
AM_CONDITIONAL([HAVE_GETRTABLE], [test "x$ac_cv_func_getrtable" = xyes])
AM_CONDITIONAL([HAVE_STRNVIS], [test "x$ac_cv_func_strnvis" = xyes])
AM_CONDITIONAL([BROKEN_STRNVIS], [test "x$ac_cv_broken_strnvis" = xyes])
AM_CONDITIONAL([HAVE_INET_NET_PTON], [test "x$ac_cv_func_inet_net_pton" = xyes])
AM_CONDITIONAL([HAVE_FMT_SCALED], [test "x$ac_cv_func_fmt_scaled" = xyes])
AM_CONDITIONAL([HAVE_MNL], [test "x$ac_cv_func_mnl_socket_open2" = "xyes"])
AM_CONDITIONAL([HAVE_PFTABLE], [test "x$ac_cv_have_pftable" = "xyes"])

# overrides for arc4random implementations with known issues
AM_CONDITIONAL([HAVE_ARC4RANDOM],
	[test "x$USE_BUILTIN_ARC4RANDOM" != xyes \
	   -a "x$ac_cv_func_arc4random" = xyes])

AC_CHECK_MEMBERS([struct tcp_md5sig.tcpm_key], , ,
	[ #include <sys/socket.h>
	  #include <netinet/in.h>
	  #include <netinet/tcp.h> ]
)
AM_CONDITIONAL([HAVE_LINUX_TCPMD5],
	[test "x$ac_cv_member_struct_tcp_md5sig_tcpm_key" = xyes])

AC_CHECK_FUNCS([setfib])
AM_CONDITIONAL([HAVE_SETFIB], [test "x$HOST_OS" = xfreebsd \
	    -a "x$ac_cv_func_setfib" = "xyes"])


if test "$enable_bgplgd" = yes; then
	AC_ARG_WITH([event],
		AS_HELP_STRING([--with-event=pkg-name],
			[Use pkg-config(1) pkg-name to find libevent]),
		PKG_NAME="$withval"
	)
	if test X"$PKG_NAME" != X; then
		LIBEVENT_CFLAGS=`pkg-config --cflags-only-I $PKG_NAME 2>/dev/null`
		LIBEVENT_LDFLAGS=`pkg-config --libs-only-L $PKG_NAME 2>/dev/null`
	fi

	AC_ARG_WITH([event-cflags],
		AS_HELP_STRING([--with-event-cflags=STRING],
			[Extra compiler flags to build with libevent]),
		LIBEVENT_CFLAGS="$withval"
	)
	AC_ARG_WITH([event-ldflags],
		AS_HELP_STRING([--with-event-ldflags=STRING],
			[Extra flags for linker to link with libevent]),
		LIBEVENT_LDFLAGS="$withval"
	)

	AM_CFLAGS="$AM_CFLAGS $LIBEVENT_CFLAGS"
	AM_LDFLAGS="$AM_LDFLAGS $LIBEVENT_LDFLAGS"

	save_cflags="$CFLAGS"
	save_ldflags="$LDFLAGS"
	CFLAGS="$AM_CFLAGS $CFLAGS"
	LDFLAGS="$AM_LDFLAGS $LDFLAGS"

	AC_CHECK_HEADERS([event.h], [],
		[AC_MSG_ERROR([libevent headers required])])
	AC_CHECK_LIB([event], [event_init], [],
		[AC_MSG_ERROR([libevent required])])
	AC_CHECK_FUNCS([event_init event_dispatch], [],
		[AC_MSG_ERROR([libevent required])])

	CFLAGS="$save_cflags"
	LDFLAGS="$save_ldflags"
fi

AC_ARG_WITH([privsep-user],
	AS_HELP_STRING([--with-privsep-user=user],
		[Privilege separation user for bgpd]),
	BGPD_USER="$withval",
	BGPD_USER="_bgpd"
)
AC_DEFINE_UNQUOTED(BGPD_USER, "$BGPD_USER", [Unprivileged user])
AC_SUBST(BGPD_USER)

if test "$enable_bgplgd" = yes; then
	AC_ARG_WITH([bgplgd-user],
		AS_HELP_STRING([--with-bgplgd-user=user],
			[Privilege drop user for bgplgd]),
		BGPLGD_USER="$withval",
		BGPLGD_USER="_bgplgd"
	)
	AC_DEFINE_UNQUOTED(BGPLGD_USER, "$BGPLGD_USER",
		[bgplgd unprivileged user])
	AC_SUBST(BGPLGD_USER)

	AC_ARG_WITH([www-user],
		AS_HELP_STRING([--with-www-user=user],
			[Owner of the FastCGI socket used by bgplgd]),
		WWW_USER="$withval",
		WWW_USER="www"
	)
	AC_DEFINE_UNQUOTED(WWW_USER, "$WWW_USER",
		[www unprivileged user])
	AC_SUBST(WWW_USER)

	AC_ARG_WITH([wwwrunstatedir],
		AS_HELP_STRING([--with-wwwrunstatedir=DIR],
			[Location for bgplgd FastCGI socket [LOCALSTATEDIR/www/run]]),
		wwwrunstatedir="$withval"
	)
	if test "x$runstatedir" = x; then
		wwwrunstatedir='${localstatedir}/www/run'
		AC_SUBST(wwwrunstatedir)
fi
fi

AM_CONDITIONAL([BUILD_BGPLGD], [test "$enable_bgplgd" = yes])

AM_CONDITIONAL([DISABLE_FIB], [test "$disable_fib" = yes])

# workaround the issue that there is no autoconf release supporting
# runstatedir but many linux distros patched their versions instead
# Check if the variable is set, if not use a basic default.
AC_ARG_WITH([runstatedir],
	AS_HELP_STRING([--with-runstatedir=DIR],
		[Location for bgpd control socket [LOCALSTATEDIR/run]]),
	runstatedir="$withval"
)
if test "x$runstatedir" = x; then
	runstatedir='${localstatedir}/run'
	AC_SUBST(runstatedir)
fi

AC_SUBST(AM_CFLAGS)
AC_SUBST(AM_LDFLAGS)

AC_CONFIG_FILES([
	Makefile
	include/Makefile
	compat/Makefile
	src/bgpctl/Makefile
	src/bgpd/Makefile
	src/bgplgd/Makefile
])

AC_OUTPUT

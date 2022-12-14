#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

m4_define([lxc_version_major], 1)
m4_define([lxc_version_minor], 0)
m4_define([lxc_version_micro], 0)
m4_define([lxc_version_beta], [alpha2])

m4_define([lxc_version_base], [lxc_version_major.lxc_version_minor.lxc_version_micro])
m4_define([lxc_version],
	  [ifelse(lxc_version_beta, [], [lxc_version_base], [lxc_version_base.lxc_version_beta])])

AC_INIT([lxc], [lxc_version])
AC_SUBST(LXC_VERSION_BASE, lxc_version_base)
AC_SUBST(LXC_VERSION_BETA, lxc_version_beta)

AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_AUX_DIR([config])
AC_CONFIG_HEADERS([src/config.h])
AM_INIT_AUTOMAKE([-Wall -Werror -Wno-portability subdir-objects])
AC_CANONICAL_HOST
AM_PROG_CC_C_O
AC_GNU_SOURCE

# Detect the distribution. This is used for the default configuration and
# for some distro-specific build options.
AC_MSG_CHECKING([host distribution])
AC_ARG_WITH(distro, AS_HELP_STRING([--with-distro=DISTRO], [Specify the Linux distribution to target: One of redhat, oracle, fedora, suse, gentoo, debian, arch, slackware, paldo, openmandriva or pardus.]))
if test "z$with_distro" = "z"; then
	with_distro=`lsb_release -is`
fi
if test "z$with_distro" = "z"; then
	AC_CHECK_FILE(/etc/redhat-release,with_distro="redhat")
	AC_CHECK_FILE(/etc/oracle-release,with_distro="oracle")
	AC_CHECK_FILE(/etc/fedora-release,with_distro="fedora")
	AC_CHECK_FILE(/etc/SuSE-release,with_distro="suse")
	AC_CHECK_FILE(/etc/gentoo-release,with_distro="gentoo")
	AC_CHECK_FILE(/etc/debian_version,with_distro="debian")
	AC_CHECK_FILE(/etc/arch-release,with_distro="arch")
	AC_CHECK_FILE(/etc/slackware-version,with_distro="slackware")
	AC_CHECK_FILE(/etc/frugalware-release,with_distro="frugalware")
	AC_CHECK_FILE(/etc/mandrakelinux-release, with_distro="openmandriva")
	AC_CHECK_FILE(/etc/mandriva-release,with_distro="openmandriva")
	AC_CHECK_FILE(/etc/pardus-release,with_distro="pardus")
fi
with_distro=`echo ${with_distro} | tr '[[:upper:]]' '[[:lower:]]'`

if test "z$with_distro" = "z"; then
	with_distro="unknown"
fi
case $with_distro in
	ubuntu)
		distroconf=default.conf.ubuntu
		;;
	redhat|fedora|oracle|oracleserver)
		distroconf=default.conf.libvirt
		;;
	*)
		echo -n "Linux distribution network config unknown, defaulting to lxc.network.type = empty"
		distroconf=default.conf.unknown
		;;
esac
AC_MSG_RESULT([$with_distro])
AM_CONDITIONAL([HAVE_DEBIAN], [test x"$with_distro" = "xdebian" -o x"$with_distro" = "xubuntu"])
AM_CONDITIONAL([DISTRO_UBUNTU], [test "x$with_distro" = "xubuntu"])

# Detect the newuidmap tool (required for userns)
AC_CHECK_PROG([NEWUIDMAP], [newuidmap], [newuidmap])
AM_CONDITIONAL([HAVE_NEWUIDMAP], [test -n "$NEWUIDMAP"])

# Allow disabling rpath
AC_ARG_ENABLE([rpath],
	[AC_HELP_STRING([--disable-rpath], [do not set rpath in executables])],
	[], [enable_rpath=yes])
AM_CONDITIONAL([ENABLE_RPATH], [test "x$enable_rpath" = "xyes"])

# Documentation (manpages)
AC_ARG_ENABLE([doc],
	[AC_HELP_STRING([--enable-doc], [make mans (requires docbook2man or docbook2x-man to be installed) [default=auto]])],
	[], [enable_doc=auto])

if test "x$enable_doc" = "xyes" -o "x$enable_doc" = "xauto"; then
	db2xman=""
	dbparsers="docbook2x-man db2x_docbook2man docbook2man"

	AC_MSG_CHECKING(for docbook2x-man)
	for name in ${dbparsers}; do
		if "$name" --help >/dev/null 2>&1; then
			db2xman="$name"
			break;
		fi
	done

	if test -n "${db2xman}"; then
		AC_MSG_RESULT([${db2xman}])
	else
		AC_MSG_RESULT([no])
		if test "x$enable_doc" = "xyes"; then
			AC_MSG_ERROR([docbook2x-man required by man request, but not found])
		fi
	fi

	AC_SUBST(db2xman)
fi
AM_CONDITIONAL([ENABLE_DOCBOOK], [test "x$db2xman" != "x"])
AM_CONDITIONAL([USE_DOCBOOK2X], [test "x$db2xman" != "xdocbook2man"])

if test "x$db2xman" = "xdocbook2man"; then
	docdtd="\"-//Davenport//DTD DocBook V3.0//EN\""
else
	docdtd="\"-//OASIS//DTD DocBook XML\" \"http://www.oasis-open.org/docbook/xml/4.5/docbookx.dtd\""
fi
AC_SUBST(docdtd)

# Apparmor
AC_ARG_ENABLE([apparmor],
	[AC_HELP_STRING([--enable-apparmor], [enable apparmor])],
	[], [enable_apparmor=check])

if test "$enable_apparmor" = "check" ; then
	AC_CHECK_LIB([apparmor],[aa_change_profile],[enable_apparmor=yes], [enable_apparmor=no])
fi
AM_CONDITIONAL([ENABLE_APPARMOR], [test "x$enable_apparmor" = "xyes"])

AC_CHECK_LIB([gnutls], [gnutls_hash_fast])

AM_COND_IF([ENABLE_APPARMOR],
	[AC_CHECK_HEADER([sys/apparmor.h],[],[AC_MSG_ERROR([You must install the AppArmor development package in order to compile lxc])])
	AC_CHECK_LIB([apparmor], [aa_change_profile],[],[AC_MSG_ERROR([You must install the AppArmor development package in order to compile lxc])])
	AC_SUBST([APPARMOR_LIBS], [-lapparmor])])

# SELinux
AC_ARG_ENABLE([selinux],
	[AC_HELP_STRING([--enable-selinux], [enable SELinux support])],
	[], [enable_selinux=check])

if test "x$enable_selinux" = xcheck; then
	AC_CHECK_LIB([selinux],[setexeccon_raw],[enable_selinux=yes],[enable_selinux=no])
fi
AM_CONDITIONAL([ENABLE_SELINUX], [test "x$enable_selinux" = "xyes"])
AM_COND_IF([ENABLE_SELINUX],
	[AC_CHECK_HEADER([selinux/selinux.h],[],[AC_MSG_ERROR([You must install the SELinux development package in order to compile lxc])])
	AC_CHECK_LIB([selinux], [setexeccon_raw],[],[AC_MSG_ERROR([You must install the SELinux development package in order to compile lxc])])
	AC_SUBST([SELINUX_LIBS])])

# Seccomp syscall filter
AC_ARG_ENABLE([seccomp],
	[AC_HELP_STRING([--enable-seccomp], [enable seccomp])],
	[], [enable_seccomp=check])

if test "$enable_seccomp" = "check" ; then
	AC_CHECK_LIB([seccomp],[seccomp_init],[enable_seccomp=yes],[enable_seccomp=no])
fi
AM_CONDITIONAL([ENABLE_SECCOMP], [test "x$enable_seccomp" = "xyes"])

AM_COND_IF([ENABLE_SECCOMP],
	[AC_CHECK_HEADER([seccomp.h],[],[AC_MSG_ERROR([You must install the seccomp development package in order to compile lxc])])
	AC_CHECK_LIB([seccomp], [seccomp_init],[],[AC_MSG_ERROR([You must install the seccomp development package in order to compile lxc])])
	AC_SUBST([SECCOMP_LIBS], [-lseccomp])])

# HAVE_SCMP_FILTER_CTX=1 will tell us we have libseccomp api >= 1.0.0
AC_CHECK_TYPES([scmp_filter_ctx], [], [], [#include <seccomp.h>])

# Configuration examples
AC_ARG_ENABLE([examples],
	[AC_HELP_STRING([--disable-examples], [do not install configuration examples])],
	[], [enable_examples=yes])
AM_CONDITIONAL([ENABLE_EXAMPLES], [test "x$enable_examples" = "xyes"])

# Python3 module and scripts
AC_ARG_ENABLE([python],
	[AC_HELP_STRING([--enable-python], [enable python binding])],
	[enable_python=yes], [enable_python=no])
AM_CONDITIONAL([ENABLE_PYTHON], [test "x$enable_python" = "xyes"])

AM_COND_IF([ENABLE_PYTHON],
	[AM_PATH_PYTHON([3.2], [], [AC_MSG_ERROR([You must install python3])])
	PKG_CHECK_MODULES([PYTHONDEV], [python3 >= 3.2],[],[AC_MSG_ERROR([You must install python3-dev])])
	AC_DEFINE_UNQUOTED([ENABLE_PYTHON], 1, [Python3 is available])])

# Not in older autoconf versions
# AS_VAR_COPY(DEST, SOURCE)
# -------------------------
# Set the polymorphic shell variable DEST to the contents of the polymorphic
# shell variable SOURCE.
m4_ifdef([AS_VAR_COPY], [],
[AC_DEFUN([AS_VAR_COPY],
    [AS_LITERAL_IF([$1[]$2], [$1=$$2], [eval $1=\$$2])])
])

dnl PKG_CHECK_VAR was introduced with pkg-config 0.28
m4_ifdef([PKG_CHECK_VAR], [],
[AC_DEFUN([PKG_CHECK_VAR],
    [AC_REQUIRE([PKG_PROG_PKG_CONFIG])dnl
     AC_ARG_VAR([$1], [value of $3 for $2, overriding pkg-config])dnl
     _PKG_CONFIG([$1], [variable="][$3]["], [$2])
     AS_VAR_COPY([$1], [pkg_cv_][$1])
     AS_VAR_IF([$1], [""], [$5], [$4])dnl
    ])# PKG_CHECK_VAR
])

# Lua module and scripts
AC_ARG_ENABLE([lua],
	[AC_HELP_STRING([--enable-lua], [enable lua binding])],
	[], [enable_lua=check])

AC_ARG_WITH([lua-pc],
	[AS_HELP_STRING(
		[--with-lua-pc=PKG],
		[Specify pkg-config package name for lua]
	)], [], [with_lua_pc=no])

if test "x$enable_lua" = "xyes" -a "x$with_lua_pc" != "xno"; then
	# exit with error if not found
	PKG_CHECK_MODULES([LUA], [$with_lua_pc], [LUAPKGCONFIG=$with_lua_pc])
fi

if test "x$enable_lua" = "xcheck" -a "x$with_lua_pc" != "xno"; then
	PKG_CHECK_MODULES([LUA], [$with_lua_pc],
		[LUAPKGCONFIG=$with_lua_pc
		enable_lua=yes],
		[enable_lua=no])
fi

if test "x$enable_lua" != "xno"; then
	PKG_CHECK_MODULES([LUA], [lua], [LUAPKGCONFIG=lua],
		[PKG_CHECK_MODULES([LUA], [lua5.2], [LUAPKGCONFIG=lua5.2],
			[PKG_CHECK_MODULES([LUA], [lua5.1], [LUAPKGCONFIG=lua5.1],
				[AS_IF([test "x$enable_lua" = "xyes"],
					[AC_MSG_ERROR([Lua not found. Please use --with-lua-pc=PKG])],
					[enable_lua=no])]
			)]
		)])
	AS_IF([test "x$LUAPKGCONFIG" != "x"], [enable_lua=yes])
fi

AM_CONDITIONAL([ENABLE_LUA],
	[test "x$enable_lua" = "xyes"])

AM_COND_IF([ENABLE_LUA],
	[AC_MSG_CHECKING([Lua version])
	PKG_CHECK_VAR([LUA_VERSION], [$LUAPKGCONFIG], [V],,
		[PKG_CHECK_VAR([LUA_VERSION], [$LUAPKGCONFIG], [major_version])])
	AC_MSG_RESULT([$LUA_VERSION])
	PKG_CHECK_VAR([LUA_INSTALL_CMOD], [$LUAPKGCONFIG], [INSTALL_CMOD],,
		[LUA_INSTALL_CMOD=$libdir/lua/$LUA_VERSION])
	PKG_CHECK_VAR([LUA_INSTALL_LMOD], [$LUAPKGCONFIG], [INSTALL_LMOD],,
		[LUA_INSTALL_LMOD=$datadir/lua/$LUA_VERSION])
	])

# Optional test binaries
AC_ARG_ENABLE([tests],
	[AC_HELP_STRING([--enable-tests], [build test/example binaries])],
	[enable_tests=yes], [enable_tests=no])
AM_CONDITIONAL([ENABLE_TESTS], [test "x$enable_tests" = "xyes"])

# LXC container path, where the containers are actually stored
# This is overridden by an entry in the file called LXCCONF
# (i.e. /etc/lxc/lxc.conf)
AC_ARG_WITH([config-path],
	[AC_HELP_STRING(
		[--with-config-path=dir],
		[lxc configuration repository path]
	)], [], [with_config_path=['${localstatedir}/lib/lxc']])

# The path of the global lxc configuration file.
AC_ARG_WITH([global-conf],
	[AC_HELP_STRING(
		[--with-global-conf=dir],
		[global lxc configuration file]
	)], [], [with_global_conf=['${sysconfdir}/lxc/lxc.conf']])

# The path of the userns network configuration file
AC_ARG_WITH([usernic-conf],
	[AC_HELP_STRING(
		[--with-usernic-conf],
		[user network interface configuration file]
	)], [], [with_usernic_conf=['${sysconfdir}/lxc/lxc-usernet']])

# The path of the runtime usernic database
AC_ARG_WITH([usernic-db],
	[AC_HELP_STRING(
		[--with-usernic-db],
		[lxc user nic database]
	)], [], [with_usernic_db=['/run/lxc/nics']])

# Rootfs path, where the container mount structure is assembled
AC_ARG_WITH([rootfs-path],
	[AC_HELP_STRING(
		[--with-rootfs-path=dir],
		[lxc rootfs mount point]
	)], [], [with_rootfs_path=['${libdir}/lxc/rootfs']])

# cgroup pattern specification
AC_ARG_WITH([cgroup-pattern],
	[AC_HELP_STRING(
		[--with-cgroup-pattern=pattern],
		[pattern for container cgroups]
	)], [], [with_cgroup_pattern=['/lxc/%n']])

# Container log path.  By default, use $lxcpath.
AC_MSG_CHECKING([Whether to place logfiles in container config path])
AC_ARG_ENABLE([configpath-log],
	[AC_HELP_STRING([--enable-configpath-log], [use logfiles in config path])],
	[use_configpath_logs=yes], [use_configpath_logs=no])
AC_MSG_RESULT([$use_configpath_logs])
AM_CONDITIONAL([USE_CONFIGPATH_LOGS], [test "$use_configpath_logs" = "yes"])

if test "$use_configpath_logs" = "yes"; then
	default_log_path="${with_config_path}"
else
	default_log_path="${localstatedir}/log/lxc"
fi

AC_ARG_WITH([log-path],
	[AC_HELP_STRING(
		[--with-log-path=dir],
		[per container log path]
	)], [], [with_log_path=['${default_log_path}']])

# Expand some useful variables
AS_AC_EXPAND(PREFIX, "$prefix")
AS_AC_EXPAND(LIBDIR, "$libdir")
AS_AC_EXPAND(BINDIR, "$bindir")
AS_AC_EXPAND(LIBEXECDIR, "$libexecdir")
AS_AC_EXPAND(INCLUDEDIR, "$includedir")
AS_AC_EXPAND(SYSCONFDIR, "$sysconfdir")
AS_AC_EXPAND(LXC_DEFAULT_CONFIG, "$sysconfdir/lxc/default.conf")
AS_AC_EXPAND(DATADIR, "$datadir")
AS_AC_EXPAND(LOCALSTATEDIR, "$localstatedir")
AS_AC_EXPAND(DOCDIR, "$docdir")
AS_AC_EXPAND(LXC_DISTRO_CONF, "$distroconf")
AS_AC_EXPAND(LXC_GENERATE_DATE, "$(date)")
AS_AC_EXPAND(LXCPATH, "$with_config_path")
AS_AC_EXPAND(LXC_GLOBAL_CONF, "$with_global_conf")
AS_AC_EXPAND(LXC_USERNIC_CONF, "$with_usernic_conf")
AS_AC_EXPAND(LXC_USERNIC_DB, "$with_usernic_db")
AS_AC_EXPAND(LXCROOTFSMOUNT, "$with_rootfs_path")
AS_AC_EXPAND(LXCTEMPLATEDIR, "$datadir/lxc/templates")
AS_AC_EXPAND(LXCHOOKDIR, "$datadir/lxc/hooks")
AS_AC_EXPAND(LXCINITDIR, "$libexecdir")
AS_AC_EXPAND(LOGPATH, "$with_log_path")
AC_SUBST(DEFAULT_CGROUP_PATTERN, ["$with_cgroup_pattern"])

# Check for some standard kernel headers
AC_CHECK_HEADERS([linux/unistd.h linux/netlink.h linux/genetlink.h],
				[],
				AC_MSG_ERROR([Please install the Linux kernel headers.]),
				[#include <sys/socket.h>])

# Allow disabling libcap support
AC_ARG_ENABLE([capabilities],
	[AC_HELP_STRING([--disable-capabilities], [disable kernel capabilities])],
	[], [enable_capabilities=yes])

# Check for libcap support
if test "x$enable_capabilities" = "xyes"; then
	AC_CHECK_LIB(cap,cap_set_proc,caplib=yes,caplib=no)
	AC_MSG_CHECKING([linux capabilities])
	if test "x$caplib" = "xyes" ; then
		CAP_LIBS="-lcap"
		AC_MSG_RESULT([$CAP_LIBS])
	else
		AC_MSG_RESULT([no])
		AC_MSG_ERROR([You are missing libcap support. If you really want to build without kernel capabilities, use --disable-capabilities])
	fi
else
	CAP_LIBS=""
fi
AC_SUBST([CAP_LIBS])

# Check for alternate C libraries
AC_MSG_CHECKING(for bionic libc)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
	[[#ifndef __ANDROID__
error: Not bionic!
#endif]])],
	[is_bionic=yes],
	[is_bionic=no])
if test "x$is_bionic" = "xyes"; then
	AC_DEFINE([IS_BIONIC], 1, [bionic libc])
	AC_MSG_RESULT([yes])
else
	AC_MSG_RESULT([no])
fi
AM_CONDITIONAL([IS_BIONIC], [test "x$is_bionic" = "xyes"])

# Some systems lack PR_CAPBSET_DROP definition => HAVE_DECL_PR_CAPBSET_DROP
AC_CHECK_DECLS([PR_CAPBSET_DROP], [], [], [#include <sys/prctl.h>])

# Check for some headers
AC_CHECK_HEADERS([sys/signalfd.h pty.h ifaddrs.h sys/capability.h sys/personality.h utmpx.h sys/timerfd.h])

# Check for some syscalls functions
AC_CHECK_FUNCS([setns pivot_root sethostname unshare rand_r confstr])

# Check for some functions
AC_CHECK_LIB(util, openpty)
AC_CHECK_FUNCS([openpty hasmntopt setmntent endmntent])
AC_CHECK_FUNCS([getline],
	AM_CONDITIONAL(HAVE_GETLINE, true)
	AC_DEFINE(HAVE_GETLINE,1,[Have getline]),
	AM_CONDITIONAL(HAVE_GETLINE, false))
AC_CHECK_FUNCS([fgetln],
	AM_CONDITIONAL(HAVE_FGETLN, true)
	AC_DEFINE(HAVE_FGETLN,1,[Have fgetln]),
	AM_CONDITIONAL(HAVE_FGETLN, false))

# Check for some libraries
AC_SEARCH_LIBS(sem_open, [rt pthread])
AC_SEARCH_LIBS(clock_gettime, [rt])

# Check for some standard binaries
AC_PROG_GCC_TRADITIONAL
AC_PROG_SED

if test "x$GCC" = "xyes"; then
	CFLAGS="$CFLAGS -Wall -Werror"
fi

# Files requiring some variable expansion
AC_CONFIG_FILES([
	Makefile
	lxc.pc
	lxc.spec
	config/Makefile

	doc/Makefile
	doc/legacy/lxc-ls.sgml
	doc/lxc-attach.sgml
	doc/lxc-cgroup.sgml
	doc/lxc-checkconfig.sgml
	doc/lxc-checkpoint.sgml
	doc/lxc-clone.sgml
	doc/lxc-console.sgml
	doc/lxc-create.sgml
	doc/lxc-destroy.sgml
	doc/lxc-device.sgml
	doc/lxc-execute.sgml
	doc/lxc-freeze.sgml
	doc/lxc-info.sgml
	doc/lxc-kill.sgml
	doc/lxc-ls.sgml
	doc/lxc-monitor.sgml
	doc/lxc-netstat.sgml
	doc/lxc-ps.sgml
	doc/lxc-restart.sgml
	doc/lxc-snapshot.sgml
	doc/lxc-start-ephemeral.sgml
	doc/lxc-start.sgml
	doc/lxc-stop.sgml
	doc/lxc-top.sgml
	doc/lxc-unfreeze.sgml
	doc/lxc-unshare.sgml
	doc/lxc-version.sgml
	doc/lxc-wait.sgml

	doc/lxc.conf.sgml
	doc/lxc.sgml
	doc/common_options.sgml
	doc/see_also.sgml

	doc/rootfs/Makefile

	doc/examples/Makefile
	doc/examples/lxc-macvlan.conf
	doc/examples/lxc-vlan.conf
	doc/examples/lxc-no-netns.conf
	doc/examples/lxc-empty-netns.conf
	doc/examples/lxc-phys.conf
	doc/examples/lxc-veth.conf
	doc/examples/lxc-complex.conf

	doc/ja/Makefile
	doc/ja/legacy/lxc-ls.sgml
	doc/ja/lxc-attach.sgml
	doc/ja/lxc-cgroup.sgml
	doc/ja/lxc-checkconfig.sgml
	doc/ja/lxc-checkpoint.sgml
	doc/ja/lxc-clone.sgml
	doc/ja/lxc-console.sgml
	doc/ja/lxc-create.sgml
	doc/ja/lxc-destroy.sgml
	doc/ja/lxc-device.sgml
	doc/ja/lxc-execute.sgml
	doc/ja/lxc-freeze.sgml
	doc/ja/lxc-info.sgml
	doc/ja/lxc-kill.sgml
	doc/ja/lxc-ls.sgml
	doc/ja/lxc-monitor.sgml
	doc/ja/lxc-netstat.sgml
	doc/ja/lxc-ps.sgml
	doc/ja/lxc-restart.sgml
	doc/ja/lxc-snapshot.sgml
	doc/ja/lxc-start-ephemeral.sgml
	doc/ja/lxc-start.sgml
	doc/ja/lxc-stop.sgml
	doc/ja/lxc-top.sgml
	doc/ja/lxc-unfreeze.sgml
	doc/ja/lxc-unshare.sgml
	doc/ja/lxc-version.sgml
	doc/ja/lxc-wait.sgml

	doc/ja/lxc.conf.sgml
	doc/ja/lxc.sgml
	doc/ja/common_options.sgml
	doc/ja/see_also.sgml

	hooks/Makefile

	templates/Makefile
	templates/lxc-cirros
	templates/lxc-debian
	templates/lxc-ubuntu
	templates/lxc-ubuntu-cloud
	templates/lxc-opensuse
	templates/lxc-busybox
	templates/lxc-fedora
	templates/lxc-openmandriva
	templates/lxc-oracle
	templates/lxc-altlinux
	templates/lxc-sshd
	templates/lxc-archlinux
	templates/lxc-alpine
	templates/lxc-plamo

	src/Makefile
	src/lxc/Makefile
	src/lxc/lxc-ps
	src/lxc/lxc-netstat
	src/lxc/lxc-checkconfig
	src/lxc/lxc-version
	src/lxc/lxc-start-ephemeral
	src/lxc/legacy/lxc-ls
	src/lxc/lxc.functions

	src/python-lxc/Makefile

	src/lua-lxc/Makefile

	src/tests/Makefile
])
AC_CONFIG_COMMANDS([default],[[]],[[]])
AC_OUTPUT

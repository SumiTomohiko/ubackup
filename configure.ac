#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.68])
AC_INIT([UnnamedBackupTool], [0.0.1], [SumiTomohiko@neko-daisuki.ddo.jp])
AM_INIT_AUTOMAKE([dist-xz])
AC_CONFIG_SRCDIR([ubtc.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([limits.h stdlib.h string.h sys/file.h sys/time.h syslog.h \
                  unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_TYPE_UID_T
AC_TYPE_MODE_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MKTIME
AC_CHECK_FUNCS([bzero dup2 getcwd gettimeofday localtime_r mkdir strchr \
                strerror strrchr strtoul])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

# vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4

AC_PREREQ([2.69])
AC_INIT([Mux2tty], [0.10], [mux2tty@klickitat.com])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([mux2tty.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h netdb.h netinet/in.h stdlib.h string.h sys/socket.h sys/time.h syslog.h termios.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([dup2 memset select socket strndup strtoul])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

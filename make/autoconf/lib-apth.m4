#
# Setup libapth (userspace threading library for JVM integration)
#
AC_DEFUN_ONCE([LIB_SETUP_LIBAPTH],
[
  AC_ARG_WITH(libapth, [AS_HELP_STRING([--with-libapth],
      [specify prefix directory for libapth])])

  LIBAPTH_CFLAGS=
  LIBAPTH_LIBS=

  if test "x$with_libapth" != "x" -a "x$with_libapth" != "xno"; then
    LIBAPTH_CFLAGS="-I${with_libapth}/src -DUSE_LIBAPTH -DUSE_LIBRARY_BASED_TLS_ONLY"
    LIBAPTH_LIBS="-L${with_libapth}/build/lib -Wl,-rpath,${with_libapth}/build/lib -lapth -ldl"
    AC_MSG_NOTICE([Using libapth from $with_libapth])
  fi

  AC_SUBST(LIBAPTH_CFLAGS)
  AC_SUBST(LIBAPTH_LIBS)
])

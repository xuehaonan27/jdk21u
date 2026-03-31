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
    # Resolve all paths to absolute for reliable build from any directory.
    LIBAPTH_ABS_SRC=$(cd "${with_libapth}/src" 2>/dev/null && pwd)
    if test -z "$LIBAPTH_ABS_SRC"; then
      AC_MSG_ERROR([libapth src directory not found at ${with_libapth}/src])
    fi
    LIBAPTH_ABS_LIB=$(cd "${with_libapth}/build/lib" 2>/dev/null && pwd)
    if test -z "$LIBAPTH_ABS_LIB"; then
      AC_MSG_ERROR([libapth build/lib directory not found. Run 'make' in libapth first.])
    fi

    LIBAPTH_CFLAGS="-I${LIBAPTH_ABS_SRC} -DUSE_LIBAPTH -DUSE_LIBRARY_BASED_TLS_ONLY"
    # Link the standard libapth.so dynamically.  At runtime, the JVM
    # should also be launched with LD_PRELOAD=libapth.so for process-wide
    # libc interposition (covers libnio epoll_wait, etc.).
    LIBAPTH_LIBS="-L${LIBAPTH_ABS_LIB} -lapth -Wl,-rpath,${LIBAPTH_ABS_LIB} -lpthread -ldl"
    AC_MSG_NOTICE([Using libapth from ${LIBAPTH_ABS_SRC} (lib: ${LIBAPTH_ABS_LIB})])
  fi

  AC_SUBST(LIBAPTH_CFLAGS)
  AC_SUBST(LIBAPTH_LIBS)
])

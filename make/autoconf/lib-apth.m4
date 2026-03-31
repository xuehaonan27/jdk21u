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
    # Link the JVM-specific shared library dynamically.  libapth_jvm.so
    # includes I/O hooks (read/write/poll/epoll_wait/accept/connect/recv/send)
    # that interpose libc for M:N cooperative scheduling, but EXCLUDES signal
    # hooks (sigaction/signal) since HotSpot manages its own signal handlers.
    # Resolve to absolute path for reliable linking and rpath.
    # Link the standard libapth.so dynamically.  At runtime, the JVM
    # should also be launched with LD_PRELOAD=libapth.so for process-wide
    # libc interposition (covers libnio epoll_wait, etc.).
    LIBAPTH_ABS_LIB=$(cd "${with_libapth}/build/lib" 2>/dev/null && pwd)
    if test -z "$LIBAPTH_ABS_LIB"; then
      AC_MSG_ERROR([libapth build/lib directory not found. Run 'make' in libapth first.])
    fi
    LIBAPTH_LIBS="-L${LIBAPTH_ABS_LIB} -lapth -Wl,-rpath,${LIBAPTH_ABS_LIB} -lpthread -ldl"
    AC_MSG_NOTICE([Using libapth (JVM shared library) from $with_libapth])
  fi

  AC_SUBST(LIBAPTH_CFLAGS)
  AC_SUBST(LIBAPTH_LIBS)
])

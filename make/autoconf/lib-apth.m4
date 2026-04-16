#
# Setup libapth (userspace threading library for JVM integration)
#
AC_DEFUN_ONCE([LIB_SETUP_LIBAPTH],
[
  AC_ARG_WITH(libapth, [AS_HELP_STRING([--with-libapth],
      [specify prefix directory for libapth])])

  AC_ARG_WITH(remote, [AS_HELP_STRING([--with-remote],
      [remote memory backend: SIM (default, in-process), TCP (executor via TCP), RDMA (executor via ibverbs)])])

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
    # Link against libapth.so (includes I/O hooks for M:N cooperative scheduling).
    # LD_PRELOAD of libapth.so is needed at runtime for the hooks to take effect.
    # This is used for localrate<100 (disaggregated memory with RDMA yield).
    # For localrate=100 baseline, build with USE_LIBAPTH=0 instead.
    LIBAPTH_LIBS="-L${LIBAPTH_ABS_LIB} -lapth -Wl,-rpath,${LIBAPTH_ABS_LIB} -lpthread -ldl"

    # Remote memory backend selection.
    # --with-remote=SIM   → default, in-process simulated remote (no network)
    # --with-remote=TCP   → TCP to remote_executor process
    # --with-remote=RDMA  → ibverbs RDMA to remote_executor process
    # If not specified, defaults to SIM.
    REMOTE_BACKEND="SIM"
    if test "x$with_remote" != "x"; then
      REMOTE_BACKEND=$(echo "$with_remote" | tr '[[a-z]]' '[[A-Z]]')
    fi

    case "$REMOTE_BACKEND" in
      SIM)
        AC_MSG_NOTICE([Remote memory backend: SIM (in-process, no network)])
        LIBAPTH_CFLAGS="${LIBAPTH_CFLAGS} -DREMOTE_BACKEND_SIM"
        ;;
      TCP)
        AC_MSG_NOTICE([Remote memory backend: TCP (executor via TCP)])
        LIBAPTH_CFLAGS="${LIBAPTH_CFLAGS} -DREMOTE_BACKEND_TCP"
        ;;
      RDMA)
        AC_MSG_NOTICE([Remote memory backend: RDMA (executor via ibverbs)])
        LIBAPTH_CFLAGS="${LIBAPTH_CFLAGS} -DREMOTE_EXECUTOR_USE_RDMA -DREMOTE_BACKEND_RDMA"
        LIBAPTH_LIBS="${LIBAPTH_LIBS} -libverbs"
        ;;
      *)
        AC_MSG_ERROR([Invalid --with-remote value: $with_remote. Use SIM, TCP, or RDMA.])
        ;;
    esac

    AC_MSG_NOTICE([Using libapth from ${LIBAPTH_ABS_SRC} (lib: ${LIBAPTH_ABS_LIB})])
  fi

  AC_SUBST(LIBAPTH_CFLAGS)
  AC_SUBST(LIBAPTH_LIBS)
])

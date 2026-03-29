# Building and Running the LIBAPTH-Integrated JVM

This guide documents the exact build configuration, compilation flags,
and runtime behavior of the OpenJDK 21 HotSpot JVM integrated with
LIBAPTH userspace M:N threading.

## Prerequisites

- Linux x86-64 (tested on Ubuntu 24.04, kernel 6.8)
- GCC with C11 and C++17 support
- Standard OpenJDK build dependencies (`build-essential`, `autoconf`, etc.)
- A stock JDK 21 as the boot/build JDK (for cross-compilation bootstrapping)
- jtreg (optional, for running tests)

## Directory Layout

```
jvm_libapth/
├── libapth/          # LIBAPTH userspace thread library (branch: feature/jvm-integration)
├── jdk21u/           # OpenJDK 21 source (branch: feature/jvm-integration)
└── build-jdk-stock/  # Stock JDK 21 used as --with-build-jdk
```

## Step 1: Build libapth_core.a

```bash
cd libapth
make core
```

This builds `build/lib/libapth_core.a` — a hook-free static library
specifically designed for JVM integration.

### Core Build Flags

| Flag | Value | Purpose |
|------|-------|---------|
| `APTH_CORE_BUILD` | Defined | Disables libc interposition (no hook wrappers), skips signal handler installation — HotSpot manages its own signals |
| `APTH_CUR_USING_KEYWORD` | Defined | Uses C11 `_Thread_local` for scheduler TLS (faster than `pthread_getspecific`) |
| `APTH_HOLD_INITIALIZER_PTHREAD` | Defined | Holds the initializer pthread, calls `exit()` after worker 0 finishes |
| `-fPIC` | Set | Position-independent code for linking into `libjvm.so` |

### Explicitly Disabled Features (in core build)

| Flag | Reason |
|------|--------|
| `APTH_PREEMPT_SIGNAL` | SIGPROF conflicts with HotSpot's deliberate SIGSEGV probes and profiling |
| `APTH_USE_RDMA` | RDMA support is for future phases, not needed for local-machine operation |
| `APTH_NUMA` | NUMA multi-reactor is stub only, not yet implemented |
| `APTH_USE_IOURING` | io_uring not needed for JVM integration |
| Hook wrappers | `hook_libc/hook_*.c` excluded — HotSpot does its own I/O, no libc interposition |

### What libapth_core.a Includes

- Assembly context switch (`apth_ctx_x86_64.S`) — ~20ns userspace switch
- Scheduler loop + worker pool (`apth_sched.c`, `apth_worker.c`)
- Event manager with epoll (`apth_event.c`)
- Global I/O reactor (`apth_reactor.c`)
- Synchronization primitives: mutex, cond, semaphore, rwlock, barrier
- Thread lifecycle: create, join, exit, detach
- TLS: key_create, getspecific, setspecific
- Signal management: sigmask, kill (software-only, no kernel mask for M:N)
- JVM integration APIs: `apth_init_library`, `apth_attach_self_as_dedicated`,
  `apth_get_stack_bounds`, `apth_get_thread_stats`, `apth_request_pause_all`,
  `apth_for_each_thread`, state callbacks, preempt hooks
- Dedicated thread support (1:1 pthreads that bypass scheduler)
- Raw syscall wrappers (`raw_funcs.c`)

## Step 2: Configure OpenJDK

```bash
cd jdk21u
bash configure \
  --with-libapth=../libapth \
  --with-build-jdk=../build-jdk-stock \
  --with-jtreg=~/jtreg
```

### Key Configure Effect

The `--with-libapth=<prefix>` flag triggers `make/autoconf/lib-apth.m4`, which sets:

```makefile
LIBAPTH_CFLAGS = -I<prefix>/src -DUSE_LIBAPTH -DUSE_LIBRARY_BASED_TLS_ONLY
LIBAPTH_LIBS   = <prefix>/build/lib/libapth_core.a -lpthread -ldl
```

These propagate through the build system:

| Build file | What it does |
|------------|--------------|
| `lib-apth.m4` | Defines `LIBAPTH_CFLAGS` and `LIBAPTH_LIBS` from configure arg |
| `spec.gmk.in` | Templates for `@LIBAPTH_CFLAGS@` and `@LIBAPTH_LIBS@` |
| `JvmFeatures.gmk` | Adds `LIBAPTH_CFLAGS` to `JVM_CFLAGS_FEATURES` |
| `CompileJvm.gmk` | Adds `LIBAPTH_LIBS` to `JVM_LIBS` (static link into `libjvm.so`) |
| `Images.gmk` | Skips CDS archive generation (LIBAPTH JVM can't run during build) |

### Compilation Flags Enabled in HotSpot

| Flag | Purpose |
|------|---------|
| `USE_LIBAPTH` | Master conditional. Guards all LIBAPTH-specific code paths (~800 lines across 26 files) |
| `USE_LIBRARY_BASED_TLS_ONLY` | Forces `Thread::current()` to use `ThreadLocalStorage::thread()` → `apth_getspecific()` instead of `_Thread_local` (which is per-worker, not per-apth) |

### Why --with-build-jdk is Needed

The LIBAPTH-integrated JVM cannot run during the build (it needs the
LIBAPTH scheduler workers, which aren't available in the build
environment's cross-compilation context). A stock JDK 21 is used for
build-time Java compilation (javac, jlink, etc.).

## Step 3: Build

```bash
make images CONF=linux-x86_64-server-release
```

Output: `build/linux-x86_64-server-release/images/jdk/`

### Incremental Rebuilds

After modifying libapth source:
```bash
cd libapth && rm -f build/lib/libapth_core.a build/obj_core/**/*.o && make core
cd jdk21u && touch src/hotspot/os/posix/semaphore_posix.cpp  # force relink
make images CONF=linux-x86_64-server-release
```

The `touch` is needed because the JDK build system tracks `.o` dependencies
but not external `.a` files. Touching any HotSpot source file forces
`libjvm.so` to relink with the updated `libapth_core.a`.

## Step 4: Run

```bash
./build/linux-x86_64-server-release/images/jdk/bin/java -Xshare:off -version
```

### Why -Xshare:off

CDS (Class Data Sharing) archives are not generated during the build
(see above). The `-Xshare:off` flag disables CDS to avoid "shared archive
not found" errors. This is only needed until CDS archive generation is
fixed for LIBAPTH builds.

## Thread Classification

| HotSpot ThreadType | LIBAPTH Class | Scheduling |
|--------------------|---------------|------------|
| `os::java_thread` | `APTH_CLASS_IO_BOUND` | M:N — multiplexed on scheduler workers |
| `os::vm_thread` | `APTH_CLASS_DEDICATED` | 1:1 — own pthread |
| `os::gc_thread` | `APTH_CLASS_DEDICATED` | 1:1 — own pthread |
| `os::compiler_thread` | `APTH_CLASS_DEDICATED` | 1:1 — own pthread |
| `os::watcher_thread` | `APTH_CLASS_DEDICATED` | 1:1 — own pthread |
| Primordial thread | DEDICATED (attached) | 1:1 — `apth_attach_self_as_dedicated()` |

**M:N threads include**: Reference Handler, Finalizer, Signal Dispatcher,
ServiceThread, Notification Thread, Common-Cleaner, user Java threads.

**DEDICATED threads include**: VM Thread, GC workers, C1/C2 compilers,
WatcherThread, primordial/main thread.

## Worker Configuration

Number of scheduler workers: `sysconf(_SC_NPROCESSORS_ONLN) - 2` (minimum 1).

On a 12-core machine: 10 workers. Each worker is a real pthread pinned
to the scheduler loop. M:N apths are dispatched on workers via assembly
context switch.

## Safepoint (Stop-The-World) Integration

HotSpot's STW mechanism is kept as-is — it only pauses JavaThreads via
polling word/page arming. LIBAPTH's `apth_request_pause_all()` is NOT
used because:
- It pauses all M:N threads (too broad — includes non-Java M:N)
- It doesn't affect DEDICATED JavaThreads (too narrow)

Instead, every cooperative yield path in HotSpot that could leave an M:N
JavaThread in `_thread_in_vm` state transitions the thread to
`_thread_blocked` first. This makes the thread visible as "safe" to
`SafepointSynchronize::begin()`.

## HotSpot Files Modified (26 files, ~800 lines)

### Linux-specific
- `os_linux.cpp` — Thread creation, stack bounds, signal masks, CPU time
- `os_linux.hpp` — Worker count, class mapping
- `osThread_linux.hpp` — `apth_t _apth_id` field
- `waitBarrier_linux.cpp` — M:N yield loop instead of futex
- `os_perf_linux.cpp` — Context switch lock

### POSIX-generic
- `os_posix.cpp` — PlatformEvent/Parker/PlatformMonitor/PlatformMutex all use apth_* primitives; `os::naked_yield()` uses `apth_yield()`
- `os_posix.inline.hpp` — PlatformMutex lock/unlock/trylock
- `mutex_posix.hpp` — `apth_mutex_t` and `apth_cond_t` type substitution
- `semaphore_posix.hpp/.cpp` — `apth_sem_t` substitution
- `signals_posix.cpp` — Signal handling via `apth_sigmask`, `apth_kill`
- `threadLocalStorage_posix.cpp` — TLS via `apth_key_create`/`apth_getspecific`
- `safefetch_sigjmp.cpp` — SafeFetch TLS via `apth_getspecific`

### Shared (platform-independent)
- `mutex.cpp` — M:N state transition in `lock_without_safepoint_check` and `wait_without_safepoint_check`
- `safepointMechanism.hpp/.inline.hpp/.cpp` — `_need_resched` field and yield action
- `threads.cpp` — `apth_init_library()` and `apth_attach_self_as_dedicated()` during VM init
- `vmError.cpp` — Error handling with LIBAPTH context
- `jni.cpp` — JNI attach uses `apth_attach_self_as_dedicated()`

### Build system
- `lib-apth.m4` — Configure integration
- `libraries.m4` — Include lib-apth.m4
- `spec.gmk.in` — Template variables
- `JvmFeatures.gmk` — CFLAGS propagation
- `CompileJvm.gmk` — Library linking
- `Images.gmk` — CDS skip logic

## Testing

```bash
# Quick smoke test
java -Xshare:off -version
java -Xshare:off -help

# Multi-thread test
java -Xshare:off YourProgram.java

# jtreg (requires --with-jtreg during configure)
~/jtreg/bin/jtreg \
  -jdk:build/linux-x86_64-server-release/images/jdk \
  -vmoptions:"-Xshare:off" \
  -timeout:1 \
  test/hotspot/jtreg/gc/TestSystemGC.java \
  test/hotspot/jtreg/runtime/Monitor/ \
  test/hotspot/jtreg/runtime/Thread/Fibonacci.java
```

## Known Limitations

1. **CDS disabled** — `-Xshare:off` required
2. **No signal-based preemption** — cooperative scheduling only; `_need_resched` writer not wired
3. **Suspend/resume targets workers** — `pthread_kill(SIGUSR2)` hits the worker, not a specific apth; JFR profiling may misbehave
4. **RDMA not connected** — `apth_rdma_wait` has no call sites in HotSpot; for future integration
5. **Shutdown imperfect** — `apth_drop()` skipped; relies on `_exit()` cleanup
6. **Worker-0 funnel** — M:N threads from DEDICATED parent default to worker 0; no round-robin yet

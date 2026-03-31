# LIBAPTH-JVM Remaining Work

This document captures known limitations and future integration points
that are not yet implemented. Items are ordered by priority.

## Deferred: Cooperative Preemption (_need_resched)

The `_need_resched` field in `SafepointMechanism::ThreadData` is declared
and checked at every safepoint poll, and the consumer (yield action in
`SafepointMechanism::process()`) is implemented. But there is no writer.

**Plan**: Wire LIBAPTH's preemption timer (SIGPROF, already owned by
LIBAPTH) to set `_need_resched = true` and arm the local poll word.
Use `apth_set_preempt_hook()` to register a HotSpot callback.

Files:
- `safepointMechanism.hpp:72` — field declaration
- `safepointMechanism.inline.hpp:72` — check
- `safepointMechanism.cpp:169` — yield action
- `libapth/src/internal/apth_preempt.c:74` — SIGPROF timer
- `libapth/src/core/apth_safepoint.c:86` — preempt hook API

## Deferred: RDMA Integration

The RDMA yield path is the core research goal. LIBAPTH has the
infrastructure; HotSpot needs interception points.

**LIBAPTH side (ready)**:
- CQ registration: `apth_rdma.c:319`
- RDMA wait (yield on CQ): `apth_rdma.c:429`
- Simulated CQ test: `test/test_rdma_sim.c`
- Build flag: `APTH_USE_RDMA`

**HotSpot interception points**:
- Heap access barrier: `access.hpp:155` → `barrierSet.hpp:193` → `access.inline.hpp:160`
- Compiled code (C1): `barrierSetC1.cpp:88`
- Compiled code (C2): `barrierSetC2.cpp:132`

**Strategy**: Custom barrier slow path detects "remote" object reference
→ posts RDMA read → `apth_rdma_wait()` → M:N thread yields → RDMA
completion poller wakes thread → retry load.

**Testing**: Use `test_rdma_sim.c` simulated CQ flow — no RDMA hardware
needed for initial development and validation.

## Deferred: WaitBarrier Yield Loop Efficiency

The WaitBarrier (`waitBarrier_linux.cpp`) uses a yield loop for M:N
threads instead of futex. During long safepoints, this burns CPU.

A more efficient approach would use LIBAPTH's event system to wait for
the barrier value to change, avoiding busy-yield. But since safepoints
are typically brief (<1ms), this is low priority.

File: `waitBarrier_linux.cpp:78`

## Note: IO Hook Coverage

LIBAPTH hooks cover the major I/O families used by the JDK:
- read/write/pread/pwrite/readv/writev (input_output_primitives.c)
- open/close/dup/pipe/fcntl (open_close_files.c, duplicating_fd.c, fcntl.c, pipe.c)
- poll/select/pselect (select_poll.c)
- epoll_wait/epoll_pwait (epoll.c) — added for JDK NIO
- socket/connect/accept/recv/send/recvfrom/sendto/recvmsg/sendmsg (hook_socket.c)
- sleep/nanosleep/usleep (hook_time.c)

Not hooked (low value for JVM):
- futex, mmap, mprotect — not called from M:N hot paths
- pthread_kill, pthread_cond_* — HotSpot uses apth_* equivalents directly

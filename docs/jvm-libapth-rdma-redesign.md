# JVM/libapth/RDMA Far-Memory Redesign

Date: 2026-05-07

This memo resets the design direction after the Spark NB 25% local-memory
experiments showed that the current object-handle path is not a viable
universal architecture.  It is meant to guide the next implementation steps
across `jdk21u`, `libapth`, and `remote_executor`.

## 1. Current Evidence

The latest dense-anchor cascade run used JDK commit `c26f9003655` and app
`app-20260507141029-0000`.  Evidence is archived on the CPU node at:

`/home/xuehaonan/eval-disagg-gc/scripts/codex-c26f900-dense-cascade-fetchstorm-20260507-141029`

Observed behavior:

- Correctness held during the monitored window: one executor, no new `hs_err`,
  no Java exception observed before the run was stopped.
- Throughput was poor: around 80 tasks finished by about 415s, worse than the
  previous 118-task run and far from the pthread/kernel-paging baseline.
- Dense eviction now commits, but the cost is too high.  Useful cycles evicted
  only tens of MiB while spending seconds in STW metadata, verification, root
  guards, and remote-send work.
- Fetch storm persisted.  By GC(129), the JVM had about 624K successful fetches
  and about 305s cumulative fetch time.
- At GC(129), a 1040MiB eviction target was fully blocked by roots, remote
  anchors, and raw-stack guards.

The conclusion is not "tune thresholds harder".  The conclusion is that the
current per-object handle/tagged-oop semantics have too much fixed work per
remote object for dense/sequential workloads.

## 2. Why The Current Path Loses

The ideal far-memory access path is:

1. Detect that data is remote.
2. Fetch data.
3. Use it with normal CPU/JVM semantics.

The current object path is much heavier.  Fetching an object back can involve:

- FCR allocation.
- Object byte copy.
- mark-word normalization.
- `Klass*` restoration.
- edge-table-based oop field patching.
- post-fetch validation.
- stale field repair or nulling.
- card dirtying.
- handle table rekeying.
- handle state transitions.
- later stale-handle, root-anchor, tagged-field, and old-region fixup.

Eviction is also heavy:

- ensure handles for many objects.
- scan/tag all heap refs into candidate regions.
- build edge tables.
- verify missed refs.
- guard roots and raw stacks.
- finalize handles as remote.
- fillerize old heap addresses.
- repair stale refs in later GCs.

This violates the main lesson from AIFM and Eden:

- Object granularity wins only if local access is near-native and remote miss
  handling is cheap.
- Pervasive software guards and metadata can erase the benefit of avoiding page
  faults.

For Spark NB, the current prototype combines the worst parts of both worlds:
object-level metadata overhead plus dense/sequential access behavior that
benefits from page/chunk locality.

## 3. New Design Principle

The system should not claim that object granularity always wins.

The stronger claim is:

> A managed runtime can choose the right far-memory granularity and policy from
> JVM semantics: object granularity for sparse pointer-heavy data, chunk/region
> granularity for dense or sequential data, and compiler/interpreter guidance to
> prefetch and evict according to the program's actual access shape.

This makes the system adaptive rather than dogmatic.

## 4. Target Architecture

The redesigned system has four residency modes.

### 4.1 Ordinary Local Mode

Most oops are plain native HotSpot oops.

Rules:

- No handle lookup.
- No tagged field.
- No per-access metadata work.
- Normal G1 barriers continue to apply.

This must remain the common case.

### 4.2 Dense Chunk Mode

Dense old regions, primitive arrays, object arrays, and Spark block-like data use
chunk/region granularity.

Core idea:

- Preserve virtual object addresses.
- Evict a complete chunk or G1 old region to the memory node.
- Drop local RSS with `madvise` and protect the address range.
- On access, fetch the chunk back into the same virtual address range.
- Existing raw oops inside the chunk remain valid because object addresses do
  not change.

This is the closest path to "just fetch, and the object is usable".

Required metadata:

- chunk id / region id.
- local virtual base and byte length.
- remote segment id.
- residency state: local, remote, fetching.
- dirty state.
- access counters and last semantic access site.
- conservative outgoing-reference summary for GC.

What this avoids:

- no per-object handle creation for dense chunks.
- no per-field tagged oop patching.
- no edge table per object.
- no `Klass*` restoration per fetched object.
- no handle rekeying per fetched object.

### 4.3 Sparse Object Mode

Small, cold, pointer-heavy objects with low spatial locality can still use an
object-granularity path.

However, this mode must be explicitly limited:

- It is not the default for dense old regions.
- It should avoid tagging ordinary heap refs wherever possible.
- It must be benchmarked on small-object/random workloads such as DaCapo
  `h2`, `lusearch`, and `xalan`.
- It must have a measured local-access overhead budget.

The current handle system can be kept as a prototype and correctness reference,
but should not define the whole architecture.

### 4.4 Fallback Fault Mode

Missed hints need a correctness fallback.

Options:

- `mprotect` plus signal handler.
- `userfaultfd`.
- conservative synchronous runtime localization.

This fallback is not the performance path.  If the main path depends on kernel
faults, libapth cannot hide latency because the kernel blocks the OS thread.
The performance path must use computation-guided hints before the actual fault.

## 5. Computation-Guided Memory Management

Interpreter, C1, and C2 should actively guide memory management.  They should
not only profile.

### 5.1 Access Shapes

Add a compact access-shape vocabulary:

- `unknown`
- `scalar_field_load`
- `field_chain`
- `object_array_scan`
- `primitive_array_scan`
- `indexed_random_array`
- `iterator_next`
- `graph_pointer_chase`
- `spark_block_scan`
- `gc_trace`

Each access site should optionally carry:

- bytecode index or nmethod pc.
- base object type / array type when cheaply available.
- stride or index expression classification.
- expected locality window.
- prefetch distance.
- eviction priority class.

### 5.2 Interpreter

Interpreter hooks should annotate and act at:

- `getfield`
- `putfield`
- `aaload`
- `aastore`
- primitive array loads/stores
- invoke/iterator patterns where cheaply recognizable

For chunk mode, the interpreter can call:

`ensure_chunk_local(base_oop, access_shape, site_id)`

before dereferencing a base object or array.  If the chunk is local, this is a
cheap residency bitmap check.  If it is remote, it starts fetch and yields via
libapth when safe.

### 5.3 C1

C1 should emit the same chunk-residency checks but keep them thin:

- inline region lookup.
- test residency byte/bit.
- branch to runtime stub only on remote/fetching.

For simple counted loops over arrays, C1 should hoist the check out of the loop
or prefetch the next chunk window.

### 5.4 C2

C2 should treat far-memory checks as memory access guards that can be:

- hoisted out of loops.
- coalesced for multiple loads from the same base.
- specialized by array/scalar/field-chain access shape.
- paired with prefetch nodes.

The goal is that most local accesses remain native loads plus, at worst, one
hoisted chunk residency check per loop/window.

### 5.5 Policy Use

The runtime should use access shapes for:

- chunk fetch size.
- prefetch distance.
- eviction victim selection.
- whether to use sparse object mode or dense chunk mode.
- remote executor fetch-around policy.
- GC tracing priority.

Example:

- `primitive_array_scan`: fetch/prefetch contiguous chunks, do not pollute sparse
  object cache.
- `object_array_scan`: fetch object-array chunk, then prefetch target chunks by
  element clusters.
- `field_chain`: fetch graph neighbors by outgoing edges only when sparse mode
  is active.
- `spark_block_scan`: keep related RDD block chunks grouped and prefetch ahead.

## 6. GC Cooperation

Chunk mode changes the GC contract.

### 6.1 Initial Correctness Contract

For the first dense-chunk vertical slice:

- Remote chunks are old-generation only.
- Remote chunks are non-moving while remote.
- G1 must not include remote chunks in the collection set.
- A remote chunk can be conservatively treated as live as a whole.
- On eviction, the JVM records a conservative outgoing-reference summary for
  references from the chunk to outside the chunk.
- While a chunk is remote, it cannot be mutated locally; a write first localizes
  it and dirties its cards.

This is conservative but avoids per-object pointer rewriting.

### 6.2 Young GC Interaction

Old-to-young references from remote chunks are the risky case.

Initial safe choices:

- Prefer evicting chunks with no young outgoing refs.
- Otherwise keep a remembered-summary produced at eviction time.
- If G1 needs exact card scanning for a remote chunk, localize that chunk or let
  the memory node scan the chunk bytes and return referenced handles/oops.

### 6.3 Marking And Mixed GC

Initial safe choices:

- Mark remote chunks live as whole chunks.
- Keep all cross-chunk outgoing refs conservatively live.
- Do not reclaim individual dead objects inside remote chunks.

This may retain extra memory, but it makes the first slice safe.  Later work can
add memory-node tracing and chunk-level liveness.

### 6.4 MemLiner-Style Alignment

GC should consume mutator access metadata:

- Trace recently accessed chunks first.
- Avoid scanning chunks that the mutator is unlikely to touch soon.
- Prefer GC order that matches current application traversal.
- Use compiler/interpreter access-shape history to choose GC trace queues.

This is computation-guided GC, not just mutator prefetch.

## 7. remote_executor Redesign

The memory node should be structured around segments/chunks, not only slots.

### 7.1 Segment Store

Add a chunk store:

- `segment_id`
- virtual base address from CPU JVM.
- byte size.
- current data buffer / RDMA memory region.
- dirty/live bytes.
- optional object index for scan/debug.
- optional outgoing-reference summary.

Fetch should support:

- exact segment fetch.
- subrange fetch.
- prefetch window fetch.
- grouped fetch by semantic stream id.

### 7.2 Sparse Object Store

Keep a separate sparse-object store:

- size-class slabs for object bytes.
- compact CSR-like edge storage.
- handle directory.
- graph fetch-around.

Do not mix dense chunk storage with sparse object metadata on the hot path.

### 7.3 Remote GC

Later remote GC should operate differently per mode:

- dense chunk mode: chunk-level liveness first, object-level tracing optional.
- sparse object mode: handle/edge tracing.
- arrays and Spark blocks: segment retention/eviction based on stream and block
  lifetime, not individual object death.

## 8. libapth Requirements

libapth is only valuable if remote waits are:

- frequent enough to create scheduling opportunity.
- nonblocking at the kernel-thread level.
- cheap enough to yield from.
- integrated with RDMA CQ wakeups without futex-heavy hot paths.

For chunk mode, libapth must be used by hinted misses:

1. computation-guided check sees chunk remote.
2. runtime posts RDMA fetch.
3. current user-level thread yields.
4. RDMA poller wakes it when the chunk is local.

If a miss reaches a kernel page fault first, libapth does not help much because
the OS thread is already blocked.  This is why hint-before-fault is mandatory.

Required libapth evaluation:

- pthread vs libapth at 100% local memory.
- synthetic RDMA wait/yield throughput.
- futex/syscall count.
- scheduler queue contention.
- per-core run queue behavior.
- RDMA CQ wakeup latency.

## 9. First Vertical Slice

The first implementation target should be dense chunk mode for Spark NB.

Scope:

- Old G1 regions only.
- Complete-region eviction only.
- Preserve virtual addresses.
- No per-object handles for chunk residents.
- No per-object edge tables.
- No object fillerization inside remote chunks.
- Conservative GC summaries.
- Runtime `ensure_chunk_local()` for interpreter/C1 first.

Success criteria:

- Spark NB does not crash or throw Java exceptions.
- One executor, one GC log.
- Later object loads after a chunk fetch hit local memory.
- Remote fetch count drops by orders of magnitude compared with per-object mode.
- STW eviction work becomes proportional to chunks/regions, not objects.
- At 420s, task count exceeds the previous 118-task run before further tuning.

Implementation steps:

1. Add chunk residency metadata to HotSpot G1 regions.
2. Add remote executor segment commands: evict segment, fetch segment, discard
   segment.
3. Evict selected dense old regions as segments and mark region remote/protected.
4. Add `ensure_chunk_local()` runtime path.
5. Add interpreter checks for array and field base objects.
6. Add C1 checks for the Spark test configuration.
7. Add conservative GC rules: remote chunks old/non-moving/live-whole.
8. Add metrics: chunk evicts, chunk fetches, bytes, latency, residency hits,
   fallback faults.

## 10. Ablation Plan

Before claiming the redesign works, run:

1. `localrate=100`: stock pthread JVM, current libapth JVM, redesigned JVM with
   remote disabled.
2. chunk mode disabled vs enabled at `localrate=25`.
3. object mode disabled vs enabled for small-object workloads.
4. interpreter only vs C1 enabled.
5. prefetch disabled vs computation-guided prefetch.
6. libapth yield enabled vs synchronous wait.
7. remote executor segment store vs object slot store.

Primary metrics:

- end-to-end task progress and completion time.
- number of remote misses.
- remote bytes fetched.
- fetch latency distribution.
- RDMA bandwidth.
- CPU utilization.
- STW pause breakdown.
- cgroup RSS and OOM events.
- number of fallback faults.
- local access overhead at 100% memory.

## 11. Design Decisions

Current decisions:

- Stop treating object granularity as universal.
- Keep the existing handle path as a prototype/reference and possible sparse
  mode, not as the default dense-data path.
- Make dense chunk mode the next implementation target.
- Make computation-guided memory management a first-class mechanism, not a
  profiling side project.
- Use libapth only where the JVM can yield before kernel blocking.
- Keep correctness conservative even if the first chunk-GC policy retains extra
  remote data.

Open decisions:

- `mprotect` versus `userfaultfd` for fallback correctness.
- Exact chunk size: G1 region, subregion, or multiple regions.
- Whether the first C1 implementation is enough for Spark NB, or whether C2
  support is needed before meaningful performance results.
- Whether remote chunks should be visible as G1 old regions or a separate
  far-old space.
- How much outgoing-reference summary precision is needed to avoid excess
  retention without returning to per-object overhead.

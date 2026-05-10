# JVM/libapth/RDMA Far-Memory Redesign

Date: 2026-05-07

This memo resets the design direction after the Spark NB 25% local-memory
experiments showed that the current object-handle path is not a viable
universal architecture.  It is meant to guide the next implementation steps
across `jdk21u`, `libapth`, and `remote_executor`.

## 0. Superseding Update: Handle-Centric Old Generation

Date: 2026-05-10

The design direction changed again after the C2 correctness work and old-set
repair regressions. The earlier idea of making dense chunks primarily preserve
raw virtual object addresses is no longer the target architecture.

New invariant:

> Nearly the whole Old Generation should be managed through stable
> `RemoteHandle` identity. A heap slot is either an ordinary local oop or a
> managed+indirect oop that points to a `RemoteHandle`. Chunk, cluster,
> array-window, remote location, residency, hotness, and prefetch semantics live
> behind the Handle, not in long-lived direct tagged heap addresses.

Equivalently, Old Generation becomes a HIT-style second-level indirection
space:

`oop slot -> virtual object identity / HIT entry -> physical local address or remote location`

The HIT entry is the stable object identity.  Its current physical location can
be a local old/FCR address, a remote single-object slot, a remote cluster object,
or an array/chunk descriptor.  GC, fetch, eviction, and prefetch update the HIT
entry; ordinary heap slots do not need to be globally rewritten when the object
moves between local and remote forms.

Direct tagged heap addresses are a compatibility and repair input only. New
promotion, eviction, dense/chunk, prefetch, C1, and C2 code must not create
long-lived direct tagged heap slots.

Chunk/cluster mode is not raw-address segment mode. It is a storage and
transfer representation for groups of Handles. Many object payloads may be
physically contiguous on the memory node, and a miss may fetch a whole cluster
window, but Java-visible correctness is still published by updating the
corresponding Handles to LOCAL.

Older sections that mention preserving virtual object addresses should be read
as historical context unless explicitly restated in handle-centric form below.

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

### 4.2 Handle-Centric Dense Chunk / Cluster Mode

Dense old regions, primitive arrays, object arrays, and Spark block-like data use
chunk/region granularity.

Core idea:

- Keep stable per-object `RemoteHandle` identity for old remote-manageable
  objects.
- Store dense small objects and array slices in remote clusters/chunks so the
  memory node keeps payloads physically contiguous.
- Attach a remote-location descriptor to each Handle, or to a side table indexed
  by the Handle:
  `{kind, cluster_id, chunk_id, offset, byte_size, klass, flags}`.
- On a miss, fetch a bounded cluster/window, install the returned objects into
  local FCR/chunk-cache memory, patch fields using existing edge metadata, and
  publish all fetched Handles LOCAL in one batch.
- Later Java loads resolve through the Handle fast path and hit local memory;
  they do not issue one remote request per object.

This is the closest safe path to "just fetch, and the object is usable": the
object is usable after its Handle is LOCAL, not because a raw stale address
inside a dense range was trusted.

Required metadata:

- per-Handle remote-location descriptor.
- cluster id / chunk id / offset table.
- cluster-local object index for fetch windows.
- edge metadata stored compactly per cluster, preferably CSR-like for many
  small homogeneous objects.
- dirty and residency summaries.
- access counters and last semantic access site.
- conservative outgoing-reference summary for GC and remote tracing.

What this avoids:

- no long-lived direct tagged heap addresses.
- no correctness dependence on "address is inside a dense segment".
- no full old-set repair caused by raw stale direct aliases.
- no one-RDMA-fetch-per-object behavior for dense clusters.
- no per-object `malloc/free` on the memory node for objects that were evicted
  as a cluster.

This mode still uses Handles. Its purpose is to make Handle-managed objects
cheap in dense/sequential workloads by batching storage, transfer, install, and
publish.

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

For handle-centric chunk/cluster mode, the interpreter can call:

`ensure_handle_cluster_local(base_oop, access_shape, site_id)`

before dereferencing a base object or array. If the Handle or cluster window is
already local, this is a cheap state check. If it is remote, it starts fetch and
yields via libapth when safe.

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
- whether to use sparse object mode or handle-centric cluster/chunk mode.
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

Handle-centric cluster mode changes the GC contract.

### 6.1 Initial Correctness Contract

For the first handle-centric cluster vertical slice:

- Remote-managed objects are old-generation only at first.
- A remote object is Java-visible only through its Handle. The old heap address
  may be filler, reclaimed, or guarded, but no live heap slot should depend on
  that raw address.
- A cluster can be conservatively treated as live as a whole while any resident
  Handle is live.
- On eviction, the JVM records edge metadata from each remote object to target
  Handles, and the remote executor stores this compactly for cluster tracing and
  graph fetch-around.
- On fetch, the JVM installs object bytes locally, patches oop fields to
  managed+indirect Handle oops, dirties cards for the installed objects, and
  publishes the fetched Handles LOCAL in one batch.
- Writes to remote Handles first resolve/localize the target object. Later
  optimization may add copy-on-write or dirty cluster tracking.

This is conservative but avoids raw-address source repair as the correctness
mechanism.

### 6.2 Young GC Interaction

Old-to-young references from remote clusters are the risky case.

Initial safe choices:

- Prefer evicting old objects/clusters with no young outgoing refs.
- Patch remote object fields to target Handles before sending/storing, so young
  targets are represented by Handles and can be updated by normal Handle
  maintenance.
- Otherwise keep a remembered-summary produced at eviction time.
- If G1 needs exact scanning for a remote cluster, use the memory node's edge
  metadata or localize the cluster window.

### 6.3 Marking And Mixed GC

Initial safe choices:

- Mark remote clusters live as whole clusters at first when any member Handle is
  live.
- Keep all cross-cluster outgoing refs conservatively live unless remote
  executor edge tracing proves otherwise.
- Do not reclaim individual dead objects inside a cluster in the first slice;
  later add cluster compaction or split hot/live objects out of cold/dead
  clusters.

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

The memory node should be structured around Handle-aware segments/clusters, not
only independent slots.

### 7.1 Cluster Segment Store

Add a cluster store:

- `cluster_id` / segment id.
- ordered slot ids and Handle ids in the cluster.
- per-slot offset and byte size inside the segment.
- current data buffer / RDMA memory region.
- dirty/live bytes.
- compact object index for fetch-window selection.
- compact outgoing-reference summary / CSR edge table.

Fetch should support:

- exact Handle/slot fetch.
- same-cluster neighbor fetch.
- bounded cluster-window fetch.
- grouped fetch by semantic stream id.

### 7.2 Sparse Object Store

Keep a separate sparse-object store:

- size-class slabs for object bytes.
- compact CSR-like edge storage.
- handle directory.
- graph fetch-around.

Do not require dense clusters to use one allocation per object on the hot path.

### 7.3 Remote GC

Later remote GC should operate differently per mode:

- cluster mode: cluster-level liveness first, object-level tracing optional.
- sparse object mode: handle/edge tracing.
- arrays and Spark blocks: segment retention/eviction based on stream and block
  lifetime, not individual object death.

## 8. libapth Requirements

libapth is only valuable if remote waits are:

- frequent enough to create scheduling opportunity.
- nonblocking at the kernel-thread level.
- cheap enough to yield from.
- integrated with RDMA CQ wakeups without futex-heavy hot paths.

For handle-centric cluster mode, libapth must be used by hinted misses:

1. computation-guided check sees a Handle/cluster window remote.
2. runtime posts RDMA fetch.
3. current user-level thread yields.
4. RDMA poller wakes it when the requested Handles are local.

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

The first implementation target should be handle-centric cluster/chunk mode for
Spark NB and dense old-object populations.

Scope:

- Old G1 regions only.
- Do not create new direct tagged heap slots.
- Keep per-object Handles for all remote-managed objects.
- Use remote executor segment-backed storage for many Handle/slot payloads in
  one physical cluster.
- Fetch a bounded cluster window on a miss and publish many Handles LOCAL in one
  operation.
- Keep existing single-object fetch as fallback.
- Keep conservative GC summaries and full correctness checks until cluster
  source maps are exact.
- Runtime `ensure_handle_cluster_local()` for interpreter/C1/C2 slow paths,
  backed by the existing tagged-oop resolver.

Success criteria:

- Spark NB does not crash or throw Java exceptions.
- One executor, one GC log.
- Later object loads after a cluster-window fetch resolve through LOCAL Handles
  without issuing new fetches.
- Remote fetch count and request count drop substantially compared with
  one-object fetch behavior.
- Remote executor stores dense batches as contiguous cluster segments, not as
  one allocation per object.
- STW eviction work shifts from failed whole-region dense scans toward bounded
  preparation of useful Handle clusters.
- At 420s, task count exceeds the previous 118-task run before further tuning.

Implementation steps:

1. Add a `RemoteLocation` side descriptor for `RemoteHandle`: object slot,
   cluster object, array chunk, and future dense chunk kinds.
2. Make existing single-object eviction/fetch go through the descriptor without
   changing behavior.
3. Upgrade remote executor segment-backed batches into explicit clusters:
   cluster id, slot order, Handle ids, offsets, and compact edge metadata.
4. Add JVM-side cluster-window fetch/install/publish using the existing
   batch-fetch installer shape: allocate local objects, patch fields to Handles,
   call `localize_batch`, then publish all Handles LOCAL.
5. Prefer cluster neighbors over numeric slot neighbors in fetch-around, and
   expose metrics for cluster seen/returned/installed/wasted.
6. Add array-aware descriptors next: large object arrays and primitive arrays
   should fetch windows/chunks rather than using single-object array fetch or
   whole raw-region restore.
7. Feed interpreter/C1/C2 access hints into the cluster-window size decision.
8. Add metrics: cluster evicts, cluster fetches, Handles published, bytes,
   latency, local Handle hits after prefetch, fallback exact fetches.

## 10. Ablation Plan

Before claiming the redesign works, run:

1. `localrate=100`: stock pthread JVM, current libapth JVM, redesigned JVM with
   remote disabled.
2. handle-centric cluster/chunk mode disabled vs enabled at `localrate=25`.
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
- Make stable Handle identity the default for old remote-manageable objects.
- Use cluster/chunk/array layout as a remote storage and transfer optimization
  behind Handles, not as direct raw-address oop semantics.
- Keep direct tagged addresses only as compatibility/repair input while draining
  old states.
- Make handle-centric cluster/chunk mode the next implementation target.
- Make computation-guided memory management a first-class mechanism, not a
  profiling side project.
- Use libapth only where the JVM can yield before kernel blocking.
- Keep correctness conservative even if the first chunk-GC policy retains extra
  remote data.
- The current x86 signal path treats SIGSEGV on an evict-guarded G1 region as
  fatal.  Therefore `mprotect` is only a last-resort correctness mechanism in
  the current prototype, not a cheap miss path.  The performance path must
  localize or prefetch before a raw CPU load reaches the protected range.
- Phase C.5 full-heap verification is currently correctness work, not stale
  diagnostics: disabling it can leave raw refs that fault on guarded dense
  regions.  The next optimization should replace it with remembered-source /
  card-summary validation and repair, not simply skip it.

Open decisions:

- `mprotect` versus `userfaultfd` for fallback correctness.
- Exact chunk size: G1 region, subregion, or multiple regions.
- Whether the first C1 implementation is enough for Spark NB, or whether C2
  support is needed before meaningful performance results.
- Whether fetched cluster windows should land only in FCR, or in a separate
  local cluster cache with better spatial locality.
- How much outgoing-reference summary precision is needed to avoid excess
  retention without returning to per-object overhead.

## 12. HIT-Based System Design

This section is the current target design.  It supersedes the raw dense-region
segment direction above where the two conflict.

### 12.1 Object Identity And Oop Encoding

Old remote-manageable objects have stable HIT entries.  The current
`RemoteHandle` can become that HIT entry:

```text
heap oop slot -> tagged pointer to RemoteHandle/HIT entry
HIT entry     -> LOCAL physical oop address
              -> REMOTE object slot
              -> REMOTE cluster object
              -> REMOTE array/chunk descriptor
```

Rules:

- Young objects may stay as ordinary raw oops.
- Heap slots that reference old remote-manageable objects should store
  managed+indirect Handle oops.
- Stack/register/JNI raw old oops are tolerated only as short-lived roots.  A
  safepoint eviction must either pin those objects or convert the roots before
  eviction.  They must not force full old-heap source repair.
- Direct managed heap addresses are legacy repair input only.

### 12.2 Thin Load Barrier

If most Old Generation references are HIT references, the current "tagged means
call runtime" path is too expensive.  The load barrier must become:

```text
load ref
if ref is ordinary raw oop:
    return ref
if ref is managed+indirect:
    h = decode_handle(ref)
    state_addr = h->_state_and_addr
    if state is LOCAL:
        return local_addr(state_addr)
    else:
        call slow path to fetch/wait/throw/null as appropriate
```

The LOCAL Handle case must be inline in interpreter/C1/C2.  Runtime calls are
for REMOTE, FETCHING, DEAD, invalid, or diagnostic states.  Without this, a
HIT-based old generation will trade correctness for unacceptable barrier cost.

### 12.3 Store Barrier And Promotion

The store barrier is the source of the invariant:

- When storing an oop into any heap field/array element, if the value is an old
  remote-manageable object, store its HIT reference instead of a raw physical
  oop.
- Interpreter, C1, C2, and atomic/unsafe store paths must share this rule.
- C2 must model the returned tagged value as raw bits, not as a GC oop in
  OopMaps.  The previous raw leaf-call approach exposed this requirement, but
  the final implementation should be a real store barrier/lowering rule rather
  than an ad-hoc call on arbitrary oop values.
- Young-to-old promotion is the clean transition point: create or attach a HIT
  entry for the promoted object, publish it LOCAL, and make evacuated heap slots
  that point to it write the HIT reference.

This means Phase C should no longer scan the old heap trying to discover and
tag every source reference to candidate objects.  The heap slots should already
hold stable identities.

### 12.4 Eviction

Eviction becomes a location change for HIT entries:

1. Select old objects or clusters to evict.
2. Build remote bytes where oop fields are already encoded as Handle/HIT
   references.
3. Send the bytes and compact metadata to the memory node.
4. Change the selected HIT entries from LOCAL to REMOTE with a location
   descriptor.
5. Reclaim or fillerize local physical memory only after stack/root guards prove
   no live raw physical roots still require it.

The key difference from the current path is that correctness does not require
finding all heap source slots.  Source slots already reference the HIT entry.

### 12.5 Fetch And Prefetch

Fetch is also a location change:

1. A mutator sees a REMOTE HIT entry.
2. Runtime chooses a fetch unit from access semantics:
   - exact object,
   - graph cluster,
   - same eviction segment,
   - object-array element window,
   - primitive-array payload chunk.
3. The memory node returns object records for a bounded window.
4. The JVM installs those objects into FCR or a local cluster cache.
5. The JVM publishes all corresponding HIT entries LOCAL in a batch.

Fetched object bytes should already contain Handle/HIT references in oop
fields.  That lets install avoid rebuilding every field from stale raw
addresses.  Validation remains necessary, but per-fetch edge patching should
move out of the hot path over time.

### 12.6 Arrays

Arrays need explicit policies:

- Object arrays: elements that point to old objects should be HIT references.
  Array-load semantics can prefetch the element Handle window and the target
  clusters.
- Large primitive arrays: the array object identity is a HIT entry, but payload
  should be managed in chunks.  Primitive array load/store barriers need
  `ensure_array_chunk_local(base, index, width, access_site)`.
- Small arrays: keep local or evict as ordinary cluster objects until metrics
  prove chunking helps.

This is where Spark NB should eventually beat one-object fetch: array access
shape gives exact chunk/window information.

### 12.7 GC Contract

GC must treat HIT entries as the authority for old object location:

- Moving a LOCAL old object updates its HIT entry.
- Evicting an old object changes its HIT entry to REMOTE.
- Fetching an object changes its HIT entry to LOCAL.
- Heap scanning of managed+indirect slots marks the HIT target, not the stale
  physical address.
- Remote clusters can be conservatively live as a whole in the first version.
- Remote executor edge metadata can later trace cluster/object references using
  Handle ids.

The expensive full old/cset repair path should become a debug verifier and
legacy-state drain, not a normal performance path.

### 12.8 remote_executor v2

The memory node should be redesigned around HIT ids, not slot ids as the public
identity.

Core tables:

```text
HitDirectory:
  hit_id -> {state, location_kind, cluster_id, offset, size, klass, version}

ClusterStore:
  cluster_id -> contiguous payload buffer
             -> ordered hit list
             -> per-hit offset/size/class metadata
             -> CSR edge table of hit_id -> target_hit_ids
             -> hotness/live/dead counters

ArrayChunkStore:
  array_hit_id -> payload chunks
               -> dirty/resident/chunk-hotness metadata
```

Protocol v2 should provide:

- `PUT_CLUSTER_STAGED`: JVM RDMA-writes a contiguous payload and sends compact
  hit metadata.
- `FETCH_HIT_WINDOW`: primary hit plus semantic policy/window, returning a batch
  of hit records.
- `LOCALIZE_HITS`: JVM reports HIT entries now local.
- `REPORT_ROOT_HITS` and `TRACE_HITS`: remote GC roots and tracing by hit id.
- `FREE_HITS` / `FREE_CLUSTER`: explicit reclamation.

The current C `remote_executor` already contains parts of this: handle
directory, edge tables, staged homogeneous batches, and segment-backed storage.
It can be refactored toward this model, or rewritten if the monolithic C file
keeps blocking correctness and performance.

### 12.9 Computation Guidance

Interpreter/C1/C2 should drive the fetch unit:

- field load: exact object plus graph neighbors by edge metadata.
- object-array load: element Handle window plus target clusters.
- primitive-array load: payload chunk/window.
- loop over array: prefetch next chunks and hoist locality checks.
- pointer chase: small graph cluster, not slot-neighbor scan.

C2 must carry base/index/stride from IR or barrier metadata.  Do not infer
array index or stride from arbitrary machine registers after lowering.

### 12.10 First Engineering Phase

The first implementation phase should prove the HIT abstraction without
rewriting everything at once:

1. Add a `RemoteLocation` side descriptor to `RemoteHandle`.
2. Route existing object-slot fetch/evict through `RemoteLocation`.
3. Make heap stores to old remote-manageable objects consistently write
   managed+indirect HIT refs in interpreter/C1/C2/native paths.
4. Inline LOCAL Handle resolution in the load barrier.
5. Promote current remote_executor segment-backed eviction batches into
   explicit clusters with hit order and offsets.
6. Add cluster-window fetch that installs and publishes many HIT entries LOCAL.
7. Keep full verification enabled until metrics show no new direct tagged refs
   and no raw stale heap slots are produced by new paths.

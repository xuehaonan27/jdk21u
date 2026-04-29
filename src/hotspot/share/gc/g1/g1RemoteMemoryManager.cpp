/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 */

#include "precompiled.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1RemoteBackend.hpp"
#include "gc/g1/g1RemoteBackendSim.hpp"
#include "gc/g1/g1RemoteBackendTcp.hpp"
#include "gc/g1/g1RemoteBackendRdma.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.inline.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1NUMA.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/threads.hpp"
#include "gc/shared/oopStorageSet.inline.hpp"
#include "utilities/copy.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "gc/shared/workerThread.hpp"
#include "gc/g1/heapRegionManager.inline.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/referenceProcessor.hpp"

// TCP client for remote executor communication
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

G1RemoteMemoryManager::G1RemoteMemoryManager(G1CollectedHeap* g1h)
  : _g1h(g1h), _backend(nullptr), _handle_allocator(),
    _entry_chunks(nullptr), _entry_free_list(nullptr), _entry_chunk_top(ENTRY_CHUNK_CAPACITY),
    _table_lock(0), _alloc_lock(0),
    _sim_remote_next_slot(0), _sim_remote_evicted_count(0),
    _sim_remote_fetched_count(0), _gc_epoch(0),
    _remote_roots(nullptr), _remote_roots_count(0), _remote_roots_capacity(0),
    _cross_roots_count(0), _deferred_decrement_count(0),
    _tagged_fields(nullptr), _tagged_field_count(0), _tagged_field_capacity(0),
    _fcr_evac_writes(0), _fcr_fixup_nulls(0),
    _current_fcr(nullptr), _fcr_lock(0) {
  _table = NEW_C_HEAP_ARRAY(HandleEntry*, TABLE_SIZE, mtGC);
  memset(_table, 0, TABLE_SIZE * sizeof(HandleEntry*));
  memset((void*)_stripe_locks, 0, sizeof(_stripe_locks));
  memset(_edge_buckets, 0, sizeof(_edge_buckets));
  memset(_hotness_stats, 0, sizeof(_hotness_stats));
  memset(_prev_hotness_stats, 0, sizeof(_prev_hotness_stats));
  memset(_sim_remote_slots, 0, sizeof(_sim_remote_slots));

  // Create remote storage backend.
  // Selection priority:
  //   1. Compile-time: --with-remote=RDMA/TCP/SIM sets REMOTE_BACKEND_* macros
  //   2. Runtime: -XX:+UseRemoteExecutor overrides to TCP (or RDMA if compiled)
  //   3. Default: SimLocalBackend (in-process, no network)
  //
  // With --with-remote=RDMA, the RDMA backend is the DEFAULT (no runtime flag needed).
  // With --with-remote=TCP, the TCP backend is the DEFAULT.
  // UseRemoteExecutor=true at runtime overrides SIM to the compiled executor backend.

#if defined(REMOTE_BACKEND_RDMA)
  // Compiled with --with-remote=RDMA: default to RDMA executor
  _backend = new RDMAExecutorBackend();
#elif defined(REMOTE_BACKEND_TCP)
  // Compiled with --with-remote=TCP: default to TCP executor
  _backend = new TCPExecutorBackend();
#else
  // Compiled with --with-remote=SIM (or not specified): default to sim-local
  // But UseRemoteExecutor at runtime can override to executor
  if (UseRemoteExecutor) {
#ifdef REMOTE_EXECUTOR_USE_RDMA
    _backend = new RDMAExecutorBackend();
#else
    _backend = new TCPExecutorBackend();
#endif
  } else {
    _backend = new SimLocalBackend();
  }
#endif

  // Backend object created; connection deferred to initialize_backend()
  // (called from G1CollectedHeap::initialize() when heap info is available).
}

bool G1RemoteMemoryManager::concurrent_marking_active() const {
  return _g1h->collector_state()->mark_or_rebuild_in_progress();
}

void G1RemoteMemoryManager::initialize_backend() {
  for (int attempt = 1; attempt <= 5; attempt++) {
    if (_backend->initialize()) {
      log_info(gc)("Remote memory backend: %s", _backend->name());
      return;
    }
    log_warning(gc)("Remote backend (%s) initialization attempt %d/5 failed, retrying in 2s...",
                    _backend->name(), attempt);
    _backend->shutdown();
    os::naked_sleep(2000);
  }
  log_warning(gc)("Remote backend (%s) initialization failed after 5 attempts, falling back to sim-local",
                  _backend->name());
  delete _backend;
  _backend = new SimLocalBackend();
  _backend->initialize();
  log_info(gc)("Remote memory backend: %s", _backend->name());
}

G1RemoteMemoryManager::~G1RemoteMemoryManager() {
  // Free HandleEntry chunks (entries are pool-managed, not individually freed)
  HandleEntryChunk* ec = _entry_chunks;
  while (ec != nullptr) {
    HandleEntryChunk* next = ec->_next;
    delete ec;
    ec = next;
  }
  FREE_C_HEAP_ARRAY(HandleEntry*, _table);
  _table = nullptr;

  // Free edge tables (chained hash)
  for (size_t i = 0; i < EDGE_TABLE_BUCKETS; i++) {
    EdgeTableEntry* e = _edge_buckets[i];
    while (e != nullptr) {
      EdgeTableEntry* next = e->_next;
      ObjectEdgeTable::free(e->_table);
      os::free(e);
      e = next;
    }
    _edge_buckets[i] = nullptr;
  }

  // Free simulated remote slot data
  for (size_t i = 0; i < SIM_REMOTE_MAX_SLOTS; i++) {
    if (_sim_remote_slots[i]._data != nullptr) {
      os::free(_sim_remote_slots[i]._data);
      _sim_remote_slots[i]._data = nullptr;
    }
  }
}

size_t G1RemoteMemoryManager::sim_remote_evict(oop obj, size_t word_size, Klass* klass) {
  assert(_sim_remote_next_slot < SIM_REMOTE_MAX_SLOTS, "Simulated remote memory full");
  size_t slot = _sim_remote_next_slot++;

  size_t byte_size = word_size * HeapWordSize;
  _sim_remote_slots[slot]._data = os::malloc(byte_size, mtGC);
  _sim_remote_slots[slot]._word_size = word_size;
  _sim_remote_slots[slot]._klass = klass;
  _sim_remote_slots[slot]._in_use = true;

  // Copy object bytes to simulated remote
  memcpy(_sim_remote_slots[slot]._data, cast_from_oop<void*>(obj), byte_size);

  _sim_remote_evicted_count++;
  return slot;
}

Klass* G1RemoteMemoryManager::sim_remote_fetch(size_t slot_id, void* dest, size_t word_size) {
  assert(slot_id < SIM_REMOTE_MAX_SLOTS, "Invalid slot");
  assert(_sim_remote_slots[slot_id]._in_use, "Slot not in use");

  size_t byte_size = word_size * HeapWordSize;
  memcpy(dest, _sim_remote_slots[slot_id]._data, byte_size);

  _sim_remote_fetched_count++;
  return _sim_remote_slots[slot_id]._klass;
}

// ============================================================
// Edge Table Construction
// ============================================================
// Closure that scans an object's oop fields and builds an edge table.
// For each non-null oop field, creates a dormant anchor Handle for the
// target and records the edge (field_offset → target_handle).

class EdgeTableBuildClosure : public BasicOopIterateClosure {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  RemoteHandleAllocBuffer* _hab;
  oop                    _base_obj;

  // Heap-allocated, growable. Replaces the old fixed MAX_EDGES=8192 stack
  // array, which pinned whole regions whenever a single large array
  // (e.g. scala.Tuple3[N>8192]) was hit during eviction.
  G1RemoteMemoryManager::EdgeEntry* _edges;
  uint32_t _count;
  uint32_t _capacity;

  void grow() {
    uint32_t new_cap = (_capacity == 0) ? 16u : _capacity * 2u;
    G1RemoteMemoryManager::EdgeEntry* new_edges =
      NEW_C_HEAP_ARRAY(G1RemoteMemoryManager::EdgeEntry, new_cap, mtGC);
    if (_edges != nullptr) {
      memcpy(new_edges, _edges,
             (size_t)_count * sizeof(G1RemoteMemoryManager::EdgeEntry));
      FREE_C_HEAP_ARRAY(G1RemoteMemoryManager::EdgeEntry, _edges);
    }
    _edges = new_edges;
    _capacity = new_cap;
  }

  void append(uint32_t offset, RemoteHandle* h) {
    if (_count >= _capacity) grow();
    _edges[_count]._field_offset = offset;
    _edges[_count]._target_handle = h;
    _count++;
  }

  oop canonical_target(oop target) {
    if (!_g1h->is_in(target)) return nullptr;
    if (target->is_forwarded()) {
      target = target->forwardee();
      if (target == nullptr || !_g1h->is_in(target)) return nullptr;
    }
    return target;
  }

public:
  EdgeTableBuildClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                        RemoteHandleAllocBuffer* hab, oop base)
    : _rmm(rmm), _g1h(g1h), _hab(hab), _base_obj(base),
      _edges(nullptr), _count(0), _capacity(0) {}

  ~EdgeTableBuildClosure() {
    if (_edges != nullptr) {
      FREE_C_HEAP_ARRAY(G1RemoteMemoryManager::EdgeEntry, _edges);
    }
  }

  virtual void do_oop(oop* p) {
    // Read field as raw uintptr_t to avoid debug oop constructor checks on tagged values
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;  // null

    // If already tagged (bit 63 set), the field already has a Handle reference.
    if ((raw >> 63) != 0) {
      if (raw & G1_OOP_INDIRECT_BIT) {
        // Shared OOP → already points to a Handle
        RemoteHandle* h = (RemoteHandle*)(raw & G1_OOP_ADDR_MASK);
        uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
        append(offset, h);
        h->increment_remote_refcount();
      } else {
        // Unique OOP → strip tags, get target, create dormant anchor
        oop target = (oop)(raw & G1_OOP_ADDR_MASK);
        target = canonical_target(target);
        if (target != nullptr) {
          RemoteHandle* h = _rmm->ensure_dormant_anchor_for(target, _hab);
          uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
          append(offset, h);
          h->increment_remote_refcount();
        }
      }
      return;
    }

    // Clean oop — create dormant anchor for the target
    oop target = cast_to_oop(raw);
    target = canonical_target(target);
    if (target != nullptr) {
      RemoteHandle* h = _rmm->ensure_dormant_anchor_for(target, _hab);
      uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
      append(offset, h);
      h->increment_remote_refcount();
    }
  }

  virtual void do_oop(narrowOop* p) {
    // Narrow oops: not used (UseCompressedOops=false in our config)
  }

  uint32_t count() const { return _count; }
  const G1RemoteMemoryManager::EdgeEntry* edges() const { return _edges; }
};

G1RemoteMemoryManager::ObjectEdgeTable*
G1RemoteMemoryManager::build_edge_table(oop obj, RemoteHandle* obj_handle,
                                        RemoteHandleAllocBuffer* hab) {
  EdgeTableBuildClosure cl(this, _g1h, hab, obj);
  obj->oop_iterate(&cl);

  // Allocate exact-sized ObjectEdgeTable and copy from the (now-sized) build
  // buffer. The build buffer is freed by the closure destructor below.
  ObjectEdgeTable* et = ObjectEdgeTable::allocate(cl.count());
  et->_source_handle = obj_handle;
  et->_eviction_word_size = obj->size();
  for (uint32_t i = 0; i < cl.count(); i++) {
    et->add(cl.edges()[i]._field_offset, cl.edges()[i]._target_handle);
  }

  log_debug(gc)("Edge table built: obj=" PTR_FORMAT " edges=%u",
                p2i((void*)obj), cl.count());
  return et;
}

static volatile int _prep_fail_null = 0;
static volatile int _prep_fail_locked = 0;
static volatile int _prep_fail_edge = 0;
static volatile int _prep_fail_slot = 0;
static volatile int _prep_success = 0;
static volatile int _prep_diag_logged = 0;

bool G1RemoteMemoryManager::prepare_eviction(oop obj, RemoteHandleAllocBuffer* hab,
                                              PreparedEviction* out) {
  if (obj == nullptr) { Atomic::add(&_prep_fail_null, 1); return false; }

  markWord mw = obj->mark();
  if (!mw.is_unlocked()) {
    if (Atomic::add(&_prep_fail_locked, 1) <= 3 && !_prep_diag_logged) {
      log_info(gc)("prepare_eviction: locked obj=" PTR_FORMAT " mw=0x%lx klass=%s",
                   p2i((void*)obj), (unsigned long)mw.value(), obj->klass()->external_name());
    }
    return false;
  }

  Klass* klass = obj->klass();
  size_t word_size = obj->size_given_klass(klass);

  RemoteHandle* h = handle_for(obj);
  if (h == nullptr) h = create_handle_for(obj, hab);

  ObjectEdgeTable* et = build_edge_table(obj, h, hab);
  if (et == nullptr) { Atomic::add(&_prep_fail_edge, 1); return false; }
  store_edge_table(et);

  size_t slot_id = _backend->allocate_slot_id();
  if (slot_id == (size_t)-1) { Atomic::add(&_prep_fail_slot, 1); return false; }

  Atomic::add(&_prep_success, 1);
  out->obj = obj;
  out->handle = h;
  out->klass = klass;
  out->word_size = word_size;
  out->slot_id = slot_id;
  out->edge_table = et;
  return true;
}

void G1RemoteMemoryManager::log_prepare_eviction_stats() {
  if (_prep_fail_null + _prep_fail_locked + _prep_fail_edge + _prep_fail_slot + _prep_success > 0) {
    log_info(gc)("prepare_eviction stats: success=%d null=%d locked=%d edge=%d slot=%d",
                 _prep_success, _prep_fail_null, _prep_fail_locked, _prep_fail_edge, _prep_fail_slot);
    _prep_diag_logged = 1;
  }
  _prep_fail_null = _prep_fail_locked = _prep_fail_edge = _prep_fail_slot = _prep_success = 0;
  _prep_diag_logged = 0;
}

void G1RemoteMemoryManager::finalize_eviction(PreparedEviction* entry) {
  entry->handle->set_eviction_word_size(entry->word_size);
  entry->handle->set_remote(entry->slot_id);

  markWord mw = entry->obj->mark();
  if (mw.is_unlocked()) {
    entry->obj->set_mark(mw.set_remote_class(markWord::remote_class_shared));
  }
  HeapRegion* hr = _g1h->heap_region_containing(entry->obj);
  if (hr != nullptr) {
    hr->set_has_classified_objects();
  }

  CollectedHeap::fill_with_object(cast_from_oop<HeapWord*>(entry->obj), entry->word_size, false);
}

int G1RemoteMemoryManager::collect_remote_anchor_addrs_in_regions(const bool* region_set,
                                                                  uint num_regions,
                                                                  uintptr_t* addrs,
                                                                  int max_addrs,
                                                                  bool* overflow) {
  if (overflow != nullptr) *overflow = false;
  if (region_set == nullptr || num_regions == 0) return 0;

  int count = 0;

  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      RemoteHandle* h = e->_handle;
      if (h == nullptr || !h->is_local() || h->remote_refcount() == 0) continue;

      uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
      if (addr == 0 || !_g1h->is_in((void*)addr)) continue;

      HeapRegion* hr = _g1h->heap_region_containing((void*)addr);
      if (hr == nullptr) continue;
      uint ridx = hr->hrm_index();
      if (ridx >= num_regions || !region_set[ridx]) continue;

      if (count < max_addrs && addrs != nullptr) {
        addrs[count] = addr;
      } else if (overflow != nullptr) {
        *overflow = true;
      }
      count++;
    }
  }

  for (int i = 0; i < _cross_roots_count; i++) {
    RemoteHandle* h = _cross_roots[i];
    if (h == nullptr || !h->is_local()) continue;

    uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
    if (addr == 0 || !_g1h->is_in((void*)addr)) continue;

    HeapRegion* hr = _g1h->heap_region_containing((void*)addr);
    if (hr == nullptr) continue;
    uint ridx = hr->hrm_index();
    if (ridx >= num_regions || !region_set[ridx]) continue;

    if (count < max_addrs && addrs != nullptr) {
      addrs[count] = addr;
    } else if (overflow != nullptr) {
      *overflow = true;
    }
    count++;
  }
  table_unlock();

  return count;
}

// ============================================================
// Region-Granularity Eviction
// ============================================================
// Evicts ALL objects in a region to remote. For each object:
//   1. Create Handle + build edge table (dormant anchors for outgoing refs)
//   2. Backend evict (send bytes via TCP/RDMA/SIM)
//   3. Handle → REMOTE + fill with filler
// After all objects evicted: tag incoming refs, free the region.

// Closure to tag incoming refs on a specific card range pointing into the evicted region.
class IncomingRefTagClosure : public BasicOopIterateClosure {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  HeapRegion*            _target_hr;
  int                    _tagged;
  bool                   _has_untaggable; // narrow oop or other untaggable ref found
public:
  IncomingRefTagClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h, HeapRegion* target)
    : _rmm(rmm), _g1h(g1h), _target_hr(target), _tagged(0), _has_untaggable(false) {}

  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;

    // Shared oops (bits 63+62) already go through a Handle — skip.
    if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
        (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

    // Resolve: strip Unique tag bits if present to get raw address.
    oop target;
    if ((raw >> 63) != 0) {
      target = cast_to_oop(raw & G1_OOP_ADDR_MASK);
    } else {
      target = cast_to_oop(raw);
    }
    if (!_g1h->is_in(target)) return;

    HeapRegion* target_region = _g1h->heap_region_containing(target);
    if (target_region != _target_hr) return;

    RemoteHandle* h = _rmm->handle_for(target);
    if (h != nullptr) {
      *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
      _tagged++;
    }
  }

  virtual void do_oop(narrowOop* p) {
    // Can't tag narrow oops — flag as untaggable
    narrowOop v = *p;
    if (!CompressedOops::is_null(v)) {
      oop target = CompressedOops::decode(v);
      if (_g1h->is_in(target)) {
        HeapRegion* target_region = _g1h->heap_region_containing(target);
        if (target_region == _target_hr) {
          _has_untaggable = true;
        }
      }
    }
  }

  int tagged() const { return _tagged; }
  bool has_untaggable() const { return _has_untaggable; }
};

// Remset visitor that collects card indices for a target region.
// Used to find which cards in source regions contain refs into the target.
class RemsetCardCollector {
  G1CollectedHeap* _g1h;
  G1CardTable*     _ct;
  HeapRegion*      _target_hr;
  G1RemoteMemoryManager* _rmm;
  int              _tagged;
  bool             _has_untaggable;

public:
  RemsetCardCollector(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h, HeapRegion* target)
    : _g1h(g1h), _ct(g1h->card_table()), _target_hr(target),
      _rmm(rmm), _tagged(0), _has_untaggable(false) {}

  bool start_iterate(uint tag, uint region_idx) {
    // Accept all source regions
    return true;
  }

  void do_card(uint card_idx) {
    scan_card(card_idx, 1);
  }

  void do_card_range(uint start_card_idx, uint length) {
    scan_card(start_card_idx, length);
  }

  int tagged() const { return _tagged; }
  bool has_untaggable() const { return _has_untaggable; }

private:
  void scan_card(uint card_idx, uint length) {
    // Convert card index to memory region
    HeapWord* card_start = _ct->addr_for((G1CardTable::CardValue*)(_ct->byte_for_index(card_idx)));
    HeapWord* card_end = card_start + length * G1CardTable::card_size_in_words();

    // Find the source region
    if (!_g1h->is_in(card_start)) return;
    HeapRegion* source_hr = _g1h->heap_region_containing(card_start);
    if (source_hr == nullptr || source_hr == _target_hr) return;

    // Clip to region bounds
    HeapWord* scan_start = MAX2(card_start, source_hr->bottom());
    HeapWord* scan_end = MIN2(card_end, source_hr->top());
    if (scan_start >= scan_end) return;

    // Scan objects overlapping this card range for refs into target region
    IncomingRefTagClosure cl(_rmm, _g1h, _target_hr);
    MemRegion mr(scan_start, scan_end);
    source_hr->oops_on_memregion_seq_iterate_careful<true>(mr, &cl);

    _tagged += cl.tagged();
    if (cl.has_untaggable()) _has_untaggable = true;
  }
};

void G1RemoteMemoryManager::tag_incoming_refs_to_region(HeapRegion* target_hr) {
  HeapRegionRemSet* rem_set = target_hr->rem_set();

  if (rem_set->is_complete() && !rem_set->is_empty()) {
    // Remset complete — use efficient remset-based scan
    RemsetCardCollector collector(this, _g1h, target_hr);
    rem_set->iterate_for_merge(collector);

    if (collector.has_untaggable()) {
      log_info(gc)("Region %u has untaggable incoming refs — pinning", target_hr->hrm_index());
      target_hr->set_root_pinned();
      return;
    }

    if (collector.tagged() > 0) {
      log_info(gc)("Tagged %d incoming refs to region %u via remset",
                   collector.tagged(), target_hr->hrm_index());
    }
  } else {
    // Remset not complete — cannot safely find all incoming refs.
    // Skip this region for eviction.
    log_debug(gc)("Remset incomplete for region %u — skipping eviction",
                  target_hr->hrm_index());
    target_hr->set_root_pinned();  // prevent eviction
  }
}

// Full heap scan: tag ALL heap refs pointing to any eviction candidate.
// Walks every non-candidate, non-empty region and checks each oop field.
// O(live_heap) but parallelized across GC workers. Catches refs that remset
// misses (dirty cards not yet refined, post-evacuation card dirtying, etc.).

class EvictionSetTagClosure : public BasicOopIterateClosure {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  const bool*            _eviction_set;
  uint                   _num_regions;
  int                    _tagged;
  int                    _no_handle;

  typedef G1RemoteMemoryManager::TaggedFieldEntry TaggedFieldEntry;
  TaggedFieldEntry* _local_buf;
  int               _local_count;
  int               _local_capacity;

  void local_buf_add(oop* field_addr, RemoteHandle* h) {
    if (_local_count >= _local_capacity) {
      int new_cap = (_local_capacity == 0) ? 4096 : _local_capacity * 2;
      TaggedFieldEntry* nb = NEW_C_HEAP_ARRAY(TaggedFieldEntry, new_cap, mtGC);
      if (_local_buf != nullptr) {
        memcpy(nb, _local_buf, _local_count * sizeof(TaggedFieldEntry));
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _local_buf);
      }
      _local_buf = nb;
      _local_capacity = new_cap;
    }
    _local_buf[_local_count]._field_addr = field_addr;
    _local_buf[_local_count]._handle = h;
    _local_count++;
  }

public:
  EvictionSetTagClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                        const bool* eset, uint nregions)
    : _rmm(rmm), _g1h(g1h), _eviction_set(eset),
      _num_regions(nregions), _tagged(0), _no_handle(0),
      _local_buf(nullptr), _local_count(0), _local_capacity(0) {}

  ~EvictionSetTagClosure() {
    // Don't free _local_buf here — caller takes ownership via release_local_buf()
  }

  TaggedFieldEntry* release_local_buf() {
    TaggedFieldEntry* buf = _local_buf;
    _local_buf = nullptr;
    return buf;
  }

  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;
    if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
        (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

    oop target;
    if ((raw >> 63) != 0) {
      target = cast_to_oop(raw & G1_OOP_ADDR_MASK);
    } else {
      target = cast_to_oop(raw);
    }
    if (!_g1h->is_in(target)) return;

    HeapRegion* target_hr = _g1h->heap_region_containing(target);
    uint idx = target_hr->hrm_index();
    if (idx >= _num_regions || !_eviction_set[idx]) return;

    // Root-catch relocated: object has forwarding pointer → redirect to new address
    if (target->is_forwarded()) {
      oop fwd = target->forwardee();
      uintptr_t tag_bits = raw & G1_OOP_TAG_MASK;
      uintptr_t new_addr = cast_from_oop<uintptr_t>(fwd) & G1_OOP_ADDR_MASK;
      *(uintptr_t*)p = tag_bits | new_addr;
      _tagged++;
      return;
    }

    RemoteHandle* h = _rmm->handle_for(target);
    if (h != nullptr) {
      *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
      local_buf_add(p, h);
      _tagged++;
    } else {
      _no_handle++;
      if (_no_handle <= 10) {
        log_warning(gc)("Tagging: no handle for target " PTR_FORMAT " in candidate region %u "
                        "(field at " PTR_FORMAT ")",
                        p2i((void*)target), idx, p2i(p));
      }
    }
  }

  virtual void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }

  int tagged() const { return _tagged; }
  int no_handle() const { return _no_handle; }
  const TaggedFieldEntry* local_buf() const { return _local_buf; }
  int local_count() const { return _local_count; }
};

static void scan_region_for_eviction_tags(HeapRegion* hr, EvictionSetTagClosure* cl,
                                          const G1CMBitMap* bitmap) {
  HeapWord* const pb = hr->parsable_bottom_acquire();
  HeapWord* const region_top = hr->top();
  HeapWord* const region_end = hr->end();
  int objects_scanned = 0;

  // Below parsable_bottom: dead objects may have dangling klasses (class unloaded,
  // concurrent rebuild hasn't filled them yet). Use the mark bitmap to find live
  // objects, skipping dead ones — same approach as G1ConcurrentRebuildAndScrub.
  HeapWord* p = hr->bottom();
  while (p < pb && p < region_top) {
    if (bitmap->is_marked(p)) {
      oop obj = cast_to_oop(p);
      size_t sz = obj->size();
      obj->oop_iterate(cl);
      objects_scanned++;
      p += sz;
    } else {
      p = bitmap->get_next_marked_addr(p, pb);
    }
  }

  // Above parsable_bottom: all objects are live, sequential scan is safe.
  if (p < pb) p = pb;
  HeapWord* seq_start = p;
  while (p < region_top) {
    if (p >= region_end) break;
    oop obj = cast_to_oop(p);
    Klass* k = obj->klass_or_null();
    if (k == nullptr) {
      size_t skipped_words = pointer_delta(region_top, p);
      log_warning(gc)("Phase C scan TRUNCATED: region %u null klass at " PTR_FORMAT
                      " (scanned %d objs, skipping " SIZE_FORMAT " words to top " PTR_FORMAT
                      ", pb=" PTR_FORMAT ")",
                      hr->hrm_index(), p2i(p), objects_scanned, skipped_words,
                      p2i(region_top), p2i(pb));
      break;
    }
    size_t sz = obj->size();
    if (sz == 0) {
      size_t skipped_words = pointer_delta(region_top, p);
      log_warning(gc)("Phase C scan TRUNCATED: region %u zero size at " PTR_FORMAT
                      " klass=%s (scanned %d objs, skipping " SIZE_FORMAT " words)",
                      hr->hrm_index(), p2i(p), k->external_name(),
                      objects_scanned, skipped_words);
      break;
    }
    if (sz > (size_t)(region_end - p)) {
      obj->oop_iterate(cl);
      objects_scanned++;
      break;
    }
    obj->oop_iterate(cl);
    objects_scanned++;
    p += sz;
  }
}

class TagAllHeapRefsTask : public WorkerTask {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  const bool*            _eviction_set;
  uint                   _num_regions;
  const G1CMBitMap*      _bitmap;
  HeapRegionClaimer      _claimer;
  volatile int           _total_tagged;
  volatile int           _total_no_handle;

  typedef G1RemoteMemoryManager::TaggedFieldEntry TaggedFieldEntry;
  TaggedFieldEntry** _worker_bufs;
  int*               _worker_counts;
  uint               _num_workers;

public:
  TagAllHeapRefsTask(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                     const bool* eset, uint nregions, uint num_workers,
                     const G1CMBitMap* bitmap)
    : WorkerTask("Tag eviction refs"),
      _rmm(rmm), _g1h(g1h), _eviction_set(eset), _num_regions(nregions),
      _bitmap(bitmap),
      _claimer(num_workers), _total_tagged(0), _total_no_handle(0),
      _num_workers(num_workers) {
    _worker_bufs = NEW_C_HEAP_ARRAY(TaggedFieldEntry*, num_workers, mtGC);
    _worker_counts = NEW_C_HEAP_ARRAY(int, num_workers, mtGC);
    memset(_worker_bufs, 0, num_workers * sizeof(TaggedFieldEntry*));
    memset(_worker_counts, 0, num_workers * sizeof(int));
  }

  ~TagAllHeapRefsTask() {
    for (uint i = 0; i < _num_workers; i++) {
      if (_worker_bufs[i] != nullptr) {
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _worker_bufs[i]);
      }
    }
    FREE_C_HEAP_ARRAY(TaggedFieldEntry*, _worker_bufs);
    FREE_C_HEAP_ARRAY(int, _worker_counts);
  }

  void work(uint worker_id) {
    EvictionSetTagClosure cl(_rmm, _g1h, _eviction_set, _num_regions);
    for (uint i = _claimer.offset_for_worker(worker_id); i < _g1h->num_regions(); i++) {
      if (!_claimer.claim_region(i)) continue;
      HeapRegion* hr = _g1h->region_at(i);
      if (hr->is_empty() || hr->is_free()) continue;
      if (hr->is_continues_humongous()) continue;
      scan_region_for_eviction_tags(hr, &cl, _bitmap);
    }
    Atomic::add(&_total_tagged, cl.tagged());
    Atomic::add(&_total_no_handle, cl.no_handle());
    _worker_bufs[worker_id] = cl.release_local_buf();
    _worker_counts[worker_id] = cl.local_count();
  }

  void flush_to_rmm() {
    for (uint i = 0; i < _num_workers; i++) {
      for (int j = 0; j < _worker_counts[i]; j++) {
        _rmm->add_tagged_field(_worker_bufs[i][j]._field_addr,
                               _worker_bufs[i][j]._handle);
      }
    }
  }

  int total_tagged() const { return _total_tagged; }
  int total_no_handle() const { return _total_no_handle; }
};

int G1RemoteMemoryManager::tag_all_heap_refs_to_eviction_set(
    const bool* eviction_set, uint num_regions,
    WorkerThreads* workers, uint num_workers) {

  int total_tagged, total_no_handle;

  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();

  if (workers != nullptr && num_workers > 1) {
    TagAllHeapRefsTask task(this, _g1h, eviction_set, num_regions, num_workers, bitmap);
    workers->run_task(&task, num_workers);
    task.flush_to_rmm();
    total_tagged = task.total_tagged();
    total_no_handle = task.total_no_handle();
  } else {
    EvictionSetTagClosure cl(this, _g1h, eviction_set, num_regions);
    for (uint i = 0; i < _g1h->num_regions(); i++) {
      HeapRegion* hr = _g1h->region_at(i);
      if (hr->is_empty() || hr->is_free()) continue;
      if (hr->is_continues_humongous()) continue;
      scan_region_for_eviction_tags(hr, &cl, bitmap);
    }
    for (int j = 0; j < cl.local_count(); j++) {
      add_tagged_field(cl.local_buf()[j]._field_addr, cl.local_buf()[j]._handle);
    }
    total_tagged = cl.tagged();
    total_no_handle = cl.no_handle();
  }

  if (total_tagged > 0 || total_no_handle > 0) {
    log_info(gc)("Full heap scan (%u workers): tagged %d refs, %d refs had no handle",
                 (workers != nullptr ? num_workers : 1), total_tagged, total_no_handle);
  }
  return total_tagged;
}

int G1RemoteMemoryManager::tag_evacuated_area_refs_to_eviction_set(
    const bool* eviction_set, uint num_regions,
    HeapWord* const* pre_evac_tops) {

  EvictionSetTagClosure cl(this, _g1h, eviction_set, num_regions);
  int regions_rescanned = 0;

  for (uint i = 0; i < _g1h->num_regions(); i++) {
    HeapRegion* hr = _g1h->region_at(i);
    if (hr->is_empty() || hr->is_free()) continue;
    if (hr->is_continues_humongous()) continue;

    HeapWord* pre_top = pre_evac_tops[i];
    HeapWord* cur_top = hr->top();
    if (pre_top >= cur_top) continue;

    HeapWord* p = pre_top;
    HeapWord* region_end = hr->end();
    while (p < cur_top) {
      if (p >= region_end) break;
      oop obj = cast_to_oop(p);
      Klass* k = obj->klass_or_null();
      if (k == nullptr) {
        log_warning(gc)("Phase C.1: null klass at " PTR_FORMAT " in region %u "
                        "(pre_top=" PTR_FORMAT " cur_top=" PTR_FORMAT ")",
                        p2i(p), i, p2i(pre_top), p2i(cur_top));
        break;
      }
      size_t sz = obj->size();
      if (sz == 0 || sz > (size_t)(region_end - p)) break;
      obj->oop_iterate(&cl);
      p += sz;
    }
    regions_rescanned++;
  }

  int total_tagged = cl.tagged();
  if (total_tagged > 0) {
    for (int j = 0; j < cl.local_count(); j++) {
      add_tagged_field(cl.local_buf()[j]._field_addr, cl.local_buf()[j]._handle);
    }
    log_warning(gc)("Phase C.1: re-scanned %d regions, tagged %d missed refs "
                    "(%d no handle)", regions_rescanned, total_tagged, cl.no_handle());
  }
  return total_tagged;
}

// RSet visitor: for each card in a candidate's RSet, scan with
// EvictionSetTagClosure to tag refs pointing to ANY candidate.
class EvictionSetRsetScanner {
  G1CollectedHeap*       _g1h;
  G1CardTable*           _ct;
  EvictionSetTagClosure* _cl;
  const bool*            _eviction_set;
  uint                   _num_regions;

public:
  EvictionSetRsetScanner(G1CollectedHeap* g1h, EvictionSetTagClosure* cl,
                         const bool* eviction_set, uint num_regions)
    : _g1h(g1h), _ct(g1h->card_table()), _cl(cl),
      _eviction_set(eviction_set), _num_regions(num_regions) {}

  bool start_iterate(uint tag, uint region_idx) { return true; }
  void do_card(uint card_idx) { scan_card(card_idx, 1); }
  void do_card_range(uint start_card_idx, uint length) { scan_card(start_card_idx, length); }

private:
  void scan_card(uint card_idx, uint length) {
    HeapWord* card_start = _ct->addr_for(_ct->byte_for_index(card_idx));
    HeapWord* card_end = card_start + length * CardTable::card_size_in_words();
    if (!_g1h->is_in(card_start)) return;
    HeapRegion* source_hr = _g1h->heap_region_containing(card_start);
    if (source_hr == nullptr || source_hr->is_empty() || source_hr->is_free()) return;
    uint src_idx = source_hr->hrm_index();
    // Skip regions already scanned directly (candidates + young/survivors)
    if (src_idx < _num_regions && _eviction_set[src_idx]) return;
    if (source_hr->is_young()) return;

    HeapWord* scan_start = MAX2(card_start, source_hr->bottom());
    HeapWord* scan_end = MIN2(card_end, source_hr->top());
    if (scan_start >= scan_end) return;

    MemRegion mr(scan_start, scan_end);
    source_hr->oops_on_memregion_seq_iterate_careful<true>(mr, _cl);
  }
};

class TagFastRefsTask : public WorkerTask {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  const bool*            _eviction_set;
  uint                   _num_regions;
  HeapWord* const*       _pre_evac_tops;
  const G1CMBitMap*      _bitmap;
  HeapRegionClaimer      _claimer;
  volatile int           _total_tagged;
  volatile int           _total_no_handle;
  volatile int           _regions_scanned;

  typedef G1RemoteMemoryManager::TaggedFieldEntry TaggedFieldEntry;
  TaggedFieldEntry** _worker_bufs;
  int*               _worker_counts;
  uint               _num_workers;

public:
  TagFastRefsTask(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                  const bool* eset, uint nregions, HeapWord* const* pre_evac_tops,
                  uint num_workers, const G1CMBitMap* bitmap)
    : WorkerTask("Tag eviction refs (fast)"),
      _rmm(rmm), _g1h(g1h), _eviction_set(eset), _num_regions(nregions),
      _pre_evac_tops(pre_evac_tops), _bitmap(bitmap),
      _claimer(num_workers), _total_tagged(0), _total_no_handle(0),
      _regions_scanned(0), _num_workers(num_workers) {
    _worker_bufs = NEW_C_HEAP_ARRAY(TaggedFieldEntry*, num_workers, mtGC);
    _worker_counts = NEW_C_HEAP_ARRAY(int, num_workers, mtGC);
    memset(_worker_bufs, 0, num_workers * sizeof(TaggedFieldEntry*));
    memset(_worker_counts, 0, num_workers * sizeof(int));
  }

  ~TagFastRefsTask() {
    for (uint i = 0; i < _num_workers; i++) {
      if (_worker_bufs[i] != nullptr) {
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _worker_bufs[i]);
      }
    }
    FREE_C_HEAP_ARRAY(TaggedFieldEntry*, _worker_bufs);
    FREE_C_HEAP_ARRAY(int, _worker_counts);
  }

  void work(uint worker_id) {
    EvictionSetTagClosure cl(_rmm, _g1h, _eviction_set, _num_regions);
    EvictionSetRsetScanner rset_scanner(_g1h, &cl, _eviction_set, _num_regions);
    int scanned = 0;

    for (uint i = _claimer.offset_for_worker(worker_id); i < _num_regions; i++) {
      if (!_claimer.claim_region(i)) continue;
      HeapRegion* hr = _g1h->region_at(i);

      if (_eviction_set[i]) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        HeapRegionRemSet* rem_set = hr->rem_set();
        if (rem_set->is_complete() && !rem_set->is_empty()) {
          rem_set->iterate_for_merge(rset_scanner);
        }
        scanned++;
        continue;
      }

      if (hr->is_young()) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        continue;
      }

      if (hr->is_old_or_humongous() && !hr->is_empty() && !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        continue;
      }
    }

    Atomic::add(&_total_tagged, cl.tagged());
    Atomic::add(&_total_no_handle, cl.no_handle());
    Atomic::add(&_regions_scanned, scanned);
    _worker_bufs[worker_id] = cl.release_local_buf();
    _worker_counts[worker_id] = cl.local_count();
  }

  void flush_to_rmm() {
    for (uint i = 0; i < _num_workers; i++) {
      for (int j = 0; j < _worker_counts[i]; j++) {
        _rmm->add_tagged_field(_worker_bufs[i][j]._field_addr,
                               _worker_bufs[i][j]._handle);
      }
    }
  }

  int total_tagged() const { return _total_tagged; }
  int total_no_handle() const { return _total_no_handle; }
  int regions_scanned() const { return _regions_scanned; }
};

int G1RemoteMemoryManager::tag_refs_to_eviction_set_fast(
    const bool* eviction_set, uint num_regions,
    HeapWord* const* pre_evac_tops,
    WorkerThreads* workers, uint num_workers) {

  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();
  int total_tagged, total_no_handle;

  if (workers != nullptr && num_workers > 1) {
    TagFastRefsTask task(this, _g1h, eviction_set, num_regions,
                         pre_evac_tops, num_workers, bitmap);
    workers->run_task(&task, num_workers);
    task.flush_to_rmm();
    total_tagged = task.total_tagged();
    total_no_handle = task.total_no_handle();

    if (total_tagged > 0 || total_no_handle > 0) {
      log_info(gc)("Fast Phase C (%u workers, %d regions scanned): tagged %d refs, %d no handle",
                   num_workers, task.regions_scanned(), total_tagged, total_no_handle);
    }
  } else {
    EvictionSetTagClosure cl(this, _g1h, eviction_set, num_regions);
    EvictionSetRsetScanner rset_scanner(_g1h, &cl, eviction_set, num_regions);
    int scanned = 0;

    for (uint i = 0; i < num_regions; i++) {
      HeapRegion* hr = _g1h->region_at(i);

      if (eviction_set[i]) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        HeapRegionRemSet* rem_set = hr->rem_set();
        if (rem_set->is_complete() && !rem_set->is_empty()) {
          rem_set->iterate_for_merge(rset_scanner);
        }
        scanned++;
        continue;
      }

      if (hr->is_young()) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        continue;
      }

      if (hr->is_old_or_humongous() && !hr->is_empty() && !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        continue;
      }
    }

    for (int j = 0; j < cl.local_count(); j++) {
      add_tagged_field(cl.local_buf()[j]._field_addr, cl.local_buf()[j]._handle);
    }
    total_tagged = cl.tagged();
    total_no_handle = cl.no_handle();

    if (total_tagged > 0 || total_no_handle > 0) {
      log_info(gc)("Fast Phase C (1 worker, %d regions scanned): tagged %d refs, %d no handle",
                   scanned, total_tagged, total_no_handle);
    }
  }

  // Phase C root scan: tag references from non-heap root sources
  // (thread stacks, JNI handles, ClassLoaderData, OopStorages)
  // NOTE: CodeCache is NOT scanned here. Nmethod oop constants are raw
  // machine-code immediates — tagging them corrupts compiled code (the
  // movabs constant becomes a non-canonical tagged address that #GPs on
  // dereference). Phase D root-catch already relocates objects referenced
  // by nmethod oops to the catch region, so no tagging is needed.
  {
    EvictionSetTagClosure root_cl(this, _g1h, eviction_set, num_regions);

    Threads::oops_do(&root_cl, nullptr);
    JNIHandles::oops_do(&root_cl);
    OopStorageSet::strong_oops_do(&root_cl);
    for (auto id : EnumRange<OopStorageSet::WeakId>()) {
      OopStorageSet::storage(id)->oops_do(&root_cl);
    }
    oops_do_remote_anchors(&root_cl);
    {
      CLDToOopClosure cld_cl(&root_cl, ClassLoaderData::_claim_none);
      ClassLoaderDataGraph::cld_do(&cld_cl);
    }

    for (int j = 0; j < root_cl.local_count(); j++) {
      add_tagged_field(root_cl.local_buf()[j]._field_addr, root_cl.local_buf()[j]._handle);
    }

    int root_tagged = root_cl.tagged();
    total_tagged += root_tagged;
    total_no_handle += root_cl.no_handle();

    if (root_tagged > 0 || root_cl.no_handle() > 0) {
      log_info(gc)("Phase C root scan: tagged %d refs, %d no handle",
                   root_tagged, root_cl.no_handle());
    }

    TaggedFieldEntry* buf = root_cl.release_local_buf();
    if (buf != nullptr) FREE_C_HEAP_ARRAY(TaggedFieldEntry, buf);
  }

  return total_tagged;
}

int G1RemoteMemoryManager::untag_all_heap_refs(WorkerThreads* workers, uint num_workers) {
  class UntagClosure : public BasicOopIterateClosure {
    int _untagged;
  public:
    UntagClosure() : _untagged(0) {}

    virtual void do_oop(oop* p) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) return;
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) !=
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

      RemoteHandle* h = (RemoteHandle*)(raw & G1_OOP_ADDR_MASK);
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) {
        uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
        *(uintptr_t*)p = addr;
        _untagged++;
      }
    }
    virtual void do_oop(narrowOop* p) { }
    int untagged() const { return _untagged; }
  };

  UntagClosure cl;
  for (uint i = 0; i < _g1h->num_regions(); i++) {
    HeapRegion* hr = _g1h->region_at(i);
    if (hr->is_empty() || hr->is_free()) continue;
    HeapWord* p = hr->bottom();
    HeapWord* region_end = hr->end();
    while (p < hr->top()) {
      if (p < hr->bottom() || p >= region_end) break;
      oop obj = cast_to_oop(p);
      Klass* k = obj->klass_or_null();
      if (k == nullptr) break;
      size_t sz = obj->size();
      if (sz == 0 || sz > (size_t)(region_end - p)) break;
      obj->oop_iterate(&cl);
      p += sz;
    }
  }

  if (cl.untagged() > 0) {
    log_info(gc)("Untag cleanup: restored %d tagged refs to clean oops", cl.untagged());
  }
  return cl.untagged();
}

int G1RemoteMemoryManager::verify_no_untagged_refs_to_eviction_set(
    const bool* eviction_set, uint num_regions) {

  class VerifyTagClosure : public BasicOopIterateClosure {
    G1CollectedHeap*       _g1h;
    const bool*            _eviction_set;
    uint                   _num_regions;
    int                    _missed;
    oop                    _cur_obj;
  public:
    VerifyTagClosure(G1CollectedHeap* g1h, const bool* eset, uint nregions)
      : _g1h(g1h), _eviction_set(eset), _num_regions(nregions),
        _missed(0), _cur_obj(nullptr) {}

    void set_cur_obj(oop obj) { _cur_obj = obj; }

    virtual void do_oop(oop* p) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) return;
      // Shared oops (bits 63+62) already go through a Handle — OK.
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

      oop target;
      if ((raw >> 63) != 0) {
        target = cast_to_oop(raw & G1_OOP_ADDR_MASK);
      } else {
        target = cast_to_oop(raw);
      }
      if (!_g1h->is_in(target)) return;

      HeapRegion* target_hr = _g1h->heap_region_containing(target);
      uint idx = target_hr->hrm_index();
      if (idx >= _num_regions || !_eviction_set[idx]) return;

      // Root-catch relocated objects have forwarding pointers — OK
      if (target->is_forwarded()) return;

      _missed++;
      if (_missed <= 20) {
        HeapRegion* src_hr = (_cur_obj != nullptr && _g1h->is_in(_cur_obj))
          ? _g1h->heap_region_containing(_cur_obj) : nullptr;
        log_warning(gc)("VERIFY: untagged ref field=" PTR_FORMAT " -> target=" PTR_FORMAT
                        " in candidate region %u, src_obj=" PTR_FORMAT " klass=%s src_region=%u"
                        " raw=0x%lx",
                        p2i(p), p2i((void*)target), idx,
                        p2i((void*)_cur_obj),
                        (_cur_obj != nullptr ? _cur_obj->klass()->external_name() : "root"),
                        (src_hr != nullptr ? src_hr->hrm_index() : 9999),
                        (unsigned long)raw);
      }
    }

    virtual void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }

    int missed() const { return _missed; }
  };

  VerifyTagClosure cl(_g1h, eviction_set, num_regions);
  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();

  // 1. Verify heap: use SAME walk as Phase C tagging to avoid blind spots
  for (uint i = 0; i < _g1h->num_regions(); i++) {
    HeapRegion* hr = _g1h->region_at(i);
    if (hr->is_empty() || hr->is_free()) continue;
    if (hr->is_continues_humongous()) continue;

    HeapWord* const pb = hr->parsable_bottom_acquire();
    HeapWord* const region_top = hr->top();
    HeapWord* const region_end = hr->end();

    HeapWord* p = hr->bottom();
    while (p < pb && p < region_top) {
      if (bitmap->is_marked(p)) {
        oop obj = cast_to_oop(p);
        size_t sz = obj->size();
        cl.set_cur_obj(obj);
        obj->oop_iterate(&cl);
        p += sz;
      } else {
        p = bitmap->get_next_marked_addr(p, pb);
      }
    }

    if (p < pb) p = pb;
    while (p < region_top) {
      if (p >= region_end) break;
      oop obj = cast_to_oop(p);
      Klass* k = obj->klass_or_null();
      if (k == nullptr) {
        log_warning(gc)("VERIFY scan TRUNCATED: region %u null klass at " PTR_FORMAT
                        " (pb=" PTR_FORMAT " top=" PTR_FORMAT ")",
                        hr->hrm_index(), p2i(p), p2i(pb), p2i(region_top));
        break;
      }
      size_t sz = obj->size();
      if (sz == 0) {
        log_warning(gc)("VERIFY scan TRUNCATED: region %u zero size at " PTR_FORMAT
                        " klass=%s", hr->hrm_index(), p2i(p), k->external_name());
        break;
      }
      cl.set_cur_obj(obj);
      if (sz > (size_t)(region_end - p)) {
        obj->oop_iterate(&cl);
        break;
      }
      obj->oop_iterate(&cl);
      p += sz;
    }
  }

  int heap_missed = cl.missed();

  // 2. Verify roots (informational only — root refs are handled by
  // root-catch relocation and Pre-E guard, not by Phase C tagging).
  cl.set_cur_obj(nullptr);
  Threads::oops_do(&cl, nullptr);
  JNIHandles::oops_do(&cl);
  OopStorageSet::strong_oops_do(&cl);
  for (auto id : EnumRange<OopStorageSet::WeakId>()) {
    OopStorageSet::storage(id)->oops_do(&cl);
  }
  oops_do_remote_anchors(&cl);
  {
    CLDToOopClosure cld_cl(&cl, ClassLoaderData::_claim_none);
    ClassLoaderDataGraph::cld_do(&cld_cl);
  }
  _g1h->ref_processor_cm()->weak_oops_do(&cl);

  int root_missed = cl.missed() - heap_missed;
  if (heap_missed > 0) {
    log_warning(gc)("VERIFY: %d untagged HEAP refs to eviction candidates AFTER tagging!",
                    heap_missed);
  }
  if (root_missed > 0) {
    log_info(gc)("VERIFY: %d root refs to candidates (handled by Pre-E guard, not Phase C)",
                 root_missed);
  }
  return heap_missed;
}

int G1RemoteMemoryManager::verify_no_stale_refs_to_freed_regions() {
  class StaleRefSweepClosure : public BasicOopIterateClosure {
    G1CollectedHeap* _g1h;
    int              _stale;
    oop              _cur_obj;
    bool             _is_root;
  public:
    StaleRefSweepClosure(G1CollectedHeap* g1h)
      : _g1h(g1h), _stale(0), _cur_obj(nullptr), _is_root(false) {}

    void set_cur_obj(oop obj) { _cur_obj = obj; _is_root = false; }
    void set_root_mode() { _cur_obj = nullptr; _is_root = true; }

    virtual void do_oop(oop* p) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) return;
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

      oop target;
      if ((raw >> 63) != 0) {
        target = cast_to_oop(raw & G1_OOP_ADDR_MASK);
      } else {
        target = cast_to_oop(raw);
      }
      if (!_g1h->is_in(target)) {
        // Non-heap, non-tagged value in an oop slot — heap corruption
        _stale++;
        if (_stale <= 50) {
          const char* src_kind = _is_root ? "ROOT" : "HEAP";
          const char* src_klass = "?";
          uint src_region = 9999;
          if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
            Klass* sk = _cur_obj->klass_or_null();
            if (sk != nullptr) src_klass = sk->external_name();
            src_region = _g1h->heap_region_containing(_cur_obj)->hrm_index();
          }
          uint32_t off = (_cur_obj != nullptr) ?
            (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_cur_obj)) : 0;
          log_warning(gc)("CORRUPT-OOP [%s]: field=" PTR_FORMAT " raw=0x%lx NOT IN HEAP"
                          " (src_obj=" PTR_FORMAT " klass=%s region=%u offset=%u)",
                          src_kind, p2i(p), (unsigned long)raw,
                          p2i((void*)_cur_obj), src_klass, src_region, off);
          // Decode as markWord to detect header-scribble (Codex H6 hypothesis)
          uintptr_t mw_lock = raw & 0x3;
          uintptr_t mw_age = (raw >> 3) & 0xF;
          uintptr_t mw_hash = (raw >> 8) & 0x7FFFFFF;
          if (mw_lock == 0x1 && raw > 0xFF) {
            log_warning(gc)("  ^^ LOOKS LIKE MARK WORD: lock=unlocked age=%u hash=0x%07x"
                            " upper=0x%lx — possible header copied into oop slot",
                            (unsigned)mw_age, (unsigned)mw_hash,
                            (unsigned long)(raw >> 35));
          }
        }
        return;
      }

      HeapRegion* hr = _g1h->heap_region_containing(target);
      if (hr == nullptr) return;

      bool is_stale = hr->is_free() || hr->is_evict_guarded();
      if (!is_stale) {
        Klass* k = cast_to_oop(target)->klass_or_null();
        if (k == nullptr) is_stale = true;
      }

      if (is_stale) {
        _stale++;
        if (_stale <= 50) {
          const char* src_kind = _is_root ? "ROOT" : "HEAP";
          HeapRegion* src_hr = nullptr;
          const char* src_klass = "?";
          uint src_region = 9999;
          bool has_tagged_fields = false;
          if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
            src_hr = _g1h->heap_region_containing(_cur_obj);
            src_region = src_hr->hrm_index();
            Klass* sk = _cur_obj->klass_or_null();
            if (sk != nullptr) src_klass = sk->external_name();
            // Check if source obj has any tagged fields (indicates FCR-fetched)
            HeapWord* obj_start = (HeapWord*)_cur_obj;
            HeapWord* obj_end = obj_start + _cur_obj->size();
            for (HeapWord* w = obj_start + 2; w < obj_end; w++) {
              uintptr_t v = *(uintptr_t*)w;
              if (v & G1_OOP_MANAGED_BIT) { has_tagged_fields = true; break; }
            }
          }
          G1CardTable* ct = _g1h->card_table();
          G1CardTable::CardValue card_val = *ct->byte_for((HeapWord*)p);
          log_warning(gc)("STALE-REF-SWEEP [%s]: field=" PTR_FORMAT " raw=0x%lx -> target="
                          PTR_FORMAT " in %s region %u (src_obj=" PTR_FORMAT " klass=%s region=%u"
                          " card=0x%02x fcr_tagged=%s src_type=%s)",
                          src_kind, p2i(p), (unsigned long)raw,
                          p2i((void*)target),
                          hr->is_evict_guarded() ? "GUARDED" : "FREE",
                          hr->hrm_index(),
                          p2i((void*)_cur_obj), src_klass, src_region,
                          (unsigned)card_val,
                          has_tagged_fields ? "yes" : "no",
                          src_hr != nullptr ? src_hr->get_short_type_str() : "?");

          // Dump all oop-width slots on the same card as this stale ref
          if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
            HeapWord* card_start = ct->addr_for(ct->byte_for((HeapWord*)p));
            HeapWord* card_end = card_start + G1CardTable::card_size_in_words();
            HeapWord* obj_start = (HeapWord*)_cur_obj;
            HeapWord* obj_end = obj_start + _cur_obj->size();
            // Clamp to object bounds (skip header: mark + klass = 2 words)
            HeapWord* scan_start = MAX2(card_start, obj_start + 2);
            HeapWord* scan_end = MIN2(card_end, obj_end);
            int n_null = 0, n_tagged = 0, n_live = 0, n_stale_card = 0;
            for (HeapWord* w = scan_start; w < scan_end; w++) {
              uintptr_t v = *(uintptr_t*)w;
              if (v == 0) { n_null++; continue; }
              if ((v & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
                  (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) { n_tagged++; continue; }
              oop slot_target;
              if ((v >> 63) != 0) { slot_target = cast_to_oop(v & G1_OOP_ADDR_MASK); }
              else { slot_target = cast_to_oop(v); }
              if (!_g1h->is_in(slot_target)) { continue; }
              HeapRegion* slot_hr = _g1h->heap_region_containing(slot_target);
              if (slot_hr != nullptr && (slot_hr->is_free() || slot_hr->is_evict_guarded())) {
                n_stale_card++;
              } else {
                n_live++;
              }
            }
            log_warning(gc)("STALE-REF-SWEEP card dump: card=[" PTR_FORMAT "," PTR_FORMAT
                            ") obj=[" PTR_FORMAT "," PTR_FORMAT
                            ") scan=[" PTR_FORMAT "," PTR_FORMAT
                            "): %d null, %d tagged, %d live, %d stale (of %d slots)",
                            p2i(card_start), p2i(card_end),
                            p2i(obj_start), p2i(obj_end),
                            p2i(scan_start), p2i(scan_end),
                            n_null, n_tagged, n_live, n_stale_card,
                            (int)(scan_end - scan_start));
          }
        }
      }
    }
    virtual void do_oop(narrowOop* p) {}
    int stale() const { return _stale; }
  };

  Ticks start = Ticks::now();
  StaleRefSweepClosure cl(_g1h);
  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();
  int regions_scanned = 0;

  for (uint i = 0; i < _g1h->num_regions(); i++) {
    HeapRegion* hr = _g1h->region_at(i);
    if (hr->is_empty() || hr->is_free()) continue;
    if (hr->is_continues_humongous()) continue;
    if (hr->is_evict_guarded()) continue;

    HeapWord* const pb = hr->parsable_bottom_acquire();
    HeapWord* const region_top = hr->top();
    HeapWord* const region_end = hr->end();

    HeapWord* p = hr->bottom();
    while (p < pb && p < region_top) {
      if (bitmap->is_marked(p)) {
        oop obj = cast_to_oop(p);
        size_t sz = obj->size();
        cl.set_cur_obj(obj);
        obj->oop_iterate(&cl);
        p += sz;
      } else {
        p = bitmap->get_next_marked_addr(p, pb);
      }
    }

    if (p < pb) p = pb;
    while (p < region_top) {
      if (p >= region_end) break;
      oop obj = cast_to_oop(p);
      Klass* k = obj->klass_or_null();
      if (k == nullptr) break;
      uintptr_t klass_raw = (uintptr_t)k;
      if (klass_raw < 0x10000 || (klass_raw >> 47) != 0) {
        log_warning(gc)("CORRUPT-KLASS: obj=" PTR_FORMAT " region=%u klass_raw=0x%lx"
                        " — stopping region scan",
                        p2i(p), hr->hrm_index(), (unsigned long)klass_raw);
        break;
      }
      size_t sz = obj->size();
      if (sz == 0) break;
      cl.set_cur_obj(obj);
      if (sz > (size_t)(region_end - p)) {
        obj->oop_iterate(&cl);
        break;
      }
      obj->oop_iterate(&cl);
      p += sz;
    }
    regions_scanned++;
  }

  int heap_stale = cl.stale();

  cl.set_root_mode();
  Threads::oops_do(&cl, nullptr);
  JNIHandles::oops_do(&cl);
  OopStorageSet::strong_oops_do(&cl);
  {
    CLDToOopClosure cld_cl(&cl, ClassLoaderData::_claim_none);
    ClassLoaderDataGraph::cld_do(&cld_cl);
  }
  {
    CodeBlobToOopClosure code_cl(&cl, false);
    CodeCache::blobs_do(&code_cl);
  }
  _g1h->ref_processor_cm()->weak_oops_do(&cl);

  int root_stale = cl.stale() - heap_stale;
  double elapsed_ms = (Ticks::now() - start).seconds() * 1000.0;

  if (cl.stale() > 0) {
    log_warning(gc)("STALE-REF-SWEEP: %d stale refs found (%d heap, %d root) in %.1fms "
                    "(%d regions scanned)",
                    cl.stale(), heap_stale, root_stale, elapsed_ms, regions_scanned);
  } else {
    log_info(gc)("STALE-REF-SWEEP: clean (0 stale refs) in %.1fms (%d regions scanned)",
                 elapsed_ms, regions_scanned);
  }
  return cl.stale();
}

bool G1RemoteMemoryManager::validate_anchor_addr(RemoteHandle* h) {
  void* addr = h->local_addr();
  if (addr == nullptr) return false;
  if (!_g1h->is_in(addr)) {
    log_warning(gc)("STALE-ANCHOR: handle=" PTR_FORMAT " addr=" PTR_FORMAT
                    " NOT IN HEAP — marking DEAD (rc=%u)",
                    p2i(h), p2i(addr), h->remote_refcount());
    h->set_dead();
    return false;
  }
  HeapRegion* hr = _g1h->heap_region_containing(addr);
  if (hr->is_free() || hr->is_evict_guarded()) {
    log_warning(gc)("STALE-ANCHOR: handle=" PTR_FORMAT " addr=" PTR_FORMAT
                    " in %s region %u — marking DEAD (rc=%u)",
                    p2i(h), p2i(addr),
                    hr->is_evict_guarded() ? "GUARDED" : "FREE",
                    hr->hrm_index(), h->remote_refcount());
    h->set_dead();
    return false;
  }
  return true;
}

int G1RemoteMemoryManager::count_local_handles_in_region(HeapRegion* hr, int log_limit) {
  if (hr == nullptr) return 0;

  uintptr_t bottom = (uintptr_t)hr->bottom();
  uintptr_t end = (uintptr_t)hr->end();
  int count = 0;

  table_lock();
  for (size_t i = 0; i < TABLE_SIZE; i++) {
    HandleEntry* e = _table[i];
    while (e != nullptr) {
      RemoteHandle* h = e->_handle;
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) {
        uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
        if (addr >= bottom && addr < end) {
          count++;
          if (count <= log_limit) {
            log_warning(gc)("LOCAL handle blocks eviction free: region=%u handle=" PTR_FORMAT
                            " local=" PTR_FORMAT " table_addr=" PTR_FORMAT
                            " dormant=%d rc=%u",
                            hr->hrm_index(), p2i(h), addr, e->_obj_addr,
                            h->is_dormant() ? 1 : 0, h->remote_refcount());
          }
        }
      }
      e = e->_next;
    }
  }
  table_unlock();

  return count;
}

Klass* G1RemoteMemoryManager::fetch_remote_object(RemoteHandle* h, void* dest) {
  assert(h != nullptr, "Handle must not be null");

  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
  assert(state == REMOTE_HANDLE_REMOTE || state == REMOTE_HANDLE_FETCHING,
         "Handle must be REMOTE or FETCHING");

  size_t slot_id = sa & REMOTE_HANDLE_ADDR_MASK;

  // Fetch object bytes via backend (SIM/TCP/RDMA)
  size_t word_size = 0;
  Klass* klass = _backend->fetch(slot_id, dest, &word_size);

  if (klass != nullptr) {
    size_t expected_ws = h->eviction_word_size();
    if (word_size != expected_ws) {
      log_warning(gc)("Remote fetch size MISMATCH: slot=" SIZE_FORMAT " expected=" SIZE_FORMAT "w got=" SIZE_FORMAT "w — aborting fetch to prevent type confusion",
                       slot_id, expected_ws, word_size);
      return nullptr;
    }
    log_trace(gc)("Remote fetch: slot=" SIZE_FORMAT " -> dest=" PTR_FORMAT " klass=%s size=" SIZE_FORMAT "w",
                  slot_id, p2i(dest), klass->external_name(), word_size);
  } else {
    log_warning(gc)("Remote fetch FAILED: slot=" SIZE_FORMAT, slot_id);
  }

  return klass;
}

// ============================================================
// Post-Fetch Field Patching
// ============================================================
// After fetching remote bytes into FCR, patch oop fields using the
// sidecar edge table. The fetched bytes contain oop values from eviction
// time — targets may have moved or died since then. The edge table maps
// each oop field offset to the target's Handle, which tracks the
// current address.
//
// Must be called BEFORE set_local_release() — the fetched object must
// not be visible to other threads until all fields are patched.

void G1RemoteMemoryManager::patch_fetched_fields(RemoteHandle* source_handle, HeapWord* dest) {
  ObjectEdgeTable* et = edge_table_for(source_handle);
  if (et == nullptr) {
    // No edge table — object had no oop fields at eviction time.
    // Or edge table was already cleaned up. Nothing to patch.
    return;
  }

  uintptr_t base = (uintptr_t)dest;
  int patched = 0;
  bool cm_active = concurrent_marking_active();

  size_t obj_byte_size = et->_eviction_word_size * HeapWordSize;

  for (uint32_t i = 0; i < et->_entry_count; i++) {
    EdgeEntry& edge = et->_entries[i];
    guarantee(edge._field_offset >= 16,
              "Edge table offset %u would corrupt object header", edge._field_offset);
    guarantee(edge._field_offset + sizeof(uintptr_t) <= obj_byte_size,
              "Edge table offset %u + %zu overflows object of %zu bytes",
              edge._field_offset, sizeof(uintptr_t), obj_byte_size);
    uintptr_t* field_addr = (uintptr_t*)(base + edge._field_offset);
    RemoteHandle* target = edge._target_handle;

    uintptr_t sa = target->load_state_and_addr_acquire();
    uintptr_t target_state = sa & REMOTE_HANDLE_STATE_MASK;

    if (target_state == REMOTE_HANDLE_DEAD) {
      *field_addr = 0;
      patched++;
      if (cm_active) { defer_refcount_decrement(target); } else { target->decrement_remote_refcount(); }
    } else {
      // LOCAL, REMOTE, or FETCHING — write shared_oop(handle).
      // The load barrier resolves through the handle on every access,
      // so the field stays correct even if the target moves during GC.
      // Writing clean oops here would require card dirtying + RSet updates
      // to keep the reference current — shared_oop avoids that fragility.
      *field_addr = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)target;
      patched++;
    }
  }

  // Enqueue dirty cards covering the fetched object into the G1 dirty card
  // queue. dirty_MemRegion alone only sets card bytes — G1 concurrent
  // refinement and STW merging only process cards from the dirty card queue.
  // Without enqueuing, cross-region refs from this FCR object are never
  // added to remembered sets, so GC won't find or update them.
  if (patched > 0) {
    G1CardTable* ct = _g1h->card_table();
    G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
    Thread* thr = Thread::current();
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thr);
    CardTable::CardValue* first = ct->byte_for(dest);
    CardTable::CardValue* last = ct->byte_for(dest + et->_eviction_word_size - 1);
    for (CardTable::CardValue* card = first; card <= last; card++) {
      if (*card != G1CardTable::g1_young_card_val()) {
        *card = G1CardTable::dirty_card_val();
        qset.enqueue(queue, card);
      }
    }
  }

  log_debug(gc)("Fetch patch: handle=" PTR_FORMAT " dest=" PTR_FORMAT " patched=%d/%u fields",
                p2i(source_handle), p2i(dest), patched, et->_entry_count);

  // Remove edge table — no longer needed after fetch
  remove_edge_table(source_handle);
}

// ============================================================
// Full GC Handle Update
// ============================================================
// After Full GC phase 3 (adjust pointers), forwarding addresses are
// installed in mark words. Walk the Handle table and update each
// LOCAL Handle to point to the forwarded address. Must be called
// before phase 4 (compaction) moves the bytes.

void G1RemoteMemoryManager::update_handles_for_full_gc() {
  int updated = 0;

  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      HandleEntry* next = e->_next;
      RemoteHandle* h = e->_handle;

      if (h != nullptr && h->is_local()) {
        // Verify the object address is still in the heap before accessing.
        // During Full GC, dead objects may have been reclaimed.
        if (!_g1h->is_in((void*)e->_obj_addr)) {
          e = next;
          continue;
        }
        oop obj = cast_to_oop(e->_obj_addr);
        if (obj->is_forwarded()) {
          oop new_obj = obj->forwardee();
          uintptr_t new_addr = cast_from_oop<uintptr_t>(new_obj);

          // Update Handle to new address
          h->set_local(cast_from_oop<void*>(new_obj));

          // Rekey table entry: unlink from old bucket, insert in new
          // (we can't modify while iterating, so update in-place)
          e->_obj_addr = new_addr;
          updated++;
        }
      }
      e = next;
    }
  }

  // Rekey: some entries may now be in the wrong hash bucket.
  // Rebuild the table from the entries (simple for prototype).
  if (updated > 0) {
    // Collect all entries
    HandleEntry* all_entries = nullptr;
    for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
      HandleEntry* e = _table[idx];
      while (e != nullptr) {
        HandleEntry* next = e->_next;
        e->_next = all_entries;
        all_entries = e;
        e = next;
      }
      _table[idx] = nullptr;
    }
    // Re-insert all entries with new keys
    HandleEntry* e = all_entries;
    while (e != nullptr) {
      HandleEntry* next = e->_next;
      size_t new_idx = hash_obj(e->_obj_addr);
      e->_next = _table[new_idx];
      _table[new_idx] = e;
      e = next;
    }
  }
  table_unlock();

  if (updated > 0) {
    log_info(gc)("Full GC handle update: %d handles rekeyed", updated);
  }
}

// ============================================================
// Fetch Cache Region (FCR) Allocation
// ============================================================

HeapRegion* G1RemoteMemoryManager::allocate_new_fcr_region() {
  return _g1h->allocate_fcr_region();
}

// ============================================================
// Remote Collection — free dead remote objects without fetching
// ============================================================
// Walk the Handle table. For each remote object (Handle in REMOTE state),
// check if the original local object is still alive (marked in the concurrent
// marking bitmap). If dead: free the sim-remote slot + Handle entry.
// If alive: keep (will be fetched lazily on next access).
//
// Key principle: dead objects' bytes NEVER cross the network.

size_t G1RemoteMemoryManager::collect_dead_remote_objects() {
  // Build deduplicated root set from three sources:
  //   1. CM roots (from concurrent marking — already in _remote_roots)
  //   2. Phase C tagged field handles (shared_oops in heap)
  //   3. REMOTE handles with remote_refcount > 0 (edge-table references)
  //
  // Use a power-of-2 hash set for O(1) dedup. Sized to 2x expected entries.
  size_t total_remote = 0;
  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      if (e->_handle != nullptr && e->_handle->is_remote()) {
        total_remote++;
      }
    }
  }
  table_unlock();

  // Hash set for dedup: open addressing with linear probing
  size_t set_capacity = 1;
  while (set_capacity < (total_remote + _remote_roots_count) * 2 + 64) {
    set_capacity <<= 1;
  }
  uintptr_t* dedup_set = NEW_C_HEAP_ARRAY(uintptr_t, set_capacity, mtGC);
  memset(dedup_set, 0, set_capacity * sizeof(uintptr_t));
  size_t set_mask = set_capacity - 1;

  // Collect unique roots into _remote_roots (dynamically grown)
  int cm_count = _remote_roots_count;  // CM roots already present
  int old_count = _remote_roots_count;

  // Insert existing CM roots into dedup set
  for (int i = 0; i < cm_count; i++) {
    uintptr_t id = _remote_roots[i];
    size_t slot = (id >> 4) & set_mask;
    while (dedup_set[slot] != 0 && dedup_set[slot] != id) {
      slot = (slot + 1) & set_mask;
    }
    dedup_set[slot] = id;
  }

  // Source 2: Phase C tagged field handles
  int phase_c_added = 0;
  for (int i = 0; i < _tagged_field_count; i++) {
    RemoteHandle* h = _tagged_fields[i]._handle;
    if (h != nullptr && h->is_remote()) {
      uintptr_t id = (uintptr_t)h;
      size_t slot = (id >> 4) & set_mask;
      while (dedup_set[slot] != 0 && dedup_set[slot] != id) {
        slot = (slot + 1) & set_mask;
      }
      if (dedup_set[slot] == 0) {
        dedup_set[slot] = id;
        add_remote_root(id);
        phase_c_added++;
      }
    }
  }

  // Source 3: REMOTE handles with remote_refcount > 0
  int refcount_added = 0;
  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      if (e->_handle != nullptr && e->_handle->is_remote() &&
          e->_handle->remote_refcount() > 0) {
        uintptr_t id = (uintptr_t)e->_handle;
        size_t slot = (id >> 4) & set_mask;
        while (dedup_set[slot] != 0 && dedup_set[slot] != id) {
          slot = (slot + 1) & set_mask;
        }
        if (dedup_set[slot] == 0) {
          dedup_set[slot] = id;
          add_remote_root(id);
          refcount_added++;
        }
      }
    }
  }
  table_unlock();
  FREE_C_HEAP_ARRAY(uintptr_t, dedup_set);

  log_info(gc)("collect_dead: roots: %d CM + %d tagged-fields + %d refcount = %d unique "
               "(%zu remote handles)",
               cm_count, phase_c_added, refcount_added, _remote_roots_count, total_remote);

  if (_remote_roots_count == 0) {
    log_info(gc)("collect_dead: SKIP (no roots, %zu remote handles retained)", total_remote);
    return 0;
  }

  log_info(gc)("collect_dead: report_remote_roots_v2 (%d roots, %zu remote handles)",
               _remote_roots_count, total_remote);
  _backend->report_remote_roots_v2(_remote_roots, _remote_roots_count);
  log_info(gc)("collect_dead: report_remote_roots_v2 DONE");

  // trace_and_report — get dead handles + cross-boundary edges
  uintptr_t* dead_ids = nullptr;
  size_t num_dead = 0;
  size_t bytes_freed = 0;
  uintptr_t* cross_src = nullptr;
  uintptr_t* cross_tgt = nullptr;
  size_t num_cross = 0;

  log_info(gc)("collect_dead: trace_and_report START");
  _backend->trace_and_report(&dead_ids, &num_dead, &bytes_freed,
                             &cross_src, &cross_tgt, &num_cross);
  log_info(gc)("collect_dead: trace_and_report DONE (dead=%zu freed=%zu cross=%zu)",
               num_dead, bytes_freed, num_cross);

  // Step 2c: Populate cross-boundary roots.
  // Cross-edges: live REMOTE handle → LOCAL handle.
  // The LOCAL targets must be rooted during GC to prevent collection.
  _cross_roots_count = 0;
  if (num_cross > 0) {
    table_lock();
    for (size_t i = 0; i < num_cross && _cross_roots_count < MAX_CROSS_ROOTS; i++) {
      uintptr_t local_handle_id = cross_tgt[i];
      // Find the RemoteHandle by handle_id (address of Handle)
      RemoteHandle* h = (RemoteHandle*)local_handle_id;
      if (h != nullptr && h->is_local()) {
        _cross_roots[_cross_roots_count++] = h;
      }
    }
    table_unlock();
    log_info(gc)("Cross-boundary roots: %d LOCAL handles kept alive by live REMOTE objects",
                 _cross_roots_count);
  }

  if (cross_src) os::free(cross_src);
  if (cross_tgt) os::free(cross_tgt);

  // Step 3: Log dead handles but do NOT free them yet.
  // Freeing handles while shared_oops in the heap still reference them causes
  // SIGSEGV: mutators/GC closures dereference stale tagged oops to freed memory.
  // _tagged_fields doesn't capture all references (stack oops, moved objects).
  // Safe collection requires a full-heap scan to clear all shared_oops first.
  // TODO: implement full-heap dead-handle sweep before freeing handles.
  if (dead_ids) os::free(dead_ids);

  size_t retained = total_remote;
  if (num_dead > 0 || retained > 0) {
    log_info(gc)("Remote collection: %zu dead identified (NOT freed — unsafe), "
                 "%zu live, %zu total remote, %d cross-boundary roots",
                 num_dead, retained - num_dead, retained, _cross_roots_count);
  }
  return 0;
}

int G1RemoteMemoryManager::fixup_tagged_field_handles() {
  int updated = 0;
  int removed = 0;
  int write_idx = 0;

  for (int i = 0; i < _tagged_field_count; i++) {
    oop* field_addr = _tagged_fields[i]._field_addr;
    RemoteHandle* h = _tagged_fields[i]._handle;

    // field_addr may be in an evicted (mprotected) region — skip without reading
    HeapRegion* field_hr = _g1h->heap_region_containing((HeapWord*)field_addr);
    if (field_hr != nullptr && (field_hr->is_free() || field_hr->is_evict_guarded())) {
      removed++;
      continue;
    }

    uintptr_t raw = *(uintptr_t*)field_addr;

    // Stale entry: field no longer tagged or points to a different Handle
    if ((raw & G1_OOP_INDIRECT_BIT) == 0 ||
        (RemoteHandle*)(raw & G1_OOP_ADDR_MASK) != h) {
      removed++;
      continue;
    }

    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    // Handle must be LOCAL for fixup (REMOTE/FETCHING/DEAD don't need it)
    if (state != REMOTE_HANDLE_LOCAL) {
      _tagged_fields[write_idx++] = _tagged_fields[i];
      continue;
    }

    HeapWord* target = (HeapWord*)(sa & REMOTE_HANDLE_ADDR_MASK);
    oop target_oop = cast_to_oop(target);

    // Check if target has been forwarded (mark word contains forwarding ptr)
    if (_g1h->is_in(target_oop)) {
      HeapRegion* target_hr = _g1h->heap_region_containing(target);
      if (target_hr != nullptr && (target_hr->is_free() || target_hr->is_evict_guarded())) {
        removed++;
        continue;
      }
      markWord m = target_oop->mark();
      if (m.is_marked()) {
        oop forwardee = cast_to_oop(m.decode_pointer());
        h->set_local_release((void*)cast_from_oop<uintptr_t>(forwardee));
        updated++;
      }
    }

    _tagged_fields[write_idx++] = _tagged_fields[i];
  }

  _tagged_field_count = write_idx;

  if (updated > 0 || removed > 0) {
    log_info(gc)("Tagged field fixup: %d handles updated, %d stale entries removed, %d entries remaining",
                 updated, removed, _tagged_field_count);
  }
  return updated;
}

int G1RemoteMemoryManager::fixup_all_local_handles() {
  int updated = 0;
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      RemoteHandle* h = e->_handle;
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state != REMOTE_HANDLE_LOCAL) continue;

      uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
      if (addr == 0) continue;
      oop target = cast_to_oop(addr);
      if (!_g1h->is_in(target)) continue;

      HeapRegion* hr = _g1h->heap_region_containing(target);
      if (hr != nullptr && (hr->is_free() || hr->is_evict_guarded())) continue;

      markWord m = target->mark();
      if (m.is_marked()) {
        oop forwardee = cast_to_oop(m.decode_pointer());
        h->set_local_release((void*)cast_from_oop<uintptr_t>(forwardee));
        updated++;
      }
    }
  }
  if (updated > 0) {
    log_info(gc)("Handle table fixup: %d LOCAL handles updated for forwarded objects", updated);
  }
  return updated;
}

// Closure that fixes untagged refs to cset regions by writing forwardees.
class CSetRefFixupClosure : public BasicOopIterateClosure {
  G1CollectedHeap* _g1h;
  int _fixed;
  int _skipped;
  int _fcr_nulls;       // Subset of NULL fixes whose source is in FCR
  bool _src_is_fcr;     // Set per object via set_src_is_fcr()
public:
  CSetRefFixupClosure(G1CollectedHeap* g1h)
    : _g1h(g1h), _fixed(0), _skipped(0), _fcr_nulls(0), _src_is_fcr(false) {}

  void set_src_is_fcr(bool v) { _src_is_fcr = v; }

  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;
    if ((raw >> 63) != 0) return; // tagged — skip
    if (!_g1h->is_in((void*)raw)) return;
    oop target = cast_to_oop(raw);
    const G1HeapRegionAttr attr = _g1h->region_attr(target);
    if (!attr.is_in_cset()) return;
    markWord mw = target->mark();
    if (mw.is_marked()) {
      oop fwd = cast_to_oop(mw.decode_pointer());
      RawAccess<IS_NOT_NULL>::oop_store(p, fwd);
      _fixed++;
    } else {
      // Target in CSet but not forwarded → DEAD. Evac-failed objects
      // forward-to-self (handle_evacuation_failure_par at g1ParScanThreadState.cpp:973),
      // which sets is_marked()==true. So an unmarked CSet target was unreachable
      // and its region will be freed by FreeCollectionSetTask in post_evacuate_cleanup_2.
      // Null the dangling ref before that happens to prevent stale pointers.
      *(uintptr_t*)p = 0;
      _fixed++;
      if (_src_is_fcr) {
        _fcr_nulls++;
        _g1h->remote_memory_manager()->record_fcr_fixup_null();
      }
    }
  }
  virtual void do_oop(narrowOop* p) {}
  int fixed() const { return _fixed; }
  int skipped() const { return _skipped; }
  int fcr_nulls() const { return _fcr_nulls; }
};

int G1RemoteMemoryManager::fixup_stale_refs_in_old_regions() {
  CSetRefFixupClosure cl(_g1h);
  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();

  for (uint i = 0; i < _g1h->num_regions(); i++) {
    HeapRegion* hr = _g1h->region_at(i);
    if (hr->is_empty() || hr->is_free()) continue;
    if (hr->is_continues_humongous()) continue;
    if (!hr->is_old() && !hr->is_starts_humongous()) continue;

    cl.set_src_is_fcr(hr->is_fetch_cache());

    HeapWord* const pb = hr->parsable_bottom_acquire();
    HeapWord* const region_top = hr->top();
    HeapWord* const region_end = hr->end();

    // Below pb: use bitmap
    HeapWord* p = hr->bottom();
    while (p < pb && p < region_top) {
      if (bitmap->is_marked(p)) {
        oop obj = cast_to_oop(p);
        obj->oop_iterate(&cl);
        p += obj->size();
      } else {
        p = bitmap->get_next_marked_addr(p, pb);
      }
    }

    // Above pb: sequential scan
    if (p < pb) p = pb;
    while (p < region_top) {
      if (p >= region_end) break;
      oop obj = cast_to_oop(p);
      Klass* k = obj->klass_or_null();
      if (k == nullptr) break;
      size_t sz = obj->size();
      if (sz == 0) break;
      obj->oop_iterate(&cl);
      p += sz;
    }
  }

  if (cl.fixed() > 0 || cl.skipped() > 0) {
    log_warning(gc)("Old/humongous-region stale-ref fixup: %d fixed (forwardee or null), %d skipped"
                    " (FCR-source NULLs: %d / total-evac FCR writes: %llu vs total-fixup FCR NULLs: %llu)",
                    cl.fixed(), cl.skipped(), cl.fcr_nulls(),
                    (unsigned long long)fcr_evac_writes(),
                    (unsigned long long)fcr_fixup_nulls());
  }
  return cl.fixed() + cl.skipped();
}

HeapWord* G1RemoteMemoryManager::allocate_in_fcr(size_t word_size) {
  // Fast path: try CAS bump pointer on existing FCR region (lock-free).
  HeapRegion* fcr = _current_fcr;
  if (fcr != nullptr && !fcr->is_free()) {
    size_t actual = 0;
    HeapWord* result = fcr->par_allocate(word_size, word_size, &actual);
    if (result != nullptr) {
      fcr->update_bot_for_obj(result, word_size);
      return result;
    }
  }

  // Current FCR full or doesn't exist. Allocate a new FCR region.
  // Requires Heap_lock. The caller MUST be in _thread_in_vm state
  // (JRT_ENTRY context from resolve_tagged_oop_slow, or GC STW).
  // No os::malloc fallback — all fetched objects go into proper G1 regions.
  fcr_lock();
  if (_current_fcr != fcr) {
    fcr = _current_fcr;
    fcr_unlock();
    if (fcr != nullptr) {
      size_t actual = 0;
      HeapWord* result = fcr->par_allocate(word_size, word_size, &actual);
      if (result != nullptr) {
        fcr->update_bot_for_obj(result, word_size);
        return result;
      }
    }
    return nullptr;
  }

  HeapRegion* new_fcr = nullptr;
  {
    MutexLocker ml(Heap_lock);
    new_fcr = allocate_new_fcr_region();
  }
  if (new_fcr != nullptr) {
    _current_fcr = new_fcr;
    fcr_unlock();
    size_t actual = 0;
    HeapWord* result = new_fcr->par_allocate(word_size, word_size, &actual);
    if (result != nullptr) {
      new_fcr->update_bot_for_obj(result, word_size);
    }
    return result;
  }

  fcr_unlock();
  return nullptr;
}

// ============================================================
// Remote Executor Client — TCP communication
// NOTE: Executor client code is in g1RemoteBackendTcp.cpp.
// G1RemoteMemoryManager dispatches to _backend (SimLocal, TCP, or RDMA).

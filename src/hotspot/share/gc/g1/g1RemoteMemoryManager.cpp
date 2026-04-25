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
    _table_lock(0),
    _sim_remote_next_slot(0), _sim_remote_evicted_count(0),
    _sim_remote_fetched_count(0), _gc_epoch(0),
    _remote_roots_count(0), _deferred_decrement_count(0),
    _tagged_fields(nullptr), _tagged_field_count(0), _tagged_field_capacity(0),
    _current_fcr(nullptr), _fcr_lock(0),
    _executor_fd(-1), _executor_connected(false), _executor_seq_id(0) {
  memset(_table, 0, sizeof(_table));
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
  memset(_table, 0, sizeof(_table));

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

  // Temporary edge buffer (stack-allocated, fixed capacity)
  static const int MAX_EDGES = 256;
  G1RemoteMemoryManager::EdgeEntry _edges[MAX_EDGES];
  int _count;

public:
  EdgeTableBuildClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                        RemoteHandleAllocBuffer* hab, oop base)
    : _rmm(rmm), _g1h(g1h), _hab(hab), _base_obj(base), _count(0) {}

  virtual void do_oop(oop* p) {
    if (_count >= MAX_EDGES) return;  // safety cap

    // Read field as raw uintptr_t to avoid debug oop constructor checks on tagged values
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;  // null

    // If already tagged (bit 63 set), the field already has a Handle reference.
    // Extract the Handle directly.
    if ((raw >> 63) != 0) {
      if (raw & G1_OOP_INDIRECT_BIT) {
        // Shared OOP → already points to a Handle
        RemoteHandle* h = (RemoteHandle*)(raw & G1_OOP_ADDR_MASK);
        uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
        _edges[_count]._field_offset = offset;
        _edges[_count]._target_handle = h;
        _count++;
        h->increment_remote_refcount();
      }
      // Unique OOP → strip tags, get target, create dormant anchor
      else {
        oop target = (oop)(raw & G1_OOP_ADDR_MASK);
        if (_g1h->is_in(target)) {
          RemoteHandle* h = _rmm->ensure_dormant_anchor_for(target, _hab);
          uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
          _edges[_count]._field_offset = offset;
          _edges[_count]._target_handle = h;
          _count++;
          h->increment_remote_refcount();
        }
      }
      return;
    }

    // Clean oop — create dormant anchor for the target
    oop target = cast_to_oop(raw);
    if (_g1h->is_in(target)) {
      RemoteHandle* h = _rmm->ensure_dormant_anchor_for(target, _hab);
      uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
      _edges[_count]._field_offset = offset;
      _edges[_count]._target_handle = h;
      _count++;
      h->increment_remote_refcount();
    }
  }

  virtual void do_oop(narrowOop* p) {
    // Narrow oops: not used (UseCompressedOops=false in our config)
  }

  int count() const { return _count; }
  bool overflowed() const { return _count >= MAX_EDGES; }
  const G1RemoteMemoryManager::EdgeEntry* edges() const { return _edges; }
};

G1RemoteMemoryManager::ObjectEdgeTable*
G1RemoteMemoryManager::build_edge_table(oop obj, RemoteHandle* obj_handle,
                                        RemoteHandleAllocBuffer* hab) {
  EdgeTableBuildClosure cl(this, _g1h, hab, obj);
  obj->oop_iterate(&cl);

  if (cl.overflowed()) {
    log_info(gc)("Edge table overflow: obj=" PTR_FORMAT " klass=%s has >256 oop fields — skipping eviction",
                 p2i((void*)obj), obj->klass()->external_name());
    return nullptr;
  }

  // Allocate and populate the edge table
  ObjectEdgeTable* et = ObjectEdgeTable::allocate(cl.count());
  et->_source_handle = obj_handle;
  et->_eviction_word_size = obj->size();
  for (int i = 0; i < cl.count(); i++) {
    et->add(cl.edges()[i]._field_offset, cl.edges()[i]._target_handle);
  }

  log_debug(gc)("Edge table built: obj=" PTR_FORMAT " edges=%d",
                p2i((void*)obj), cl.count());
  return et;
}

bool G1RemoteMemoryManager::evict_object(oop obj, RemoteHandleAllocBuffer* hab) {
  // Safety checks
  if (obj == nullptr) return false;

  markWord mw = obj->mark();
  // Don't evict locked/inflated objects
  if (!mw.is_unlocked()) {
    log_debug(gc, remset)("evict_object: skipping locked object " PTR_FORMAT, p2i((void*)obj));
    return false;
  }

  Klass* klass = obj->klass();
  size_t word_size = obj->size_given_klass(klass);

  // 1. Create Handle if not already managed
  RemoteHandle* h = handle_for(obj);
  if (h == nullptr) {
    h = create_handle_for(obj, hab);
  }

  // 2. Build sidecar edge table BEFORE eviction (object bytes still readable).
  //    Scans oop fields, creates dormant anchors for targets, records edges.
  ObjectEdgeTable* et = build_edge_table(obj, h, hab);
  if (et == nullptr) {
    return false;  // too many oop fields — cannot safely evict
  }
  store_edge_table(et);

  // 3. Evict object bytes via backend (V2: with edges, V1: fallback)
  size_t slot_id;
  if (et->_entry_count > 0) {
    // V2: send edge table alongside object bytes
    G1RemoteBackend::EdgeInfo* edges = nullptr;
    if (et->_entry_count > 0) {
      edges = (G1RemoteBackend::EdgeInfo*)os::malloc(et->_entry_count * sizeof(G1RemoteBackend::EdgeInfo), mtGC);
      for (uint32_t i = 0; i < et->_entry_count; i++) {
        edges[i].field_offset = et->_entries[i]._field_offset;
        edges[i].target_handle_id = (uintptr_t)et->_entries[i]._target_handle;
      }
    }
    slot_id = _backend->evict_with_edges(cast_from_oop<void*>(obj), word_size, klass,
                                          (uintptr_t)h, edges, et->_entry_count, (size_t)-1);
    if (edges != nullptr) os::free(edges);
  } else {
    slot_id = _backend->evict(cast_from_oop<void*>(obj), word_size, klass, (size_t)-1);
  }
  if (slot_id == (size_t)-1) {
    log_warning(gc)("Remote evict failed for obj=" PTR_FORMAT, p2i((void*)obj));
    return false;
  }

  // 4. Store word_size FIRST, then publish REMOTE state (release store).
  //    Readers (resolve_tagged_oop_slow) see REMOTE via acquire load, then
  //    read eviction_word_size(). Size must be visible before REMOTE.
  h->set_eviction_word_size(word_size);
  h->set_remote(slot_id);

  // 5. Set classification in mark word + per-region bitmap.
  //    Mark word: fast per-object check for mutators (same cache line as header)
  //    Bitmap: region-level iteration for GC
  //    Re-read mark word (it may have changed since our earlier is_unlocked check,
  //    though during STW eviction trigger this is unlikely).
  mw = obj->mark();
  if (mw.is_unlocked()) {
    obj->set_mark(mw.set_remote_class(markWord::remote_class_shared));
  }
  HeapRegion* hr = _g1h->heap_region_containing(obj);
  if (hr != nullptr) {
    hr->set_has_classified_objects();
  }

  // 6. Overwrite local bytes with filler to poison stale clean oops.
  //    After this, any reference that bypassed Handle-based access will see
  //    a filler object, causing a visible crash instead of silent corruption.
  CollectedHeap::fill_with_object(cast_from_oop<HeapWord*>(obj), word_size, false);

  log_info(gc)("Remote evict: obj=" PTR_FORMAT " klass=%s size=" SIZE_FORMAT "w slot=" SIZE_FORMAT " edges=%u (filled)",
               p2i((void*)obj), klass->external_name(), word_size, slot_id, et->_entry_count);

  return true;
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
// O(live_heap) but runs during STW and catches refs that remset misses
// (dirty cards not yet refined, post-evacuation card dirtying, etc.).
int G1RemoteMemoryManager::tag_all_heap_refs_to_eviction_set(
    const bool* eviction_set, uint num_regions) {

  class EvictionSetTagClosure : public BasicOopIterateClosure {
    G1RemoteMemoryManager* _rmm;
    G1CollectedHeap*       _g1h;
    const bool*            _eviction_set;
    uint                   _num_regions;
    int                    _tagged;
    int                    _no_handle;
  public:
    EvictionSetTagClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                          const bool* eset, uint nregions)
      : _rmm(rmm), _g1h(g1h), _eviction_set(eset),
        _num_regions(nregions), _tagged(0), _no_handle(0) {}

    virtual void do_oop(oop* p) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) return;
      // Shared oops (bits 63+62) already have Handle indirection — skip.
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

      // Resolve: strip Unique tag bits if present.
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

      RemoteHandle* h = _rmm->handle_for(target);
      if (h != nullptr) {
        *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
        _rmm->add_tagged_field(p, h);
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
  };

  EvictionSetTagClosure cl(this, _g1h, eviction_set, num_regions);

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
      if (sz == 0) break;
      if (sz > (size_t)(region_end - p)) {
        // Humongous object spanning multiple regions — still must iterate
        // its oop fields, since they may reference eviction candidates.
        obj->oop_iterate(&cl);
        break;
      }
      obj->oop_iterate(&cl);
      p += sz;
    }
  }

  if (cl.tagged() > 0 || cl.no_handle() > 0) {
    log_info(gc)("Full heap scan: tagged %d refs, %d refs had no handle",
                 cl.tagged(), cl.no_handle());
  }
  return cl.tagged();
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

  // 1. Verify heap: same walk as tagging scan (including candidate regions)
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
      if (sz == 0) break;
      cl.set_cur_obj(obj);
      if (sz > (size_t)(region_end - p)) {
        obj->oop_iterate(&cl);
        break;
      }
      obj->oop_iterate(&cl);
      p += sz;
    }
  }

  // 2. Verify roots: check thread stacks, JNI, OopStorages
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
  {
    CodeBlobToOopClosure code_cl(&cl, false);
    CodeCache::blobs_do(&code_cl);
  }
  _g1h->ref_processor_cm()->weak_oops_do(&cl);

  if (cl.missed() > 0) {
    log_warning(gc)("VERIFY: %d untagged refs to eviction candidates AFTER tagging!",
                    cl.missed());
  }
  return cl.missed();
}

int G1RemoteMemoryManager::evict_region(HeapRegion* hr, RemoteHandleAllocBuffer* hab) {
  assert(hr->is_old(), "can only evict Old regions");
  assert(!hr->is_humongous(), "cannot evict humongous regions");

  // All-or-nothing: validate ALL objects before evicting any.
  // If any object is unevictable, abort the entire region.
  HeapWord* p = hr->bottom();
  HeapWord* region_end = hr->end();
  int total_objects = 0;
  while (p < hr->top()) {
    if (p < hr->bottom() || p >= region_end) return 0;
    oop obj = cast_to_oop(p);
    if (obj->klass_or_null() == nullptr) {
      log_debug(gc)("evict_region: null klass at " PTR_FORMAT " in region %u — aborting",
                     p2i(p), hr->hrm_index());
      return 0;
    }
    size_t sz = obj->size();
    if (sz == 0) {
      log_debug(gc)("evict_region: unparseable object at " PTR_FORMAT " in region %u — aborting",
                     p2i(p), hr->hrm_index());
      return 0;
    }
    markWord mw = obj->mark();
    // Locked/inflated objects can't be evicted (mark word holds pointer)
    if (!mw.is_unlocked()) {
      log_debug(gc)("evict_region: locked object at " PTR_FORMAT " in region %u — aborting",
                     p2i(p), hr->hrm_index());
      return 0;
    }
    total_objects++;
    p += sz;
  }

  if (total_objects == 0) return 0;

  // Phase 1: Create Handles + build edge tables for all objects
  p = hr->bottom();
  while (p < hr->top()) {
    oop obj = cast_to_oop(p);
    ensure_handle_for(obj, hab);
    p += obj->size();
  }

  // Phase 2: Tag incoming refs from other regions BEFORE eviction
  // (objects still readable, Handles created, so tag_incoming_refs can find them)
  tag_incoming_refs_to_region(hr);

  // If tagging found untaggable refs, the region was pinned — abort
  if (hr->is_root_pinned()) {
    log_debug(gc)("evict_region: region %u pinned by untaggable refs — aborting",
                   hr->hrm_index());
    return 0;
  }

  // Phase 3: Evict all objects (send to backend + fill with filler)
  int evicted = 0;
  p = hr->bottom();
  while (p < hr->top()) {
    oop obj = cast_to_oop(p);
    size_t sz = obj->size();
    if (evict_object(obj, hab)) {
      evicted++;
    } else {
      // Backend failure — abort. Objects already evicted in this pass
      // are lost (handles point to remote, local is filler). This is a
      // hard failure; log and stop.
      log_warning(gc)("evict_region: backend evict failed at object " PTR_FORMAT
                      " in region %u after %d objects — partial eviction!",
                      p2i((void*)obj), hr->hrm_index(), evicted);
      break;
    }
    p += sz;
  }

  if (evicted > 0) {
    log_info(gc)("Region eviction: region %u [" PTR_FORMAT "] — %d/%d objects evicted",
                 hr->hrm_index(), p2i(hr->bottom()), evicted, total_objects);
  }

  return evicted;
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
    log_info(gc)("Remote fetch: slot=" SIZE_FORMAT " -> dest=" PTR_FORMAT " klass=%s size=" SIZE_FORMAT "w",
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

    if (target_state == REMOTE_HANDLE_LOCAL) {
      // Target is local — patch to clean oop (current address)
      uintptr_t target_addr = sa & REMOTE_HANDLE_ADDR_MASK;
      if (target_addr != 0 && _g1h->is_in((void*)target_addr)) {
        oop target_oop = cast_to_oop(target_addr);
        Klass* target_klass = target_oop->klass_or_null();
        if (target_klass == nullptr) {
          log_warning(gc)("Fetch patch CORRUPT: edge %u offset=%u handle=" PTR_FORMAT
                          " target=" PTR_FORMAT " has null klass — nulling field",
                          i, edge._field_offset, p2i(target), p2i((void*)target_addr));
          *field_addr = 0;
          patched++;
          if (cm_active) { defer_refcount_decrement(target); } else { target->decrement_remote_refcount(); }
          continue;
        }
      } else if (target_addr != 0) {
        log_warning(gc)("Fetch patch CORRUPT: edge %u offset=%u handle=" PTR_FORMAT
                        " target=" PTR_FORMAT " not in heap — nulling field",
                        i, edge._field_offset, p2i(target), p2i((void*)target_addr));
        *field_addr = 0;
        patched++;
        if (cm_active) { defer_refcount_decrement(target); } else { target->decrement_remote_refcount(); }
        continue;
      }
      *field_addr = target_addr;
      patched++;
    } else if (target_state == REMOTE_HANDLE_REMOTE ||
               target_state == REMOTE_HANDLE_FETCHING) {
      // Target is still remote — patch to shared_oop(target_handle)
      // so the load barrier will trigger fetch when this field is read
      *field_addr = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)target;
      patched++;
    } else if (target_state == REMOTE_HANDLE_DEAD) {
      // Target was collected — null the field
      *field_addr = 0;
      patched++;
    }

    // P13: SATB-safe refcount management.
    // During concurrent marking, defer decrements to avoid removing dormant
    // anchors while marking threads may still encounter refs to their targets.
    // Deferred decrements are applied after remark (STW).
    if (cm_active) {
      defer_refcount_decrement(target);
    } else {
      target->decrement_remote_refcount();
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
  // Handle-id-based liveness: a remote object is alive if its handle_id
  // appears in _remote_roots (logged during P12 concurrent marking) OR
  // if it has a dormant anchor with remote_refcount > 0 (referenced by
  // another remote object's edge table that was itself rooted).
  //
  // For prototype: use _remote_roots as the live set. Objects not in this
  // set are considered dead. This is correct after a completed concurrent
  // marking cycle (remark collected the roots).

  // Build live handle set from _remote_roots
  size_t total_remote = 0;

  // Step 1: Build root slot_ids from live handle_ids for backend
  size_t root_capacity = 256;
  size_t* root_ids = (size_t*)os::malloc(root_capacity * sizeof(size_t), mtGC);
  size_t num_roots = 0;

  // Include handles from _remote_roots (P12 concurrent marking log)
  for (int i = 0; i < _remote_roots_count; i++) {
    RemoteHandle* h = (RemoteHandle*)_remote_roots[i];
    if (h != nullptr && h->is_remote()) {
      if (num_roots >= root_capacity) {
        root_capacity *= 2;
        root_ids = (size_t*)os::realloc(root_ids, root_capacity * sizeof(size_t), mtGC);
      }
      root_ids[num_roots++] = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
    }
  }

  // Report ALL remote handles as roots. Without a completed concurrent
  // marking cycle, _remote_roots is empty/stale and we cannot determine
  // which handles are truly dead. Reporting all as alive is conservative
  // but correct — the executor will only free objects not in the root set.
  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      if (e->_handle != nullptr && e->_handle->is_remote()) {
        total_remote++;
        if (num_roots >= root_capacity) {
          root_capacity *= 2;
          root_ids = (size_t*)os::realloc(root_ids, root_capacity * sizeof(size_t), mtGC);
        }
        root_ids[num_roots++] = e->_handle->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
      }
    }
  }
  table_unlock();

  // Step 2: Report roots to backend and request collection.
  // V1: slot-id based roots
  _backend->report_roots(root_ids, num_roots);
  os::free(root_ids);

  // V2: also report handle-id based roots from P12 concurrent marking
  if (_remote_roots_count > 0) {
    _backend->report_remote_roots_v2(_remote_roots, _remote_roots_count);
  }

  size_t* dead_ids = nullptr;
  size_t num_dead = 0;
  size_t bytes_freed = 0;
  _backend->collect_dead(&dead_ids, &num_dead, &bytes_freed);

  // Step 3: Clean up Handle entries for dead objects.
  size_t collected = 0;
  if (num_dead > 0) {
    table_lock();
    for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
      HandleEntry** pp = &_table[idx];
      while (*pp != nullptr) {
        HandleEntry* entry = *pp;
        if (entry->_handle != nullptr && entry->_handle->is_remote()) {
          uintptr_t sa = entry->_handle->load_state_and_addr_acquire();
          size_t slot_id = sa & REMOTE_HANDLE_ADDR_MASK;

          bool is_dead = false;
          for (size_t d = 0; d < num_dead; d++) {
            if (dead_ids[d] == slot_id) { is_dead = true; break; }
          }

          if (is_dead) {
            // Set Handle to DEAD state
            entry->_handle->set_dead();

            // Clean up edge table for this handle
            remove_edge_table(entry->_handle);

            // Decrement remote_refcount on target handles in edge table
            // (already removed, but targets may still have inflated refcounts)

            // Remove Handle entry from table
            *pp = entry->_next;
            free_entry(entry);
            collected++;
            continue;
          }
        }
        pp = &(*pp)->_next;
      }
    }
    table_unlock();
  }

  if (dead_ids) os::free(dead_ids);

  size_t retained = total_remote - collected;
  if (collected > 0 || retained > 0) {
    log_info(gc)("Remote collection: " SIZE_FORMAT " dead objects freed (" SIZE_FORMAT " bytes reclaimed remotely), "
                 SIZE_FORMAT " live objects retained",
                 collected, bytes_freed, retained);
  }
  return collected;
}

int G1RemoteMemoryManager::fixup_tagged_field_handles() {
  int updated = 0;
  int removed = 0;
  int write_idx = 0;

  for (int i = 0; i < _tagged_field_count; i++) {
    oop* field_addr = _tagged_fields[i]._field_addr;
    RemoteHandle* h = _tagged_fields[i]._handle;

    uintptr_t raw = *(uintptr_t*)field_addr;

    // Stale entry: field no longer tagged or points to a different Handle
    if ((raw & G1_OOP_INDIRECT_BIT) == 0 ||
        (RemoteHandle*)(raw & G1_OOP_ADDR_MASK) != h) {
      removed++;
      continue;
    }

    // Handle must be LOCAL for fixup (REMOTE/DEAD handles don't need it)
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    if (state != REMOTE_HANDLE_LOCAL) {
      _tagged_fields[write_idx++] = _tagged_fields[i];
      continue;
    }

    HeapWord* target = (HeapWord*)(sa & REMOTE_HANDLE_ADDR_MASK);
    oop target_oop = cast_to_oop(target);

    // Check if target has been forwarded (mark word contains forwarding ptr)
    if (_g1h->is_in(target_oop)) {
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

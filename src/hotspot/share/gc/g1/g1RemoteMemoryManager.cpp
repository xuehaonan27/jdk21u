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
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1NUMA.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/copy.hpp"

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
    _sim_remote_fetched_count(0), _gc_epoch(0), _remote_roots_count(0),
    _current_fcr(nullptr), _fcr_lock(0),
    _executor_fd(-1), _executor_connected(false), _executor_seq_id(0) {
  memset(_table, 0, sizeof(_table));
  memset(_edge_tables, 0, sizeof(_edge_tables));
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

void G1RemoteMemoryManager::initialize_backend() {
  if (!_backend->initialize()) {
    log_warning(gc)("Remote backend (%s) initialization failed, falling back to sim-local",
                    _backend->name());
    delete _backend;
    _backend = new SimLocalBackend();
    _backend->initialize();
  }
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

  // Free edge tables
  for (size_t i = 0; i < EDGE_TABLE_SIZE; i++) {
    ObjectEdgeTable::free(_edge_tables[i]);
    _edge_tables[i] = nullptr;
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
  const G1RemoteMemoryManager::EdgeEntry* edges() const { return _edges; }
};

G1RemoteMemoryManager::ObjectEdgeTable*
G1RemoteMemoryManager::build_edge_table(oop obj, RemoteHandle* obj_handle,
                                        RemoteHandleAllocBuffer* hab) {
  EdgeTableBuildClosure cl(this, _g1h, hab, obj);
  obj->oop_iterate(&cl);

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
  store_edge_table(et);

  // 3. Evict object bytes via backend (SIM/TCP/RDMA)
  size_t slot_id = _backend->evict(cast_from_oop<void*>(obj), word_size, klass, (size_t)-1);
  if (slot_id == (size_t)-1) {
    log_warning(gc)("Remote evict failed for obj=" PTR_FORMAT, p2i((void*)obj));
    return false;
  }

  // 4. Set Handle to REMOTE with slot_id + store word_size for fetch-time allocation
  h->set_remote(slot_id);
  h->set_eviction_word_size(word_size);

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

  for (uint32_t i = 0; i < et->_entry_count; i++) {
    EdgeEntry& edge = et->_entries[i];
    uintptr_t* field_addr = (uintptr_t*)(base + edge._field_offset);
    RemoteHandle* target = edge._target_handle;

    uintptr_t sa = target->load_state_and_addr_acquire();
    uintptr_t target_state = sa & REMOTE_HANDLE_STATE_MASK;

    if (target_state == REMOTE_HANDLE_LOCAL) {
      // Target is local — patch to clean oop (current address)
      *field_addr = sa & REMOTE_HANDLE_ADDR_MASK;
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

    // Decrement remote refcount — this object is now local, its fields
    // no longer represent remote-stored references
    target->decrement_remote_refcount();
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
  G1ConcurrentMark* cm = _g1h->concurrent_mark();

  // Step 1: Gather live root slot_ids from Handle table.
  // A remote object is "alive" if its local reference is marked in the bitmap.
  size_t root_capacity = 256;
  size_t* root_ids = (size_t*)os::malloc(root_capacity * sizeof(size_t), mtGC);
  size_t num_roots = 0;
  size_t total_remote = 0;

  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      if (e->_handle != nullptr && e->_handle->is_remote()) {
        total_remote++;
        oop obj = cast_to_oop(e->_obj_addr);
        bool is_alive = false;
        if (_g1h->is_in(obj)) {
          HeapRegion* hr = _g1h->heap_region_containing(obj);
          if (hr != nullptr) {
            is_alive = cm->is_marked_in_bitmap(obj);
          }
        }
        if (is_alive) {
          if (num_roots >= root_capacity) {
            root_capacity *= 2;
            root_ids = (size_t*)os::realloc(root_ids, root_capacity * sizeof(size_t), mtGC);
          }
          uintptr_t sa = e->_handle->load_state_and_addr_acquire();
          root_ids[num_roots++] = sa & REMOTE_HANDLE_ADDR_MASK;
        }
      }
    }
  }
  table_unlock();

  // Step 2: Report roots to backend and request collection.
  _backend->report_roots(root_ids, num_roots);
  os::free(root_ids);

  size_t* dead_ids = nullptr;
  size_t num_dead = 0;
  size_t bytes_freed = 0;
  _backend->collect_dead(&dead_ids, &num_dead, &bytes_freed);

  // Step 3: Clean up Handle entries + bitmaps for dead objects.
  // Build a set of dead slot_ids for fast lookup.
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

          // Check if this slot_id is in the dead list
          bool is_dead = false;
          for (size_t d = 0; d < num_dead; d++) {
            if (dead_ids[d] == slot_id) { is_dead = true; break; }
          }

          if (is_dead) {
            // Clear mark word classification bits for dead object
            oop obj = cast_to_oop(entry->_obj_addr);
            if (_g1h->is_in(obj)) {
              markWord mw = obj->mark();
              if (mw.is_unlocked() && mw.has_remote_metadata()) {
                obj->set_mark(mw.set_remote_class(markWord::remote_class_untracked));
              }
            }

            // Remove Handle entry from table, return to pool
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

HeapWord* G1RemoteMemoryManager::allocate_in_fcr(size_t word_size) {
  // Fast path: try CAS bump pointer on existing FCR region (lock-free).
  HeapRegion* fcr = _current_fcr;
  if (fcr != nullptr) {
    size_t actual = 0;
    HeapWord* result = fcr->par_allocate(word_size, word_size, &actual);
    if (result != nullptr) {
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
      if (result != nullptr) return result;
    }
    return nullptr;
  }

  HeapRegion* new_fcr = allocate_new_fcr_region();
  if (new_fcr != nullptr) {
    _current_fcr = new_fcr;
    fcr_unlock();
    size_t actual = 0;
    return new_fcr->par_allocate(word_size, word_size, &actual);
  }

  fcr_unlock();
  return nullptr;
}

// ============================================================
// Remote Executor Client — TCP communication
// NOTE: Executor client code is in g1RemoteBackendTcp.cpp.
// G1RemoteMemoryManager dispatches to _backend (SimLocal, TCP, or RDMA).

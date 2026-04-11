/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 *
 * G1RemoteMemoryManager: central coordinator for disaggregated memory.
 *
 * Manages:
 * - Handle table (allocation, lookup by object address)
 * - Remote object metadata (size, klass for remote objects)
 * - Object-to-Handle mapping (for write barrier and GC)
 * - Per-thread Handle allocation buffers (HABs)
 *
 * Phase 1: all remote-participating objects are Shared (always Handle).
 * Phase 2 adds Unique/Shared classification.
 */

#ifndef SHARE_GC_G1_G1REMOTEMEMORYMANAGER_HPP
#define SHARE_GC_G1_G1REMOTEMEMORYMANAGER_HPP

#include "gc/g1/g1RemoteHandle.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/concurrentHashTable.hpp"
#include "utilities/globalDefinitions.hpp"

class G1CollectedHeap;
class HeapRegion;

// ============================================================
// G1RemoteMemoryManager
// ============================================================

class G1RemoteBackend;

class G1RemoteMemoryManager : public CHeapObj<mtGC> {
  G1CollectedHeap* _g1h;

  // Remote storage backend (sim-local, TCP executor, or RDMA executor).
  // Selected by UseRemoteExecutor flag. Created in constructor.
  G1RemoteBackend* _backend;

  // Handle allocator (global chunk pool + per-thread HABs)
  RemoteHandleAllocator _handle_allocator;

  // ============================================================
  // Handle table: secondary index (current_local_addr → Handle)
  // ============================================================
  // The primary index is the Handle pointer itself (handle_id = RemoteHandle*).
  // This secondary index maps current local object addresses to Handles.
  // Rekeyed on every evacuation move and every fetch localization.

  struct HandleEntry {
    uintptr_t     _obj_addr;   // key: current local object address
    RemoteHandle* _handle;     // value: Handle pointer
    HandleEntry*  _next;       // chaining

    void init(uintptr_t addr, RemoteHandle* h, HandleEntry* next) {
      _obj_addr = addr;
      _handle = h;
      _next = next;
    }
  };

  // Chunked pool allocator for HandleEntry (no os::malloc during STW).
  static const size_t ENTRY_CHUNK_CAPACITY = 256; // 256 * 24B ≈ 6KB per chunk
  struct HandleEntryChunk : public CHeapObj<mtGC> {
    HandleEntry      _entries[ENTRY_CHUNK_CAPACITY];
    HandleEntryChunk* _next;
    HandleEntryChunk() : _next(nullptr) {}
  };

  HandleEntryChunk* _entry_chunks;     // all allocated entry chunks
  HandleEntry*      _entry_free_list;  // free entry list for reuse
  size_t            _entry_chunk_top;  // next free slot in current chunk

  HandleEntry* alloc_entry() {
    // Reuse from free list first
    if (_entry_free_list != nullptr) {
      HandleEntry* e = _entry_free_list;
      _entry_free_list = e->_next;
      return e;
    }
    // Allocate from current chunk
    if (_entry_chunks == nullptr || _entry_chunk_top >= ENTRY_CHUNK_CAPACITY) {
      HandleEntryChunk* chunk = new HandleEntryChunk();
      chunk->_next = _entry_chunks;
      _entry_chunks = chunk;
      _entry_chunk_top = 0;
    }
    return &_entry_chunks->_entries[_entry_chunk_top++];
  }

  void free_entry(HandleEntry* e) {
    e->_next = _entry_free_list;
    _entry_free_list = e;
  }

  // Hash table for secondary index
  static const size_t TABLE_SIZE = 1024;
  HandleEntry* _table[TABLE_SIZE];
  volatile int _table_lock;

  void table_lock()   { while (Atomic::cmpxchg(&_table_lock, 0, 1) != 0) { /* spin */ } }
  void table_unlock() { Atomic::release_store(&_table_lock, 0); }

  static size_t hash_obj(uintptr_t addr) {
    return (addr >> 3) % TABLE_SIZE;  // Objects are 8-byte aligned
  }

public:
  G1RemoteMemoryManager(G1CollectedHeap* g1h);
  ~G1RemoteMemoryManager();

  // Must be called AFTER G1CollectedHeap::initialize() has set up the heap
  // regions (_hrm.initialize, initialize_reserved_region). At that point
  // G1CollectedHeap::heap(), reserved(), and max_capacity() are valid.
  // Connects the backend to the remote executor and sends the hello handshake.
  void initialize_backend();

  // ============================================================
  // Handle management
  // ============================================================

  // ============================================================
  // Handle management — ensure_handle_for() is the primary API
  // ============================================================

  // Ensure an object has a Handle. Returns existing Handle if already present
  // (dedup), or creates a new one. This is the only way to create Handles.
  // STW-safe: uses chunked allocators, no os::malloc.
  RemoteHandle* ensure_handle_for(oop obj, RemoteHandleAllocBuffer* hab) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    table_lock();
    // Check for existing entry (dedup)
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr) {
        RemoteHandle* existing = e->_handle;
        table_unlock();
        return existing;
      }
      e = e->_next;
    }
    // Not found — create new Handle and entry
    RemoteHandle* h = _handle_allocator.allocate_handle(hab);
    h->initialize(cast_from_oop<void*>(obj));

    HandleEntry* entry = alloc_entry();
    entry->init(addr, h, _table[idx]);
    _table[idx] = entry;
    table_unlock();
    return h;
  }

  // Ensure a dormant anchor Handle for a local object referenced by remote.
  // Like ensure_handle_for() but sets the DORMANT flag on new Handles.
  RemoteHandle* ensure_dormant_anchor_for(oop obj, RemoteHandleAllocBuffer* hab) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    table_lock();
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr) {
        RemoteHandle* existing = e->_handle;
        // Existing handle might not be dormant yet; mark it
        existing->set_dormant();
        table_unlock();
        return existing;
      }
      e = e->_next;
    }
    RemoteHandle* h = _handle_allocator.allocate_handle(hab);
    h->initialize_dormant(cast_from_oop<void*>(obj));

    HandleEntry* entry = alloc_entry();
    entry->init(addr, h, _table[idx]);
    _table[idx] = entry;
    table_unlock();
    return h;
  }

  // Legacy API: create_handle_for (delegates to ensure_handle_for).
  // Kept for backward compatibility with existing eviction/classification code.
  RemoteHandle* create_handle_for(oop obj, RemoteHandleAllocBuffer* hab) {
    return ensure_handle_for(obj, hab);
  }

  // Look up Handle for an object by current local address.
  // Returns nullptr if object has no Handle.
  RemoteHandle* handle_for(oop obj) const {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr) return e->_handle;
      e = e->_next;
    }
    return nullptr;
  }

  // Check if an object has a Handle (fast negative via table lookup).
  bool has_handle(oop obj) const {
    return handle_for(obj) != nullptr;
  }

  // Update the mapping when an object is evacuated to a new address.
  // Called from do_copy_to_survivor_space() during STW for ANY object
  // with a Handle (not just SHARED — also dormant anchors).
  void update_handle_for_evacuation(oop old_obj, oop new_obj) {
    uintptr_t old_addr = cast_from_oop<uintptr_t>(old_obj);
    uintptr_t new_addr = cast_from_oop<uintptr_t>(new_obj);
    size_t old_idx = hash_obj(old_addr);

    table_lock();
    HandleEntry** pp = &_table[old_idx];
    while (*pp != nullptr) {
      if ((*pp)->_obj_addr == old_addr) {
        HandleEntry* entry = *pp;
        *pp = entry->_next;  // unlink from old bucket

        // Update Handle to point to new object address
        entry->_handle->set_local(cast_from_oop<void*>(new_obj));

        // Re-insert in new bucket
        entry->_obj_addr = new_addr;
        size_t new_idx = hash_obj(new_addr);
        entry->_next = _table[new_idx];
        _table[new_idx] = entry;

        table_unlock();
        return;
      }
      pp = &((*pp)->_next);
    }
    table_unlock();
    // Not found — object has no Handle (OK, not all objects have Handles)
  }

  // Remove Handle mapping and return entry to free list.
  // Handle is NOT freed (chunks are pool-managed). Handle state should be set
  // to DEAD by caller before removing.
  void remove_handle_for(oop obj) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    table_lock();
    HandleEntry** pp = &_table[idx];
    while (*pp != nullptr) {
      if ((*pp)->_obj_addr == addr) {
        HandleEntry* entry = *pp;
        *pp = entry->_next;
        free_entry(entry);
        table_unlock();
        return;
      }
      pp = &((*pp)->_next);
    }
    table_unlock();
  }

  // Legacy: register_unique (no longer needed, but kept for compat)
  void register_unique(oop obj) {
    // In the new design, Unique objects don't need Handle table entries.
    // Classification is in the mark word only.
  }

  // ============================================================
  // Sidecar Edge Tables (P3)
  // ============================================================
  // Per-evicted-object edge tables recording outgoing oop field references.
  // Used by:
  //   - Executor: remote GC tracing (follows handle_ids to find reachable objects)
  //   - Compute node: fetch-time field patching (translates stale fields to current addresses)
  //
  // Built at eviction time by scanning the evicted object's oop fields.
  // Each entry maps a field offset to the target's Handle (stable identity).

  struct EdgeEntry {
    uint32_t  _field_offset;      // byte offset of oop field within object
    RemoteHandle* _target_handle; // stable Handle of the referenced object
  };

  // Variable-length edge table for one evicted object.
  // Allocated from a chunked pool (no os::malloc during STW).
  struct ObjectEdgeTable : public CHeapObj<mtGC> {
    RemoteHandle* _source_handle;  // Handle of the evicted object
    size_t        _eviction_word_size; // object size for FCR allocation at fetch time
    uint32_t      _entry_count;
    uint32_t      _capacity;
    EdgeEntry     _entries[1];     // flexible array (actual size = _capacity)

    static ObjectEdgeTable* allocate(uint32_t capacity) {
      size_t sz = sizeof(ObjectEdgeTable) + (capacity > 0 ? (capacity - 1) : 0) * sizeof(EdgeEntry);
      ObjectEdgeTable* t = (ObjectEdgeTable*)os::malloc(sz, mtGC);
      t->_source_handle = nullptr;
      t->_eviction_word_size = 0;
      t->_entry_count = 0;
      t->_capacity = capacity;
      return t;
    }

    void add(uint32_t field_offset, RemoteHandle* target) {
      assert(_entry_count < _capacity, "edge table full");
      _entries[_entry_count]._field_offset = field_offset;
      _entries[_entry_count]._target_handle = target;
      _entry_count++;
    }

    static void free(ObjectEdgeTable* t) {
      if (t != nullptr) os::free(t);
    }
  };

  // Edge table storage: maps evicted object Handle → edge table.
  // Simple hash table (same pattern as _table). Low contention: only
  // written during STW eviction, read during fetch.
  static const size_t EDGE_TABLE_SIZE = 256;
  ObjectEdgeTable* _edge_tables[EDGE_TABLE_SIZE];

  static size_t hash_handle(RemoteHandle* h) {
    return ((uintptr_t)h >> 4) % EDGE_TABLE_SIZE;
  }

public:
  // Store an edge table for an evicted object.
  void store_edge_table(ObjectEdgeTable* et) {
    size_t idx = hash_handle(et->_source_handle);
    // Simple linear chain: store as linked list would be better, but for
    // prototype with few evictions, just use first-empty-slot probing.
    // Actually, use the _source_handle as key with linear probing.
    for (size_t i = 0; i < EDGE_TABLE_SIZE; i++) {
      size_t slot = (idx + i) % EDGE_TABLE_SIZE;
      if (_edge_tables[slot] == nullptr) {
        _edge_tables[slot] = et;
        return;
      }
    }
    // Table full — should not happen with few evictions
    assert(false, "edge table storage full");
  }

  // Look up edge table for a Handle (used at fetch time).
  ObjectEdgeTable* edge_table_for(RemoteHandle* h) const {
    size_t idx = hash_handle(h);
    for (size_t i = 0; i < EDGE_TABLE_SIZE; i++) {
      size_t slot = (idx + i) % EDGE_TABLE_SIZE;
      ObjectEdgeTable* et = _edge_tables[slot];
      if (et == nullptr) return nullptr;  // not found (empty slot = end of probe)
      if (et->_source_handle == h) return et;
    }
    return nullptr;
  }

  // Remove edge table for a Handle (called on fetch or discard).
  void remove_edge_table(RemoteHandle* h) {
    size_t idx = hash_handle(h);
    for (size_t i = 0; i < EDGE_TABLE_SIZE; i++) {
      size_t slot = (idx + i) % EDGE_TABLE_SIZE;
      ObjectEdgeTable* et = _edge_tables[slot];
      if (et == nullptr) return;
      if (et->_source_handle == h) {
        ObjectEdgeTable::free(et);
        _edge_tables[slot] = nullptr;
        return;
      }
    }
  }

  // Build edge table for an object about to be evicted.
  // Scans all oop fields, creates dormant anchors for targets, records edges.
  // Must be called BEFORE eviction (object bytes still readable locally).
  ObjectEdgeTable* build_edge_table(oop obj, RemoteHandle* obj_handle,
                                    RemoteHandleAllocBuffer* hab);

private:
  // ============================================================
  // Simulated Remote Memory (Phase 1, no actual RDMA)
  // ============================================================
  // A simple local buffer that pretends to be remote storage.
  // Objects are copied here during "eviction" and copied back during "fetch".

  struct SimRemoteSlot {
    void*   _data;        // malloc'd buffer holding object bytes
    size_t  _word_size;   // object size in HeapWords
    Klass*  _klass;       // Klass pointer (always local)
    bool    _in_use;
  };

  static const size_t SIM_REMOTE_MAX_SLOTS = 1024;
  SimRemoteSlot _sim_remote_slots[SIM_REMOTE_MAX_SLOTS];
  size_t _sim_remote_next_slot;
  size_t _sim_remote_evicted_count;
  size_t _sim_remote_fetched_count;

  // Allocate a simulated remote slot and copy object bytes into it.
  // Returns the slot index (used as remote_id in Handle).
  size_t sim_remote_evict(oop obj, size_t word_size, Klass* klass);

  // Fetch object bytes from simulated remote slot into local address.
  // Returns the Klass pointer stored at eviction time.
  Klass* sim_remote_fetch(size_t slot_id, void* dest, size_t word_size);

public:
  // ============================================================
  // Hotness Tracking (P8)
  // ============================================================
  // GC epoch counter: incremented each GC cycle. Used for recency epoch
  // in mark word (4-bit field, wraps at 16). Objects accessed between GCs
  // get their epoch stamped; distance from current epoch = coldness.
  uint32_t _gc_epoch;

public:
  uint32_t gc_epoch() const { return _gc_epoch; }
  void increment_gc_epoch() { _gc_epoch++; }

  // ============================================================
  // Accessors
  // ============================================================

  RemoteHandleAllocator* handle_allocator() { return &_handle_allocator; }
  G1RemoteBackend* backend() { return _backend; }

  // ============================================================
  // Fetch Cache Region (FCR) Allocation
  // ============================================================
  // Fetched remote objects are allocated into FCR regions (G1 Old sub-type).
  // FCR regions participate in GC like Old regions (marking, evacuation).
  // Uses par_allocate() (CAS-based bump pointer) for thread-safe allocation.
private:
  HeapRegion* _current_fcr;       // Current FCR region for fetch allocation
  volatile int _fcr_lock;         // Spinlock for FCR region creation

  void fcr_lock()   { while (Atomic::cmpxchg(&_fcr_lock, 0, 1) != 0) { /* spin */ } }
  void fcr_unlock() { Atomic::release_store(&_fcr_lock, 0); }

  // Allocate a new FCR region from the free region pool.
  // Must NOT be called from JRT_LEAF (needs Heap_lock).
  HeapRegion* allocate_new_fcr_region();

public:
  // Allocate space for a fetched object in the current FCR region.
  // Thread-safe (CAS-based bump pointer). Returns nullptr if FCR is full
  // and we can't allocate a new one (e.g., in JRT_LEAF context).
  HeapWord* allocate_in_fcr(size_t word_size);

  // ============================================================
  // Remote Collection — "Garbage Never Crosses the Network"
  // ============================================================
  // After concurrent marking, identify dead remote objects and free their
  // sim-remote slots WITHOUT fetching them back. Only live remote objects
  // are kept (and fetched lazily on access via the load barrier).
  //
  // This is the core research contribution: object-granularity remote memory
  // management where garbage stays remote and is discarded in-place.
  //
  // Returns: number of dead remote objects collected.
  size_t collect_dead_remote_objects();

  bool is_managed(oop obj) const {
    return handle_for(obj) != nullptr;
  }

  // Legacy fields (kept for struct layout compat; unused when backend is active):
private:
  int      _executor_fd;
  bool     _executor_connected;
  uint64_t _executor_seq_id;

public:
  size_t sim_remote_evicted_count() const { return _sim_remote_evicted_count; }
  size_t sim_remote_fetched_count() const { return _sim_remote_fetched_count; }
  size_t sim_remote_word_size(size_t slot_id) const {
    assert(slot_id < SIM_REMOTE_MAX_SLOTS, "Invalid slot");
    return _sim_remote_slots[slot_id]._word_size;
  }

  // ============================================================
  // Manual Eviction API (for testing)
  // ============================================================

  // Evict an object to simulated remote memory:
  // 1. Create Handle (if not already managed)
  // 2. Copy object bytes to simulated remote slot
  // 3. Set Handle to REMOTE state with slot_id
  // 4. Set mark word oop_managed bit
  // Returns true on success.
  bool evict_object(oop obj, RemoteHandleAllocBuffer* hab);

  // Fetch a remote object back to a local destination address.
  // Called from the load barrier when a REMOTE Handle is encountered.
  // 1. Looks up slot from Handle's remote_id
  // 2. Copies bytes from simulated remote to dest
  // 3. Updates Handle to LOCAL
  // Returns the fetched object's Klass pointer.
  Klass* fetch_remote_object(RemoteHandle* h, void* dest);

  // Update all Handles after Full GC phase 3 (forwarding addresses computed).
  // Walks the Handle table. For each LOCAL Handle, checks if the object has
  // a forwarding address. If so, updates the Handle and rekeys the table entry.
  // Must be called BEFORE phase 4 (compaction moves objects).
  void update_handles_for_full_gc();

  // Iterate dormant anchor Handles (remote_refcount > 0) as strong GC roots.
  // For each active anchor, calls closure->do_oop on a synthetic oop* pointing
  // to the Handle's stored local address. This keeps referenced local objects
  // alive during GC even if they're only reachable through remote objects.
  //
  // Called from G1RootProcessor::evacuate_roots() during STW.
  template <typename OopClosureType>
  void oops_do_remote_anchors(OopClosureType* cl);

  // Patch fetched object's oop fields using sidecar edge table.
  // Called AFTER fetch_remote_object copies bytes, BEFORE set_local_release().
  // For each edge entry:
  //   - target LOCAL  → patch field to clean oop(current_addr)
  //   - target REMOTE → patch field to shared_oop(target_handle)
  //   - target DEAD   → patch field to null
  // Decrements remote_refcount on each target Handle.
  // Removes the edge table after patching.
  void patch_fetched_fields(RemoteHandle* source_handle, HeapWord* dest);
};

// Template implementation — must be in header for instantiation.
template <typename OopClosureType>
void G1RemoteMemoryManager::oops_do_remote_anchors(OopClosureType* cl) {
  // Walk the Handle table. For each entry whose Handle is LOCAL and has
  // remote_refcount > 0, it's a dormant anchor that must be rooted.
  // We call cl->do_oop on a pointer to a stack-local oop variable
  // holding the Handle's local address. If GC moves the object, the
  // closure updates the oop — we then update the Handle to match.
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      RemoteHandle* h = e->_handle;
      if (h != nullptr && h->is_local() && h->remote_refcount() > 0) {
        // This is a dormant anchor — its target must stay alive.
        oop obj = cast_to_oop(e->_obj_addr);
        cl->do_oop(&obj);
        // If GC moved the object, the closure updated obj.
        // We need to update the Handle and table entry to match.
        uintptr_t new_addr = cast_from_oop<uintptr_t>(obj);
        if (new_addr != e->_obj_addr) {
          h->set_local(cast_from_oop<void*>(obj));
          // Note: table rekey happens in update_handle_for_evacuation,
          // which is called separately from the evacuation path.
          // Here we just update the Handle's stored address.
          e->_obj_addr = new_addr;
        }
      }
      e = e->_next;
    }
  }
}

#endif // SHARE_GC_G1_G1REMOTEMEMORYMANAGER_HPP

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
    // Check for existing entry (dedup).
    // Skip stale entries: after eviction frees a region, addresses get reused.
    // A REMOTE/DEAD handle at this address belongs to a previously evicted object.
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr && e->_handle->is_local()) {
        RemoteHandle* existing = e->_handle;
        table_unlock();
        return existing;
      }
      e = e->_next;
    }
    // Not found (or only stale entries) — create new Handle and entry
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
      if (e->_obj_addr == addr && e->_handle->is_local()) {
        RemoteHandle* existing = e->_handle;
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
      if (e->_obj_addr == addr && e->_handle->is_local()) return e->_handle;
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
        // Only rekey LOCAL handles. REMOTE/DEAD entries are stale —
        // the address was reused after the original object's region was freed.
        if (!entry->_handle->is_local()) {
          pp = &((*pp)->_next);
          continue;
        }
        *pp = entry->_next;  // unlink from old bucket

        entry->_handle->set_local(cast_from_oop<void*>(new_obj));

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
  }

  // Rekey a Handle entry when an object is fetched to a new local address.
  // The table still has the old (evicted/filler) address; update to the new FCR address.
  void rekey_handle_on_fetch(RemoteHandle* h, void* new_addr) {
    uintptr_t new_uaddr = (uintptr_t)new_addr;
    table_lock();
    // Find the entry by Handle pointer (not by address, since old address is stale)
    for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
      HandleEntry* e = _table[idx];
      HandleEntry** pp = &_table[idx];
      while (e != nullptr) {
        if (e->_handle == h) {
          // Unlink from old bucket
          *pp = e->_next;
          // Rekey and insert into new bucket
          e->_obj_addr = new_uaddr;
          size_t new_idx = hash_obj(new_uaddr);
          e->_next = _table[new_idx];
          _table[new_idx] = e;
          table_unlock();
          return;
        }
        pp = &e->_next;
        e = e->_next;
      }
    }
    // Handle not found in table — it was a dormant anchor or already removed.
    // Insert a new entry for this address.
    HandleEntry* entry = alloc_entry();
    entry->init(new_uaddr, h, _table[hash_obj(new_uaddr)]);
    _table[hash_obj(new_uaddr)] = entry;
    table_unlock();
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

  // Edge table storage: chained hash table (Handle → edge table).
  // Written during STW eviction, read during fetch.
  static const size_t EDGE_TABLE_BUCKETS = 256;

  struct EdgeTableEntry {
    ObjectEdgeTable* _table;
    EdgeTableEntry*  _next;
  };
  EdgeTableEntry* _edge_buckets[EDGE_TABLE_BUCKETS];

  static size_t hash_handle(RemoteHandle* h) {
    return ((uintptr_t)h >> 4) % EDGE_TABLE_BUCKETS;
  }

public:
  void store_edge_table(ObjectEdgeTable* et) {
    size_t idx = hash_handle(et->_source_handle);
    EdgeTableEntry* entry = (EdgeTableEntry*)os::malloc(sizeof(EdgeTableEntry), mtGC);
    entry->_table = et;
    entry->_next = _edge_buckets[idx];
    _edge_buckets[idx] = entry;
  }

  ObjectEdgeTable* edge_table_for(RemoteHandle* h) const {
    size_t idx = hash_handle(h);
    EdgeTableEntry* e = _edge_buckets[idx];
    while (e != nullptr) {
      if (e->_table->_source_handle == h) return e->_table;
      e = e->_next;
    }
    return nullptr;
  }

  void remove_edge_table(RemoteHandle* h) {
    size_t idx = hash_handle(h);
    EdgeTableEntry** pp = &_edge_buckets[idx];
    while (*pp != nullptr) {
      if ((*pp)->_table->_source_handle == h) {
        EdgeTableEntry* entry = *pp;
        *pp = entry->_next;
        ObjectEdgeTable::free(entry->_table);
        os::free(entry);
        return;
      }
      pp = &(*pp)->_next;
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

  // Per-level hotness statistics: collected each GC, used by the NEXT GC
  // to determine which objects are cold enough to evict.
  static const int HOTNESS_LEVELS = 16;
  struct HotnessLevelStats {
    size_t object_count;
    size_t total_words;
  };
  HotnessLevelStats _hotness_stats[HOTNESS_LEVELS];     // current GC's data
  HotnessLevelStats _prev_hotness_stats[HOTNESS_LEVELS]; // previous GC's data (for eviction decisions)

public:
  // Remote root set: handle_ids logged during concurrent marking.
  // Collected after remark, used for CMD_REPORT_REMOTE_ROOTS_V2.
  static const int MAX_REMOTE_ROOTS = 8192;
  uintptr_t _remote_roots[MAX_REMOTE_ROOTS];
  int       _remote_roots_count;

public:
  // Deferred remote_refcount decrements (P13: SATB safety).
  // During concurrent marking, refcount decrements are buffered here
  // instead of applied immediately. Applied after remark (STW).
  static const int MAX_DEFERRED_DECREMENTS = 4096;
  RemoteHandle* _deferred_decrements[MAX_DEFERRED_DECREMENTS];
  int           _deferred_decrement_count;

public:
  void defer_refcount_decrement(RemoteHandle* h) {
    if (_deferred_decrement_count < MAX_DEFERRED_DECREMENTS) {
      _deferred_decrements[_deferred_decrement_count++] = h;
    }
  }

  // Apply all deferred decrements. Called after remark (STW).
  void apply_deferred_decrements() {
    for (int i = 0; i < _deferred_decrement_count; i++) {
      _deferred_decrements[i]->decrement_remote_refcount();
    }
    if (_deferred_decrement_count > 0) {
      log_info(gc)("Applied %d deferred remote_refcount decrements", _deferred_decrement_count);
    }
    _deferred_decrement_count = 0;
  }

  // Check if concurrent marking is in progress
  bool concurrent_marking_active() const;

  void clear_remote_roots() { _remote_roots_count = 0; }
  void add_remote_root(uintptr_t handle_id) {
    if (_remote_roots_count < MAX_REMOTE_ROOTS) {
      // Simple dedup: check last few entries (most duplicates are adjacent)
      for (int i = (_remote_roots_count > 8 ? _remote_roots_count - 8 : 0);
           i < _remote_roots_count; i++) {
        if (_remote_roots[i] == handle_id) return;
      }
      _remote_roots[_remote_roots_count++] = handle_id;
    }
  }
  int remote_roots_count() const { return _remote_roots_count; }
  const uintptr_t* remote_roots() const { return _remote_roots; }

  uint32_t gc_epoch() const { return _gc_epoch; }
  void increment_gc_epoch() {
    // Rotate stats: current → previous, clear current
    memcpy(_prev_hotness_stats, _hotness_stats, sizeof(_hotness_stats));
    memset(_hotness_stats, 0, sizeof(_hotness_stats));
    _gc_epoch++;
  }

  // Record an Old object's hotness during classification/evacuation.
  void record_hotness(markWord mw, size_t word_size) {
    if (!mw.is_unlocked()) return;
    uintptr_t dist = mw.hotness_distance(_gc_epoch);
    _hotness_stats[dist].object_count++;
    _hotness_stats[dist].total_words += word_size;
  }

  // Get previous GC's stats for eviction decisions.
  const HotnessLevelStats* prev_hotness_stats() const { return _prev_hotness_stats; }

  // Determine eviction threshold: objects at or above this distance are cold.
  // Returns the distance threshold, or HOTNESS_LEVELS if nothing to evict.
  int eviction_threshold(size_t target_words) const {
    // Walk from coldest (15) to hottest (0), accumulating bytes.
    // Stop when we've accumulated enough to meet the target.
    size_t accumulated = 0;
    for (int level = HOTNESS_LEVELS - 1; level >= 0; level--) {
      accumulated += _prev_hotness_stats[level].total_words;
      if (accumulated >= target_words) return level;
    }
    return HOTNESS_LEVELS; // not enough cold objects
  }

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

  // Evict an entire region: evict all objects, tag incoming refs, free region.
  // Called during STW post-GC when heap pressure exceeds threshold.
  // Returns the number of objects evicted.
  int evict_region(HeapRegion* hr, RemoteHandleAllocBuffer* hab);

  // Tag incoming refs: scan heap for refs pointing into the given region,
  // replace them with shared_oop(handle). Called during STW.
  void tag_incoming_refs_to_region(HeapRegion* target_hr);

  // Full heap scan: tag ALL heap refs pointing to any region in the
  // eviction set. eviction_set[i]==true means region i is a candidate.
  // More expensive than remset-based scan but catches dirty cards not
  // yet refined and other remset gaps. Returns total refs tagged.
  int tag_all_heap_refs_to_eviction_set(const bool* eviction_set, uint num_regions);

  int verify_no_untagged_refs_to_eviction_set(const bool* eviction_set, uint num_regions);

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
  // Walk the Handle table. For each dormant anchor (LOCAL + remote_refcount > 0),
  // call cl->do_oop. If GC moves the object, update Handle and rekey table entry.
  //
  // Collect moved entries for rehashing after the walk (can't modify hash
  // structure during iteration).
  static const int MAX_MOVED = 256;
  struct MovedEntry { HandleEntry* entry; uintptr_t old_addr; };
  MovedEntry moved[MAX_MOVED];
  int num_moved = 0;

  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      RemoteHandle* h = e->_handle;
      if (h != nullptr && h->is_local() && h->remote_refcount() > 0) {
        oop obj = cast_to_oop(e->_obj_addr);
        cl->do_oop(&obj);
        uintptr_t new_addr = cast_from_oop<uintptr_t>(obj);
        if (new_addr != e->_obj_addr) {
          h->set_local(cast_from_oop<void*>(obj));
          if (num_moved < MAX_MOVED) {
            moved[num_moved++] = {e, e->_obj_addr};
          }
          e->_obj_addr = new_addr;
        }
      }
      e = e->_next;
    }
  }

  // Rehash moved entries: unlink from old bucket, insert into new
  for (int i = 0; i < num_moved; i++) {
    HandleEntry* entry = moved[i].entry;
    size_t old_idx = hash_obj(moved[i].old_addr);
    size_t new_idx = hash_obj(entry->_obj_addr);
    if (old_idx != new_idx) {
      // Unlink from old bucket
      HandleEntry** pp = &_table[old_idx];
      while (*pp != nullptr) {
        if (*pp == entry) {
          *pp = entry->_next;
          break;
        }
        pp = &(*pp)->_next;
      }
      // Insert into new bucket
      entry->_next = _table[new_idx];
      _table[new_idx] = entry;
    }
  }
}

#endif // SHARE_GC_G1_G1REMOTEMEMORYMANAGER_HPP

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
class Klass;
class WorkerThreads;

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

  // Hash table for secondary index.
  // 1M buckets: with ~800K handles, avg chain length < 1.
  static const size_t TABLE_SIZE = (1 << 20);
  HandleEntry** _table;
  HandleEntry** _eviction_table;
  volatile int _table_lock;
  RemoteHandle* _local_handles_head;
  size_t _local_handle_count;
  size_t* _local_handle_region_counts;
  RemoteHandle** _local_handle_region_heads;
  uint _local_handle_region_capacity;
  volatile int _local_handle_lock;

  void table_lock()   { while (Atomic::cmpxchg(&_table_lock, 0, 1) != 0) { /* spin */ } }
  void table_unlock() { Atomic::release_store(&_table_lock, 0); }
  void local_handle_lock()   { while (Atomic::cmpxchg(&_local_handle_lock, 0, 1) != 0) { /* spin */ } }
  void local_handle_unlock() { Atomic::release_store(&_local_handle_lock, 0); }

  bool ensure_local_handle_region_counts_locked();
  uint local_handle_region_index(uintptr_t addr) const;
  void link_local_handle_region_locked(RemoteHandle* h, uintptr_t addr);
  void unlink_local_handle_region_locked(RemoteHandle* h);
  void move_local_handle_region(RemoteHandle* h,
                                uintptr_t old_addr,
                                uintptr_t new_addr);
  void link_local_handle_locked(RemoteHandle* h, uintptr_t local_addr = 0);
  void unlink_local_handle_locked(RemoteHandle* h, uintptr_t local_addr = 0);
  void link_local_handle(RemoteHandle* h);
  void unlink_local_handle(RemoteHandle* h);
  void append_pending_local_handle(RemoteHandle* h,
                                   RemoteHandle** head,
                                   RemoteHandle** tail,
                                   size_t* count);

  // Stripe locks for parallel ensure_handle_for (Phase B).
  static const int TABLE_STRIPES = 4096;
  volatile int _stripe_locks[TABLE_STRIPES];
  volatile int _alloc_lock;

  void stripe_lock(size_t bucket_idx) {
    int stripe = (int)(bucket_idx % TABLE_STRIPES);
    while (Atomic::cmpxchg(&_stripe_locks[stripe], 0, 1) != 0) { /* spin */ }
  }
  void stripe_unlock(size_t bucket_idx) {
    int stripe = (int)(bucket_idx % TABLE_STRIPES);
    Atomic::release_store(&_stripe_locks[stripe], 0);
  }

  HandleEntry* alloc_entry_locked() {
    while (Atomic::cmpxchg(&_alloc_lock, 0, 1) != 0) { /* spin */ }
    HandleEntry* e = alloc_entry();
    Atomic::release_store(&_alloc_lock, 0);
    return e;
  }

  // Allocate a new entry chunk and link into global chain (under alloc_lock).
  // Called once per 256 entries — low contention.
  HandleEntryChunk* alloc_new_entry_chunk() {
    while (Atomic::cmpxchg(&_alloc_lock, 0, 1) != 0) { /* spin */ }
    HandleEntryChunk* chunk = new HandleEntryChunk();
    chunk->_next = _entry_chunks;
    _entry_chunks = chunk;
    Atomic::release_store(&_alloc_lock, 0);
    return chunk;
  }

public:
  // Per-worker entry allocation buffer — bump-pointer within a chunk.
  // One lock acquisition per 256 entries instead of per entry.
  struct HandleEntryAllocBuffer {
    HandleEntry* _top;
    HandleEntry* _end;
    HandleEntryAllocBuffer() : _top(nullptr), _end(nullptr) {}
    HandleEntry* allocate() {
      if (_top < _end) return _top++;
      return nullptr;
    }
    void set_chunk(HandleEntryChunk* chunk) {
      _top = &chunk->_entries[0];
      _end = &chunk->_entries[ENTRY_CHUNK_CAPACITY];
    }
  };

private:

  static size_t hash_obj(uintptr_t addr) {
    size_t h = addr >> 3;
    h ^= (h >> 17);
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= (h >> 31);
    return h & (TABLE_SIZE - 1);
  }

  RemoteHandle* handle_for_eviction_addr(uintptr_t addr) const {
    size_t idx = hash_obj(addr);

    HandleEntry* e = _eviction_table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr) return e->_handle;
      e = e->_next;
    }
    return nullptr;
  }

  void remember_eviction_alias_locked(uintptr_t addr, RemoteHandle* h) {
    if (addr == 0 || h == nullptr) return;

    size_t idx = hash_obj(addr);
    HandleEntry* e = _eviction_table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr && e->_handle == h) return;
      e = e->_next;
    }

    HandleEntry* alias = alloc_entry();
    alias->init(addr, h, _eviction_table[idx]);
    _eviction_table[idx] = alias;
  }

  // Log-only MoleculeGC-inspired class-pattern profiler.
  struct MoleculeKlassProfileEntry {
    Klass*   _klass;
    uint64_t _old_copies;
    uint64_t _old_copy_bytes;
    uint64_t _out_edges;
    uint64_t _mutation_overwrites;
  };

  struct MoleculeEdgeProfileEntry {
    Klass*   _from;
    Klass*   _to;
    uint64_t _promotion_edges;
    uint64_t _mutation_overwrites;
    uint64_t _array_source_edges;
  };

  MoleculeKlassProfileEntry* _molecule_klass_profile;
  MoleculeEdgeProfileEntry*  _molecule_edge_profile;
  uint                       _molecule_profile_capacity;
  volatile int               _molecule_profile_lock;
  volatile uint64_t          _molecule_profile_old_copies;
  volatile uint64_t          _molecule_profile_old_copy_bytes;
  volatile uint64_t          _molecule_profile_promotion_edges;
  volatile uint64_t          _molecule_profile_array_edges;
  volatile uint64_t          _molecule_profile_mutation_probes;
  volatile uint64_t          _molecule_profile_mutation_overwrites;
  volatile uint64_t          _molecule_profile_dropped_klass;
  volatile uint64_t          _molecule_profile_dropped_edges;

  void molecule_profile_lock() {
    while (Atomic::cmpxchg(&_molecule_profile_lock, 0, 1) != 0) { /* spin */ }
  }
  void molecule_profile_unlock() {
    Atomic::release_store(&_molecule_profile_lock, 0);
  }
  bool molecule_profile_ready() const {
    return _molecule_klass_profile != nullptr &&
           _molecule_edge_profile != nullptr &&
           _molecule_profile_capacity > 0;
  }
  size_t molecule_profile_klass_hash(Klass* klass) const;
  size_t molecule_profile_edge_hash(Klass* from, Klass* to) const;
  void record_molecule_profile_klass_locked(Klass* klass,
                                            uint64_t old_copies,
                                            uint64_t old_copy_bytes,
                                            uint64_t out_edges,
                                            uint64_t mutation_overwrites);
  void record_molecule_profile_edge_locked(Klass* from,
                                           Klass* to,
                                           bool array_source,
                                           bool mutation);

public:
  G1RemoteMemoryManager(G1CollectedHeap* g1h);
  ~G1RemoteMemoryManager();

  bool molecule_profile_enabled() const { return molecule_profile_ready(); }
  void record_molecule_profile_edge(Klass* from, Klass* to,
                                    bool array_source, bool mutation);
  void record_molecule_profile_old_copy(oop obj, size_t word_size);
  void record_molecule_profile_ref_overwrite(void* field, bool is_narrow);
  void log_molecule_profile_summary();

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
    link_local_handle(h);

    HandleEntry* entry = alloc_entry();
    entry->init(addr, h, _table[idx]);
    _table[idx] = entry;
    table_unlock();
    return h;
  }

  // Thread-safe variant for parallel Phase B: uses per-bucket stripe locks
  // instead of the global table_lock. Multiple workers can create handles
  // concurrently for objects that hash to different stripes.
  // Each worker supplies its own HandleEntryAllocBuffer (EAB) for lock-free
  // entry allocation (one alloc_lock acquisition per 256 entries).
  RemoteHandle* ensure_handle_for_parallel(oop obj, RemoteHandleAllocBuffer* hab,
                                           HandleEntryAllocBuffer* eab,
                                           RemoteHandle** pending_head = nullptr,
                                           RemoteHandle** pending_tail = nullptr,
                                           size_t* pending_count = nullptr) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    stripe_lock(idx);
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr && e->_handle->is_local()) {
        RemoteHandle* existing = e->_handle;
        stripe_unlock(idx);
        return existing;
      }
      e = e->_next;
    }
    HandleEntry* entry = eab->allocate();
    if (entry == nullptr) {
      HandleEntryChunk* chunk = alloc_new_entry_chunk();
      eab->set_chunk(chunk);
      entry = eab->allocate();
    }
    RemoteHandle* h = _handle_allocator.allocate_handle(hab);
    h->initialize(cast_from_oop<void*>(obj));
    entry->init(addr, h, _table[idx]);
    _table[idx] = entry;
    stripe_unlock(idx);
    if (pending_head != nullptr && pending_tail != nullptr && pending_count != nullptr) {
      append_pending_local_handle(h, pending_head, pending_tail, pending_count);
    } else {
      link_local_handle(h);
    }
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
    link_local_handle(h);

    HandleEntry* entry = alloc_entry();
    entry->init(addr, h, _table[idx]);
    _table[idx] = entry;
    table_unlock();
    return h;
  }

  RemoteHandle* ensure_dormant_anchor_for_parallel(oop obj, RemoteHandleAllocBuffer* hab,
                                                   HandleEntryAllocBuffer* eab,
                                                   RemoteHandle** pending_head,
                                                   RemoteHandle** pending_tail,
                                                   size_t* pending_count) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    stripe_lock(idx);
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr && e->_handle->is_local()) {
        RemoteHandle* existing = e->_handle;
        existing->set_dormant();
        stripe_unlock(idx);
        return existing;
      }
      e = e->_next;
    }

    HandleEntry* entry = eab->allocate();
    if (entry == nullptr) {
      HandleEntryChunk* chunk = alloc_new_entry_chunk();
      eab->set_chunk(chunk);
      entry = eab->allocate();
    }
    RemoteHandle* h = _handle_allocator.allocate_handle(hab);
    h->initialize_dormant(cast_from_oop<void*>(obj));
    entry->init(addr, h, _table[idx]);
    _table[idx] = entry;
    stripe_unlock(idx);

    if (pending_head != nullptr && pending_tail != nullptr && pending_count != nullptr) {
      append_pending_local_handle(h, pending_head, pending_tail, pending_count);
    } else {
      link_local_handle(h);
    }
    return h;
  }

  int collect_remote_anchor_addrs_in_regions(const bool* region_set, uint num_regions,
                                             uintptr_t* addrs, int max_addrs,
                                             bool* overflow);
  int mark_remote_anchor_regions_in_set(const bool* region_set, uint num_regions,
                                        bool* anchor_regions,
                                        int* anchors_seen = nullptr,
                                        const bool* region_set2 = nullptr,
                                        bool* anchor_regions2 = nullptr,
                                        int* marked_regions2 = nullptr,
                                        int* anchors_seen2 = nullptr);

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

  // Look up a Handle by the table key without requiring it to still be LOCAL.
  // Evicted handles keep their original local address as the secondary key, so
  // this lets fetch-side validation repair clean stale oops that still contain
  // that old eviction address.
  RemoteHandle* handle_for_addr_any_state(uintptr_t addr) const {
    size_t idx = hash_obj(addr);

    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr) return e->_handle;
      e = e->_next;
    }

    return handle_for_eviction_addr(addr);
  }

  RemoteHandle* handle_for_stale_eviction_addr(uintptr_t addr) const {
    size_t idx = hash_obj(addr);

    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr) {
        uintptr_t state = e->_handle->load_state_and_addr_acquire() &
                          REMOTE_HANDLE_STATE_MASK;
        if (state == REMOTE_HANDLE_REMOTE ||
            state == REMOTE_HANDLE_FETCHING) {
          return e->_handle;
        }
      }
      e = e->_next;
    }

    RemoteHandle* alias = handle_for_eviction_addr(addr);
    if (alias != nullptr) {
      uintptr_t state = alias->load_state_and_addr_acquire() &
                        REMOTE_HANDLE_STATE_MASK;
      if (state != REMOTE_HANDLE_DEAD) {
        return alias;
      }
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
  void update_handle_for_evacuation(RemoteHandle* expected_h, oop old_obj, oop new_obj) {
    uintptr_t old_addr = cast_from_oop<uintptr_t>(old_obj);
    uintptr_t new_addr = cast_from_oop<uintptr_t>(new_obj);
    size_t old_idx = hash_obj(old_addr);
    size_t new_idx = hash_obj(new_addr);

    table_lock();
    HandleEntry** pp = &_table[old_idx];
    while (*pp != nullptr) {
      HandleEntry* entry = *pp;
      if (entry->_obj_addr == old_addr &&
          (expected_h == nullptr || entry->_handle == expected_h)) {
        // Only rekey LOCAL handles. REMOTE/DEAD entries are stale —
        // the address was reused after the original object's region was freed.
        if (!entry->_handle->is_local()) {
          pp = &(entry->_next);
          continue;
        }
        // Keep the old local address as a repair alias.  Direct clean refs can
        // miss normal G1 remset processing in the remote/FCR path; if they are
        // discovered after the old region has been filled, the alias lets the
        // verifier rewrite them to the stable Handle instead of leaving a
        // stale raw heap pointer behind.
        remember_eviction_alias_locked(old_addr, entry->_handle);
        *pp = entry->_next;  // unlink from old bucket

        move_local_handle_region(entry->_handle, old_addr, new_addr);
        entry->_handle->set_local(cast_from_oop<void*>(new_obj));

        entry->_obj_addr = new_addr;
        entry->_next = _table[new_idx];
        _table[new_idx] = entry;

        if (expected_h != nullptr) {
          table_unlock();
          return;
        }
        continue;
      }
      pp = &(entry->_next);
    }
    if (expected_h != nullptr && expected_h->is_local()) {
      remember_eviction_alias_locked(old_addr, expected_h);
      move_local_handle_region(expected_h, old_addr, new_addr);
      expected_h->set_local(cast_from_oop<void*>(new_obj));
      HandleEntry* entry = alloc_entry();
      entry->init(new_addr, expected_h, _table[new_idx]);
      _table[new_idx] = entry;
    }
    table_unlock();
  }

  void update_handle_for_evacuation(oop old_obj, oop new_obj) {
    update_handle_for_evacuation(nullptr, old_obj, new_obj);
  }

  // Rekey a Handle entry when an object is fetched to a new local address.
  // Uses the Handle's saved _eviction_addr for O(1) old-bucket lookup.
  void rekey_handle_on_fetch(RemoteHandle* h, void* new_addr) {
    uintptr_t new_uaddr = (uintptr_t)new_addr;
    uintptr_t old_addr = h->_eviction_addr;
    table_lock();
    remember_eviction_alias_locked(old_addr, h);
    if (old_addr != 0) {
      size_t old_idx = hash_obj(old_addr);
      HandleEntry** pp = &_table[old_idx];
      while (*pp != nullptr) {
        if ((*pp)->_handle == h) {
          HandleEntry* e = *pp;
          *pp = e->_next;
          e->_obj_addr = new_uaddr;
          size_t new_idx = hash_obj(new_uaddr);
          e->_next = _table[new_idx];
          _table[new_idx] = e;
          table_unlock();
          return;
        }
        pp = &(*pp)->_next;
      }
    }
    HandleEntry* entry = alloc_entry();
    entry->init(new_uaddr, h, _table[hash_obj(new_uaddr)]);
    _table[hash_obj(new_uaddr)] = entry;
    table_unlock();
  }

  void publish_local_handle(RemoteHandle* h, void* local_addr);
  void publish_local_handles(RemoteHandle** handles, HeapWord** local_addrs, uint count);
  void make_handle_remote(RemoteHandle* h, uintptr_t remote_id);
  void mark_handle_dead(RemoteHandle* h);
  size_t local_handle_count() const { return _local_handle_count; }

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
  static const size_t EDGE_TABLE_BUCKETS = 65536;

  struct EdgeTableEntry {
    ObjectEdgeTable* _table;
    EdgeTableEntry*  _next;
  };
  EdgeTableEntry* _edge_buckets[EDGE_TABLE_BUCKETS];
  volatile int _edge_bucket_locks[EDGE_TABLE_BUCKETS];

  void edge_bucket_lock(size_t bucket_idx) {
    while (Atomic::cmpxchg(&_edge_bucket_locks[bucket_idx], 0, 1) != 0) { /* spin */ }
  }
  void edge_bucket_unlock(size_t bucket_idx) {
    Atomic::release_store(&_edge_bucket_locks[bucket_idx], 0);
  }

  static size_t hash_handle(RemoteHandle* h) {
    return ((uintptr_t)h >> 4) % EDGE_TABLE_BUCKETS;
  }

public:
  void store_edge_table(ObjectEdgeTable* et) {
    size_t idx = hash_handle(et->_source_handle);
    EdgeTableEntry* entry = (EdgeTableEntry*)os::malloc(sizeof(EdgeTableEntry), mtGC);
    entry->_table = et;
    edge_bucket_lock(idx);
    entry->_next = _edge_buckets[idx];
    _edge_buckets[idx] = entry;
    edge_bucket_unlock(idx);
  }

  void append_pending_edge_table(ObjectEdgeTable* et,
                                 EdgeTableEntry** head,
                                 EdgeTableEntry** tail,
                                 size_t* count) {
    if (et == nullptr || head == nullptr || tail == nullptr || count == nullptr) {
      return;
    }
    EdgeTableEntry* entry = (EdgeTableEntry*)os::malloc(sizeof(EdgeTableEntry), mtGC);
    entry->_table = et;
    entry->_next = nullptr;
    if (*tail != nullptr) {
      (*tail)->_next = entry;
    } else {
      *head = entry;
    }
    *tail = entry;
    (*count)++;
  }

  void store_edge_table_batch(EdgeTableEntry* head, size_t count) {
    if (head == nullptr || count == 0) {
      return;
    }
    EdgeTableEntry* cur = head;
    while (cur != nullptr) {
      EdgeTableEntry* next = cur->_next;
      size_t idx = hash_handle(cur->_table->_source_handle);
      edge_bucket_lock(idx);
      cur->_next = _edge_buckets[idx];
      _edge_buckets[idx] = cur;
      edge_bucket_unlock(idx);
      cur = next;
    }
  }

  ObjectEdgeTable* take_edge_table(RemoteHandle* h) {
    size_t idx = hash_handle(h);
    edge_bucket_lock(idx);
    EdgeTableEntry** pp = &_edge_buckets[idx];
    while (*pp != nullptr) {
      if ((*pp)->_table->_source_handle == h) {
        EdgeTableEntry* entry = *pp;
        ObjectEdgeTable* table = entry->_table;
        *pp = entry->_next;
        os::free(entry);
        edge_bucket_unlock(idx);
        return table;
      }
      pp = &(*pp)->_next;
    }
    edge_bucket_unlock(idx);
    return nullptr;
  }

  void remove_edge_table(RemoteHandle* h) {
    size_t idx = hash_handle(h);
    edge_bucket_lock(idx);
    EdgeTableEntry** pp = &_edge_buckets[idx];
    while (*pp != nullptr) {
      if ((*pp)->_table->_source_handle == h) {
        EdgeTableEntry* entry = *pp;
        *pp = entry->_next;
        ObjectEdgeTable::free(entry->_table);
        os::free(entry);
        edge_bucket_unlock(idx);
        return;
      }
      pp = &(*pp)->_next;
    }
    edge_bucket_unlock(idx);
  }

  // Build edge table for an object about to be evicted.
  // Scans all oop fields, creates dormant anchors for targets, records edges.
  // Must be called BEFORE eviction (object bytes still readable locally).
  ObjectEdgeTable* build_edge_table(oop obj, RemoteHandle* obj_handle,
                                    RemoteHandleAllocBuffer* hab,
                                    bool* zero_edges = nullptr,
                                    HandleEntryAllocBuffer* eab = nullptr,
                                    RemoteHandle** pending_head = nullptr,
                                    RemoteHandle** pending_tail = nullptr,
                                    size_t* pending_count = nullptr);

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

private:
  uint32_t* _eviction_backoff_until_epoch;
  uint      _eviction_backoff_capacity;
  bool*     _fast_phase_c_source_hints;
  uint      _fast_phase_c_source_hint_capacity;
  uint      _fast_phase_c_source_hint_count;
  volatile int _fast_phase_c_source_hint_lock;
  uint64_t* _inbound_region_summary;
  uint      _inbound_region_summary_capacity;
  volatile int _inbound_region_summary_lock;
  volatile uint64_t _inbound_region_summary_edges;
  volatile uint64_t _inbound_region_summary_duplicates;
  int       _last_phase_c_tagged;
  int       _last_phase_c_no_handle;
  int       _last_phase_c_untaggable;

  enum DenseSegmentState : uint32_t {
    DenseSegmentNone     = 0,
    DenseSegmentRemote   = 1,
    DenseSegmentFetching = 2,
    DenseSegmentLocal    = 3
  };

  enum DenseSegmentEdgeKind : uint8_t {
    DenseSegmentEdgeHandle = 0,
    DenseSegmentEdgeDirect = 1
  };

  struct DenseSegmentEdge {
    uint32_t     field_offset;
    uint8_t      kind;
    RemoteHandle* target_handle;
    uintptr_t    target_addr;
  };

  struct DenseSegmentEntry {
    uint64_t          segment_id;
    uintptr_t         base;
    size_t            byte_size;
    DenseSegmentEdge* edges;
    uint32_t          edge_count;
    uint32_t          flags;
    uint32_t          last_evict_epoch;
    uint32_t          last_fetch_epoch;
    uint16_t          fetch_backoff_shift;
    uint16_t          fetch_churn_count;
    volatile uint32_t state;

    void clear() {
      segment_id = 0;
      base = 0;
      byte_size = 0;
      edges = nullptr;
      edge_count = 0;
      flags = 0;
      last_evict_epoch = 0;
      last_fetch_epoch = 0;
      fetch_backoff_shift = 0;
      fetch_churn_count = 0;
      state = DenseSegmentNone;
    }
  };

  DenseSegmentEntry* _dense_segments;
  uint               _dense_segment_capacity;
  volatile int       _dense_segment_lock;
  uint64_t           _dense_segment_next_id;
  volatile uint64_t  _dense_segment_evict_success;
  volatile uint64_t  _dense_segment_evict_failures;
  volatile uint64_t  _dense_segment_fetch_success;
  volatile uint64_t  _dense_segment_fetch_failures;
  volatile uint64_t  _dense_segment_remote_bytes;

  void ensure_eviction_backoff_capacity(uint num_regions);
  void ensure_fast_phase_c_source_hint_capacity(uint num_regions);
  void ensure_inbound_region_summary_capacity(uint num_regions);
  bool ensure_dense_segment_capacity(uint num_regions);
  void release_dense_segment_edges(DenseSegmentEdge* edges, uint32_t count);
  bool scan_dense_segment_region(HeapRegion* hr,
                                 bool build_edges,
                                 DenseSegmentEdge** out_edges,
                                 uint32_t* out_edge_count,
                                 size_t* out_object_count,
                                 const char** reason);
  void patch_dense_segment_boundary_edges(DenseSegmentEntry* entry,
                                          HeapRegion* hr);
  void dense_segment_lock() {
    while (Atomic::cmpxchg(&_dense_segment_lock, 0, 1) != 0) { /* spin */ }
  }
  void dense_segment_unlock() {
    Atomic::release_store(&_dense_segment_lock, 0);
  }
  void record_phase_c_counts(int tagged, int no_handle, int untaggable) {
    _last_phase_c_tagged = tagged;
    _last_phase_c_no_handle = no_handle;
    _last_phase_c_untaggable = untaggable;
  }
  void fast_phase_c_source_hint_lock() {
    while (Atomic::cmpxchg(&_fast_phase_c_source_hint_lock, 0, 1) != 0) { /* spin */ }
  }
  void fast_phase_c_source_hint_unlock() {
    Atomic::release_store(&_fast_phase_c_source_hint_lock, 0);
  }
  void inbound_region_summary_lock() {
    while (Atomic::cmpxchg(&_inbound_region_summary_lock, 0, 1) != 0) { /* spin */ }
  }
  void inbound_region_summary_unlock() {
    Atomic::release_store(&_inbound_region_summary_lock, 0);
  }

public:
  // Remote root set: handle_ids for CMD_REPORT_REMOTE_ROOTS_V2.
  // Sources: concurrent marking logs + Phase C tagged fields + refcount>0.
  // Dynamically allocated per collect_dead cycle.
  uintptr_t* _remote_roots;
  int        _remote_roots_count;
  int        _cm_remote_roots_count;
  int        _remote_roots_capacity;

  // Cross-boundary roots: LOCAL handles referenced by live REMOTE objects.
  // Populated by trace_and_report(), consumed as GC roots during Phase D.
  static const int MAX_CROSS_ROOTS = 4096;
  RemoteHandle* _cross_roots[MAX_CROSS_ROOTS];
  int           _cross_roots_count;

  // Full remote trace-and-report is expensive after bulk eviction. Dead
  // handles are currently reported but not reclaimed, so repeated traces are
  // only needed to refresh cross-boundary roots and executor liveness state.
  bool   _remote_collection_has_trace;
  uint   _remote_collection_skipped;
  size_t _remote_collection_last_handles_allocated;

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

  // ============================================================
  // Tagged Field List (Bug 2 fix: side list for GC root scanning)
  // ============================================================
  // Records heap fields that were rewritten to shared_oop(Handle) during
  // Phase C eviction tagging. Processed as roots during GC to ensure
  // Handle targets in the collection set are evacuated and Handles updated.
  //
  // This avoids polluting G1's card/remset system with tagged oops, which
  // break refinement (Handle can transition REMOTE→LOCAL without any heap
  // field write, so cards can't track the indirection).
  enum TaggedFieldKind : uint8_t {
    TaggedFieldHandle = 0,
    TaggedFieldDirect = 1,
    TaggedFieldDenseHandle = 2
  };

  struct TaggedFieldEntry {
    oop*          _field_addr;
    RemoteHandle* _handle;
    uintptr_t     _tagged_raw;
    uint8_t       _kind;

    bool is_handle() const {
      return _kind == TaggedFieldHandle || _kind == TaggedFieldDenseHandle;
    }
    bool is_direct() const { return _kind == TaggedFieldDirect; }
    bool is_dense_handle() const { return _kind == TaggedFieldDenseHandle; }
  };
private:
  TaggedFieldEntry* _tagged_fields;
  int _tagged_field_count;
  int _tagged_field_capacity;
  volatile int _tagged_field_lock;

  void tagged_field_lock()   { while (Atomic::cmpxchg(&_tagged_field_lock, 0, 1) != 0) { /* spin */ } }
  void tagged_field_unlock() { Atomic::release_store(&_tagged_field_lock, 0); }

  void add_tagged_field_entry_locked(const TaggedFieldEntry& entry) {
    if (_tagged_field_count >= _tagged_field_capacity) {
      int new_cap = (_tagged_field_capacity == 0) ? 4096 : _tagged_field_capacity * 2;
      TaggedFieldEntry* new_buf = NEW_C_HEAP_ARRAY(TaggedFieldEntry, new_cap, mtGC);
      if (_tagged_fields != nullptr) {
        memcpy(new_buf, _tagged_fields, _tagged_field_count * sizeof(TaggedFieldEntry));
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _tagged_fields);
      }
      _tagged_fields = new_buf;
      _tagged_field_capacity = new_cap;
    }
    _tagged_fields[_tagged_field_count++] = entry;
  }

public:
  void add_tagged_field_entry(const TaggedFieldEntry& entry) {
    tagged_field_lock();
    add_tagged_field_entry_locked(entry);
    tagged_field_unlock();
  }

  void add_tagged_field(oop* field_addr, RemoteHandle* h) {
    TaggedFieldEntry entry;
    entry._field_addr = field_addr;
    entry._handle = h;
    entry._tagged_raw = 0;
    entry._kind = TaggedFieldHandle;
    add_tagged_field_entry(entry);
  }

  void add_direct_tagged_field(oop* field_addr, uintptr_t tagged_raw) {
    TaggedFieldEntry entry;
    entry._field_addr = field_addr;
    entry._handle = nullptr;
    entry._tagged_raw = tagged_raw;
    entry._kind = TaggedFieldDirect;
    add_tagged_field_entry(entry);
  }

  int tagged_field_count() const { return _tagged_field_count; }
  const TaggedFieldEntry* tagged_fields() const { return _tagged_fields; }
  int last_phase_c_tagged() const { return _last_phase_c_tagged; }
  int last_phase_c_no_handle() const { return _last_phase_c_no_handle; }
  int last_phase_c_untaggable() const { return _last_phase_c_untaggable; }
  bool has_local_handle_region_counts() const {
    return _local_handle_region_counts != nullptr;
  }
  size_t local_handle_count_for_region(uint region_idx) const {
    if (_local_handle_region_counts == nullptr ||
        region_idx >= _local_handle_region_capacity) {
      return SIZE_MAX;
    }
    return Atomic::load(&_local_handle_region_counts[region_idx]);
  }

  // Post-evacuation fixup: iterate tagged fields, update Handles whose
  // targets have been forwarded during evacuation. Lazily removes stale
  // entries (fields no longer tagged). Returns number of handles updated.
  int fixup_tagged_field_handles();
  int fixup_all_local_handles();
  int fixup_local_handles_in_regions(const bool* region_set, uint num_regions);
  int purge_stale_local_handles(const char* phase, int log_limit = 16);
  size_t rebuild_handle_table_from_handles();

  // Post-evacuation fixup: scan ALL old regions for refs to
  // collection-set regions. Must be called BEFORE free_collection_set.
  int fixup_stale_refs_in_old_regions(bool evacuation_failed);

  // Diagnostic counters for Task #11 — comparing do_oop_evac processing
  // of FCR-source fields vs fixup NULLing of stale refs from FCR sources.
  // If fixup > evac, mutator writes are bypassing do_oop_evac.
  volatile uint64_t _fcr_evac_writes;
  volatile uint64_t _fcr_fixup_nulls;
  void record_fcr_evac_write()  { Atomic::inc(&_fcr_evac_writes); }
  void record_fcr_fixup_null()  { Atomic::inc(&_fcr_fixup_nulls); }
  uint64_t fcr_evac_writes() const  { return Atomic::load(&_fcr_evac_writes); }
  uint64_t fcr_fixup_nulls() const  { return Atomic::load(&_fcr_fixup_nulls); }

  // Mutator-side remote access diagnostics. These are intentionally coarse
  // atomic counters so Spark runs can be diagnosed without JVM attach.
  volatile uint64_t _resolve_fast_local;
  volatile uint64_t _resolve_fast_remote;
  volatile uint64_t _resolve_fast_fetching;
  volatile uint64_t _resolve_fast_dead;
  volatile uint64_t _resolve_slow_entries;
  volatile uint64_t _resolve_no_safepoint_entries;
  volatile uint64_t _fetch_success;
  volatile uint64_t _fetch_failures;
  volatile uint64_t _fetch_words;
  volatile uint64_t _fetch_elapsed_counter;
  volatile uint64_t _fetch_retries;
  volatile uint64_t _fetch_wait_slow;
  volatile uint64_t _fetch_wait_no_safepoint;
  volatile uint64_t _fetch_wait_hard;
  volatile uint64_t _fetch_wait_loops;
  volatile uint64_t _fetch_progress_next;
  volatile uint64_t _fetch_batch_requests;
  volatile uint64_t _fetch_batch_returned;
  volatile uint64_t _fetch_batch_installed;
  volatile uint64_t _fetch_prefetch_installed;
  volatile uint64_t _fetch_prefetch_raced;
  volatile uint64_t _fetch_prefetch_failed;
  volatile uint64_t _fetch_prefetch_words;
  volatile uint64_t _fetch_batch_elapsed_counter;

  void record_resolve_fast_state(uintptr_t state) {
    if (state == REMOTE_HANDLE_LOCAL) {
      Atomic::inc(&_resolve_fast_local);
    } else if (state == REMOTE_HANDLE_REMOTE) {
      Atomic::inc(&_resolve_fast_remote);
    } else if (state == REMOTE_HANDLE_FETCHING) {
      Atomic::inc(&_resolve_fast_fetching);
    } else if (state == REMOTE_HANDLE_DEAD) {
      Atomic::inc(&_resolve_fast_dead);
    }
  }
  void record_resolve_slow_entry() { Atomic::inc(&_resolve_slow_entries); }
  void record_resolve_no_safepoint_entry() { Atomic::inc(&_resolve_no_safepoint_entries); }
  void record_fetch_result(size_t word_size, jlong elapsed_counter, bool success);
  void record_fetch_batch_result(size_t requested, size_t returned, size_t installed,
                                 size_t prefetched, size_t raced, size_t failed,
                                 size_t prefetch_words, jlong elapsed_counter);
  void record_fetch_retry() { Atomic::inc(&_fetch_retries); }
  void record_fetch_wait(bool no_safepoint, bool hard, uint64_t loops) {
    Atomic::add(&_fetch_wait_loops, loops);
    if (no_safepoint) {
      Atomic::inc(&_fetch_wait_no_safepoint);
    } else {
      Atomic::inc(&_fetch_wait_slow);
    }
    if (hard) {
      Atomic::inc(&_fetch_wait_hard);
    }
  }
  void log_remote_access_stats() const;

  // Post-eviction diagnostic: full heap + root sweep for stale pointers
  // into freed/guarded regions. O(heap) — gated by G1VerifyAfterEviction.
  int verify_no_stale_refs_to_freed_regions();

  // Check if concurrent marking is in progress
  bool concurrent_marking_active() const;

  void clear_remote_roots() {
    _remote_roots_count = 0;
    _cm_remote_roots_count = 0;
  }
  void ensure_remote_roots_capacity(int needed) {
    if (needed <= _remote_roots_capacity) return;
    int new_cap = MAX2(needed, _remote_roots_capacity * 2);
    if (new_cap < 16384) new_cap = 16384;
    uintptr_t* new_buf = NEW_C_HEAP_ARRAY(uintptr_t, new_cap, mtGC);
    if (_remote_roots != nullptr) {
      if (_remote_roots_count > 0) {
        memcpy(new_buf, _remote_roots, _remote_roots_count * sizeof(uintptr_t));
      }
      FREE_C_HEAP_ARRAY(uintptr_t, _remote_roots);
    }
    _remote_roots = new_buf;
    _remote_roots_capacity = new_cap;
  }
  void add_remote_root(uintptr_t handle_id) {
    ensure_remote_roots_capacity(_remote_roots_count + 1);
    _remote_roots[_remote_roots_count++] = handle_id;
  }
  int remote_roots_count() const { return _remote_roots_count; }
  void remember_remote_roots_as_cm_roots() { _cm_remote_roots_count = _remote_roots_count; }
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

  bool is_region_in_eviction_backoff(uint region_idx) const {
    return region_idx < _eviction_backoff_capacity &&
           _eviction_backoff_until_epoch[region_idx] > _gc_epoch;
  }
  void backoff_eviction_region(uint region_idx, uint gc_cycles);
  bool is_fast_phase_c_source_hint(uint region_idx) const;
  bool remember_fast_phase_c_source_hint(uint region_idx);
  uint fast_phase_c_source_hint_count() const { return _fast_phase_c_source_hint_count; }
  void record_inbound_ref(void* field_addr, oop target);
  void record_inbound_region_ref(uint source_region, uint target_region);
  bool is_inbound_source_for_eviction_set(uint source_region,
                                          const bool* eviction_set,
                                          uint num_regions) const;
  uint64_t inbound_region_summary_edges() const { return _inbound_region_summary_edges; }
  uint64_t inbound_region_summary_duplicates() const { return _inbound_region_summary_duplicates; }
  bool dense_segments_enabled() const;
  bool can_evict_dense_segment_region(HeapRegion* hr, const char** reason,
                                      size_t* object_count = nullptr);
  bool evict_dense_segment_region(HeapRegion* hr, const char** reason,
                                  size_t* object_count,
                                  uint32_t flags = 0);
  bool evict_dense_segment_region(HeapRegion* hr, uint32_t flags = 0);
  bool is_dense_segment_remote_addr(uintptr_t addr) const;
  bool is_dense_segment_managed_addr(uintptr_t addr) const;
  bool is_dense_segment_managed_region(HeapRegion* hr) const;
  bool localize_dense_segment_for_addr(uintptr_t addr);
  size_t dense_segment_remote_bytes() const {
    return (size_t)Atomic::load(&_dense_segment_remote_bytes);
  }

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

  bool try_fcr_lock() { return Atomic::cmpxchg(&_fcr_lock, 0, 1) == 0; }
  void fcr_unlock()   { Atomic::release_store(&_fcr_lock, 0); }

  // Allocate a new FCR region from the free region pool.
  // Must NOT be called from JRT_LEAF (needs Heap_lock).
  HeapRegion* allocate_new_fcr_region();

public:
  // Allocate space for a fetched object in the current FCR region.
  // Thread-safe (CAS-based bump pointer). Returns nullptr if FCR is full
  // and we can't allocate a new one (e.g., in JRT_LEAF context).
  HeapWord* allocate_in_fcr(size_t word_size);

  // Called during eviction Phase E when a region is freed.
  // If the freed region is the current FCR, clear the pointer to prevent
  // post-GC fetches from allocating into a freed/reused region.
  void invalidate_fcr_if_freed(HeapRegion* freed_hr) {
    if (_current_fcr == freed_hr) {
      _current_fcr = nullptr;
    }
  }

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

  int count_local_handles_in_region(HeapRegion* hr, int log_limit = 0);

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
  // ============================================================
  // Batch Eviction API
  // ============================================================
  // Split eviction into prepare → batch_send → finalize for batching.

  struct PreparedEviction {
    oop           obj;
    RemoteHandle* handle;
    Klass*        klass;
    size_t        word_size;
    size_t        slot_id;
    ObjectEdgeTable* edge_table;
  };

  // Prepare metadata: safety checks and handle lookup only. This is used before
  // late region guards, so rejected regions do not need to unwind edge tables.
  bool prepare_eviction_metadata(oop obj, RemoteHandleAllocBuffer* hab, PreparedEviction* out);

  // Finish preparation for entries that survived late guards: build edge table
  // and assign backend slot. Does NOT send to backend.
  bool finish_prepared_eviction(PreparedEviction* entry, RemoteHandleAllocBuffer* hab);

  bool finish_prepared_eviction_edges(PreparedEviction* entry,
                                      RemoteHandleAllocBuffer* hab,
                                      HandleEntryAllocBuffer* eab = nullptr,
                                      RemoteHandle** pending_head = nullptr,
                                      RemoteHandle** pending_tail = nullptr,
                                      size_t* pending_count = nullptr,
                                      EdgeTableEntry** pending_edge_head = nullptr,
                                      EdgeTableEntry** pending_edge_tail = nullptr,
                                      size_t* pending_edge_count = nullptr);

  // Prepare: metadata + finish. Does NOT send to backend.
  bool prepare_eviction(oop obj, RemoteHandleAllocBuffer* hab, PreparedEviction* out);
  static void log_prepare_eviction_stats();

  // Abort a prepared eviction before it is sent to the backend.
  // Drops edge-table refcounts installed by prepare_eviction().
  void abort_prepared_eviction(PreparedEviction* entry);

  // Link a per-worker chain of freshly created LOCAL handles with one global
  // list lock acquisition. Used by parallel Phase B handle creation.
  void link_local_handle_batch(RemoteHandle* head, RemoteHandle* tail, size_t count);

  // Count LOCAL handles into hr that are not among the prepared objects for
  // that region. These handles would remain LOCAL after publishing prepared
  // handles as REMOTE, so the region must not be sent/fillerized.
  int count_unprepared_local_handles_in_region(HeapRegion* hr,
                                               const PreparedEviction* entries,
                                               int start,
                                               int count,
                                               int log_limit = 0);

  // Batched form of count_unprepared_local_handles_in_region(). Scans all
  // allocated handles once for all candidate regions and fills blockers_by_region.
  int count_unprepared_local_handles_in_regions(const bool* eviction_candidates,
                                                const bool* region_complete,
                                                const int* region_start,
                                                const int* region_count,
                                                uint num_regions,
                                                const PreparedEviction* entries,
                                                int* blockers_by_region,
                                                int log_limit = 0);

  // Count any remaining LOCAL handles into complete candidate regions with one
  // allocator-wide scan. Used after publishing prepared handles REMOTE in E3.
  int count_local_handles_in_regions(const bool* eviction_candidates,
                                     const bool* region_complete,
                                     const int* region_count,
                                     uint num_regions,
                                     int* blockers_by_region,
                                     int log_limit = 0);

  // Finalize: set handle remote, mark word, fill with filler.
  // Called after backend confirms batch eviction.
  void finalize_eviction(PreparedEviction* entry);
  void finalize_evictions(PreparedEviction* entries, int start, int count, HeapRegion* hr);

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

  // Iterate cross-boundary roots: LOCAL handles that are kept alive by
  // live REMOTE objects. Populated by collect_dead_remote_objects() via
  // trace_and_report(). Complementary to oops_do_remote_anchors().
  template <typename OopClosureType>
  void oops_do_remote_cross_roots(OopClosureType* cl);

  // Tag incoming refs: scan heap for refs pointing into the given region,
  // replace them with shared_oop(handle). Called during STW.
  void tag_incoming_refs_to_region(HeapRegion* target_hr);

  // Full heap scan: tag ALL heap refs pointing to any region in the
  // eviction set. eviction_set[i]==true means region i is a candidate.
  // More expensive than remset-based scan but catches dirty cards not
  // yet refined and other remset gaps. Returns total refs tagged.
  int tag_all_heap_refs_to_eviction_set(const bool* eviction_set, uint num_regions,
                                        WorkerThreads* workers = nullptr, uint num_workers = 0);

  // Phase C.1 safety net: scan only the newly-evacuated area
  // [pre_evac_tops[i], top()) of each region.  Catches refs that the
  // general Phase C scan missed due to truncation or parse gaps.
  int tag_evacuated_area_refs_to_eviction_set(
      const bool* eviction_set, uint num_regions,
      HeapWord* const* pre_evac_tops);

  // Fast Phase C: scan only RSet entries + young/candidate/destination regions.
  // O(rset + young + candidates + destinations) instead of O(entire_heap).
  // pre_evac_tops[i] is region i's top() before evacuation — old regions where
  // top() > pre_evac_tops[i] received promoted objects and need scanning.
  int tag_refs_to_eviction_set_fast(const bool* eviction_set, uint num_regions,
                                    HeapWord* const* pre_evac_tops,
                                    WorkerThreads* workers = nullptr, uint num_workers = 0);

  // Bounded semantic pre-scan for old object-array containers. Mutates
  // eviction_set by removing dense candidates whose inbound object-array refs
  // cannot be represented by the current Phase C tagging rules.
  int prescan_old_objarray_sources_to_eviction_set(bool* eviction_set,
                                                   uint num_regions);

  int verify_no_untagged_refs_to_eviction_set(const bool* eviction_set, uint num_regions,
                                              HeapWord* const* pre_evac_tops = nullptr,
                                              bool full_heap = true);

  // Untag all tagged oop fields in the heap. Called on eviction abort to
  // restore clean oops, preventing barrier gaps from causing crashes.
  int untag_all_heap_refs(WorkerThreads* workers = nullptr, uint num_workers = 0);
  int untag_recorded_local_refs();
  int cleanup_recorded_direct_refs_after_dense_phase();

  // Patch fetched object's oop fields using sidecar edge table.
  // Called AFTER fetch_remote_object copies bytes, BEFORE set_local_release().
  // For each edge entry:
  //   - target LOCAL  → patch field to shared_oop(target_handle)
  //   - target REMOTE → patch field to shared_oop(target_handle)
  //   - target stale  → mark target DEAD and patch field to null
  //   - target DEAD   → patch field to null
  // Decrements remote_refcount on each target Handle.
  // Removes the edge table after patching.
  void patch_fetched_fields(RemoteHandle* source_handle, HeapWord* dest);

  // Returns true if the Handle's local address points to a valid live heap region.
  // If stale (freed/guarded/out-of-heap), logs a warning and marks the Handle DEAD.
  bool validate_local_handle_addr(RemoteHandle* h, const char* context,
                                  int* invalid_count = nullptr,
                                  int log_limit = 32);
  bool validate_anchor_addr(RemoteHandle* h);
};

// Template implementation — must be in header for instantiation.
template <typename OopClosureType>
void G1RemoteMemoryManager::oops_do_remote_anchors(OopClosureType* cl) {
  // Walk the allocator, not the intrusive LOCAL list. A missed list link must
  // not make a remote edge anchor invisible to root processing; stale anchors
  // are correctness-critical because fetch-time edge patching trusts them.
  //
  // For each local remote root, call cl->do_oop. This includes:
  //   - edge anchors (LOCAL + remote_refcount > 0), and
  //   - fetched-back evicted objects (LOCAL + eviction_word_size > 0).
  //
  // A fetched primitive array can be referenced only through a tagged handle in
  // a heap/root slot. If we miss that slot, treating the handle itself as a root
  // is the conservative fallback that prevents GC from freeing the FCR object
  // while Java can still resolve the handle.
  //
  // If GC moves the object, update the primary Handle immediately. Do not
  // rebuild the secondary address table here: this method is called from
  // evacuation workers, and a full table rebuild serializes the worker gang.
  class AnchorHandleClosure {
    G1RemoteMemoryManager* _rmm;
    OopClosureType* _cl;
    int _stale_anchors;

  public:
    AnchorHandleClosure(G1RemoteMemoryManager* rmm, OopClosureType* cl)
      : _rmm(rmm), _cl(cl), _stale_anchors(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;
      if (h->remote_refcount() == 0 && h->eviction_word_size() == 0) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      if ((sa & REMOTE_HANDLE_STATE_MASK) != REMOTE_HANDLE_LOCAL) return;

      if (!_rmm->validate_local_handle_addr(h, "STALE-ANCHOR", &_stale_anchors, 16)) {
        return;
      }

      oop old_obj = cast_to_oop((HeapWord*)(sa & REMOTE_HANDLE_ADDR_MASK));
      oop obj = old_obj;
      if (obj == nullptr || obj->klass_or_null() == nullptr) return;

      _cl->do_oop(&obj);

      uintptr_t new_addr = cast_from_oop<uintptr_t>(obj);
      if (new_addr != (sa & REMOTE_HANDLE_ADDR_MASK)) {
        _rmm->update_handle_for_evacuation(h, old_obj, obj);
      }
    }

    int stale_anchors() const { return _stale_anchors; }
  };

  AnchorHandleClosure hcl(this, cl);
  _handle_allocator.handles_do(&hcl);

  if (hcl.stale_anchors() > 16) {
    log_warning(gc)("STALE-ANCHOR: marked %d stale LOCAL anchor handles DEAD "
                    "(logged first 16)", hcl.stale_anchors());
  }

}

template <typename OopClosureType>
void G1RemoteMemoryManager::oops_do_remote_cross_roots(OopClosureType* cl) {
  int stale_cross_roots = 0;

  for (int i = 0; i < _cross_roots_count; i++) {
    RemoteHandle* h = _cross_roots[i];
    if (h == nullptr || !h->is_local()) continue;
    if (!validate_local_handle_addr(h, "STALE-CROSS-ROOT",
                                    &stale_cross_roots, 16)) {
      continue;
    }
    uintptr_t old_addr = (uintptr_t)h->local_addr();
    oop old_obj = cast_to_oop(h->local_addr());
    oop obj = old_obj;
    if (obj == nullptr || obj->klass_or_null() == nullptr) continue;
    cl->do_oop(&obj);
    uintptr_t new_addr = cast_from_oop<uintptr_t>(obj);
    if (new_addr != old_addr) {
      update_handle_for_evacuation(h, old_obj, obj);
    }
  }

  if (stale_cross_roots > 16) {
    log_warning(gc)("STALE-CROSS-ROOT: marked %d stale LOCAL cross-root handles "
                    "DEAD (logged first 16)", stale_cross_roots);
  }

}

#endif // SHARE_GC_G1_G1REMOTEMEMORYMANAGER_HPP

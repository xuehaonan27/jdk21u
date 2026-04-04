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

  // Object -> Handle mapping.
  // Used by write barrier (to find Handle for managed objects) and
  // by GC (to update Handle during evacuation).
  // Key: object address (oop cast to uintptr_t)
  // Value: RemoteHandle pointer
  //
  // This is a simple concurrent hash table.  In Phase 1 with manual
  // eviction the table is small.  Phase 2+ can switch to a more
  // efficient structure if needed.
  struct HandleEntry : public CHeapObj<mtGC> {
    uintptr_t     _obj_addr;   // key: object address
    RemoteHandle* _handle;     // value: Handle pointer (nullptr for Unique)
    bool          _is_shared;  // true = Shared (RC>1), false = Unique (RC=1)
    HandleEntry*  _next;       // chaining

    HandleEntry(uintptr_t addr, RemoteHandle* h, bool shared, HandleEntry* next)
      : _obj_addr(addr), _handle(h), _is_shared(shared), _next(next) {}
  };

  // Simple hash table for object->Handle mapping.
  // Low contention in Phase 1 (manual eviction, few managed objects).
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

  // Register an object as managed (Unique, no Handle).
  // Used by Phase 2 fixup for RC=1 objects.
  HandleEntry* alloc_entry(uintptr_t addr, RemoteHandle* h, bool shared, HandleEntry* next) {
    HandleEntry* e = (HandleEntry*)os::malloc(sizeof(HandleEntry), mtGC);
    e->_obj_addr = addr;
    e->_handle = h;
    e->_is_shared = shared;
    e->_next = next;
    return e;
  }

  void register_unique(oop obj) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    table_lock();
    _table[idx] = alloc_entry(addr, nullptr, false, _table[idx]);
    table_unlock();
  }

  RemoteHandle* create_handle_for(oop obj, RemoteHandleAllocBuffer* hab) {
    RemoteHandle* h = _handle_allocator.allocate_handle(hab);
    h->initialize(cast_from_oop<void*>(obj));

    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    table_lock();
    _table[idx] = alloc_entry(addr, h, true, _table[idx]);
    table_unlock();

    return h;
  }

  // Look up Handle for an object. Returns nullptr if not managed.
  RemoteHandle* handle_for(oop obj) const {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    // No lock needed for read (single-writer in Phase 1)
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_obj_addr == addr) return e->_handle;
      e = e->_next;
    }
    return nullptr;
  }

  // Update the mapping when an object is evacuated to a new address.
  // Called from do_copy_to_survivor_space() during STW.
  void update_handle_for_evacuation(oop old_obj, oop new_obj) {
    uintptr_t old_addr = cast_from_oop<uintptr_t>(old_obj);
    uintptr_t new_addr = cast_from_oop<uintptr_t>(new_obj);
    size_t old_idx = hash_obj(old_addr);

    table_lock();
    // Find and remove old entry
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
    // Not found — object was not managed (OK, not all old objects have Handles)
  }

  // Remove Handle mapping for a dead object (called during GC cleanup).
  void remove_handle_for(oop obj) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    table_lock();
    HandleEntry** pp = &_table[idx];
    while (*pp != nullptr) {
      if ((*pp)->_obj_addr == addr) {
        HandleEntry* entry = *pp;
        *pp = entry->_next;
        delete entry;
        table_unlock();
        return;
      }
      pp = &((*pp)->_next);
    }
    table_unlock();
  }

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
};

#endif // SHARE_GC_G1_G1REMOTEMEMORYMANAGER_HPP

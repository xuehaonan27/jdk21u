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

// ============================================================
// G1RemoteMemoryManager
// ============================================================

class G1RemoteMemoryManager : public CHeapObj<mtGC> {
  G1CollectedHeap* _g1h;

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
    RemoteHandle* _handle;     // value: Handle pointer
    HandleEntry*  _next;       // chaining

    HandleEntry(uintptr_t addr, RemoteHandle* h, HandleEntry* next)
      : _obj_addr(addr), _handle(h), _next(next) {}
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

  // ============================================================
  // Handle management
  // ============================================================

  // Create a Handle for an object and register it in the mapping.
  // The Handle is initialized to LOCAL with the object's current address.
  // Returns the allocated Handle.
  RemoteHandle* create_handle_for(oop obj, RemoteHandleAllocBuffer* hab) {
    RemoteHandle* h = _handle_allocator.allocate_handle(hab);
    h->initialize(cast_from_oop<void*>(obj));

    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    size_t idx = hash_obj(addr);

    table_lock();
    _table[idx] = new HandleEntry(addr, h, _table[idx]);
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
  // Accessors
  // ============================================================

  RemoteHandleAllocator* handle_allocator() { return &_handle_allocator; }

  // Check if an object is managed (has a Handle)
  bool is_managed(oop obj) const {
    return handle_for(obj) != nullptr;
  }
};

#endif // SHARE_GC_G1_G1REMOTEMEMORYMANAGER_HPP

/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 */

#include "precompiled.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/copy.hpp"

G1RemoteMemoryManager::G1RemoteMemoryManager(G1CollectedHeap* g1h)
  : _g1h(g1h), _handle_allocator(), _table_lock(0),
    _sim_remote_next_slot(0), _sim_remote_evicted_count(0),
    _sim_remote_fetched_count(0) {
  memset(_table, 0, sizeof(_table));
  memset(_sim_remote_slots, 0, sizeof(_sim_remote_slots));
}

G1RemoteMemoryManager::~G1RemoteMemoryManager() {
  // Free all HandleEntry objects in the table
  for (size_t i = 0; i < TABLE_SIZE; i++) {
    HandleEntry* e = _table[i];
    while (e != nullptr) {
      HandleEntry* next = e->_next;
      delete e;
      e = next;
    }
    _table[i] = nullptr;
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

  // 2. Copy object bytes to simulated remote
  size_t slot_id = sim_remote_evict(obj, word_size, klass);

  // 3. Set Handle to REMOTE with slot_id
  h->set_remote(slot_id);

  // 4. Set classification in per-region bitmap (NOT mark word — mark word bits
  //    cause CAS conflicts in synchronizer.cpp, see lessons learned).
  HeapRegion* hr = _g1h->heap_region_containing(obj);
  if (hr != nullptr) {
    hr->set_remote_class(cast_from_oop<HeapWord*>(obj), HeapRegion::REMOTE_CLASS_SHARED);
  }

  log_info(gc)("Remote evict: obj=" PTR_FORMAT " klass=%s size=" SIZE_FORMAT "w slot=" SIZE_FORMAT,
               p2i((void*)obj), klass->external_name(), word_size, slot_id);

  return true;
}

Klass* G1RemoteMemoryManager::fetch_remote_object(RemoteHandle* h, void* dest) {
  assert(h != nullptr, "Handle must not be null");

  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
  assert(state == REMOTE_HANDLE_REMOTE || state == REMOTE_HANDLE_FETCHING,
         "Handle must be REMOTE or FETCHING");

  size_t slot_id = sa & REMOTE_HANDLE_ADDR_MASK;
  assert(slot_id < SIM_REMOTE_MAX_SLOTS, "Invalid remote slot");

  size_t word_size = _sim_remote_slots[slot_id]._word_size;
  Klass* klass = sim_remote_fetch(slot_id, dest, word_size);

  log_info(gc)("Remote fetch: slot=" SIZE_FORMAT " -> dest=" PTR_FORMAT " klass=%s size=" SIZE_FORMAT "w",
               slot_id, p2i(dest), klass->external_name(), word_size);

  return klass;
}

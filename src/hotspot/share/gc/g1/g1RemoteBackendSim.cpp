/*
 * SimLocalBackend implementation — in-process simulated remote memory.
 */

#include "precompiled.hpp"
#include "gc/g1/g1RemoteBackendSim.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"

SimLocalBackend::SimLocalBackend()
  : _next_slot(0), _total_evicted(0), _total_fetched(0),
    _root_ids(nullptr), _num_roots(0) {
  memset(_slots, 0, sizeof(_slots));
}

SimLocalBackend::~SimLocalBackend() {
  shutdown();
}

bool SimLocalBackend::initialize() {
  log_info(gc)("Remote backend: sim-local (in-process, no network)");
  return true;
}

size_t SimLocalBackend::evict(const void* obj_bytes, size_t word_size,
                              Klass* klass, size_t hint_slot_id) {
  size_t slot = (hint_slot_id < MAX_SLOTS && !_slots[hint_slot_id].in_use)
                  ? hint_slot_id : _next_slot++;
  if (slot >= MAX_SLOTS) return (size_t)-1;

  size_t byte_size = word_size * HeapWordSize;
  _slots[slot].data = os::malloc(byte_size, mtGC);
  memcpy(_slots[slot].data, obj_bytes, byte_size);
  _slots[slot].word_size = word_size;
  _slots[slot].klass = klass;
  _slots[slot].in_use = true;
  _slots[slot].marked = false;
  _total_evicted++;
  return slot;
}

Klass* SimLocalBackend::fetch(size_t slot_id, void* dest, size_t* out_word_size) {
  if (slot_id >= MAX_SLOTS || !_slots[slot_id].in_use) return nullptr;
  size_t byte_size = _slots[slot_id].word_size * HeapWordSize;
  memcpy(dest, _slots[slot_id].data, byte_size);
  if (out_word_size) *out_word_size = _slots[slot_id].word_size;
  _total_fetched++;
  return _slots[slot_id].klass;
}

void SimLocalBackend::report_roots(const size_t* root_slot_ids, size_t num_roots) {
  if (_root_ids) os::free(_root_ids);
  _root_ids = (size_t*)os::malloc(num_roots * sizeof(size_t), mtGC);
  memcpy(_root_ids, root_slot_ids, num_roots * sizeof(size_t));
  _num_roots = num_roots;
}

void SimLocalBackend::collect_dead(size_t** out_dead_ids, size_t* out_num_dead,
                                   size_t* out_bytes_freed) {
  // Clear marks
  for (size_t i = 0; i < MAX_SLOTS; i++) _slots[i].marked = false;

  // Mark roots
  for (size_t i = 0; i < _num_roots; i++) {
    size_t sid = _root_ids[i];
    if (sid < MAX_SLOTS && _slots[sid].in_use) _slots[sid].marked = true;
  }

  // Collect dead
  size_t* dead = (size_t*)os::malloc(MAX_SLOTS * sizeof(size_t), mtGC);
  size_t num_dead = 0;
  size_t bytes_freed = 0;

  for (size_t i = 0; i < MAX_SLOTS; i++) {
    if (_slots[i].in_use && !_slots[i].marked) {
      bytes_freed += _slots[i].word_size * HeapWordSize;
      os::free(_slots[i].data);
      _slots[i].data = nullptr;
      _slots[i].in_use = false;
      dead[num_dead++] = i;
    }
  }

  *out_dead_ids = dead;
  *out_num_dead = num_dead;
  *out_bytes_freed = bytes_freed;
}

void SimLocalBackend::discard_slot(size_t slot_id) {
  if (slot_id < MAX_SLOTS && _slots[slot_id].in_use) {
    os::free(_slots[slot_id].data);
    _slots[slot_id].data = nullptr;
    _slots[slot_id].in_use = false;
  }
}

size_t SimLocalBackend::slot_word_size(size_t slot_id) const {
  if (slot_id >= MAX_SLOTS) return 0;
  return _slots[slot_id].word_size;
}

void SimLocalBackend::shutdown() {
  for (size_t i = 0; i < MAX_SLOTS; i++) {
    if (_slots[i].data) { os::free(_slots[i].data); _slots[i].data = nullptr; }
  }
  if (_root_ids) { os::free(_root_ids); _root_ids = nullptr; }
}

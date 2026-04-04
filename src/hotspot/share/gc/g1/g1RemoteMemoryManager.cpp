/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 */

#include "precompiled.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1NUMA.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/copy.hpp"

// TCP client for remote executor communication
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

G1RemoteMemoryManager::G1RemoteMemoryManager(G1CollectedHeap* g1h)
  : _g1h(g1h), _handle_allocator(), _table_lock(0),
    _sim_remote_next_slot(0), _sim_remote_evicted_count(0),
    _sim_remote_fetched_count(0),
    _current_fcr(nullptr), _fcr_lock(0),
    _executor_fd(-1), _executor_connected(false), _executor_seq_id(0) {
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
  size_t collected = 0;
  size_t retained = 0;
  size_t bytes_freed = 0;

  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    HandleEntry** pp = &_table[idx];
    while (*pp != nullptr) {
      HandleEntry* entry = *pp;

      // Only process entries with REMOTE handles (evicted objects)
      if (entry->_handle != nullptr && entry->_handle->is_remote()) {
        oop obj = cast_to_oop(entry->_obj_addr);

        // Check if the local copy of this object is still reachable.
        // The local object is at entry->_obj_addr (still valid — we don't
        // release local memory in the current prototype).
        bool is_alive = false;
        if (_g1h->is_in(obj)) {
          // Check marking bitmap: was this object marked during concurrent marking?
          HeapRegion* hr = _g1h->heap_region_containing(obj);
          if (hr != nullptr) {
            is_alive = cm->is_marked_in_bitmap(obj);
          }
        }

        if (!is_alive) {
          // DEAD remote object — free sim-remote slot without fetching.
          // This is "garbage never crosses the network."
          uintptr_t sa = entry->_handle->load_state_and_addr_acquire();
          size_t slot_id = sa & REMOTE_HANDLE_ADDR_MASK;
          if (slot_id < SIM_REMOTE_MAX_SLOTS && _sim_remote_slots[slot_id]._in_use) {
            bytes_freed += _sim_remote_slots[slot_id]._word_size * HeapWordSize;
            os::free(_sim_remote_slots[slot_id]._data);
            _sim_remote_slots[slot_id]._data = nullptr;
            _sim_remote_slots[slot_id]._in_use = false;
          }

          // Clear region bitmap classification
          if (_g1h->is_in(obj)) {
            HeapRegion* hr = _g1h->heap_region_containing(obj);
            if (hr != nullptr && hr->has_remote_class_map()) {
              hr->set_remote_class(cast_from_oop<HeapWord*>(obj), HeapRegion::REMOTE_CLASS_UNTRACKED);
            }
          }

          // Remove Handle entry from table
          *pp = entry->_next;
          os::free(entry);
          collected++;
          continue;
        } else {
          retained++;
        }
      }
      pp = &(*pp)->_next;
    }
  }
  table_unlock();

  if (collected > 0 || retained > 0) {
    log_info(gc)("Remote collection: " SIZE_FORMAT " dead objects freed (" SIZE_FORMAT " bytes reclaimed remotely), "
                 SIZE_FORMAT " live objects retained",
                 collected, bytes_freed, retained);
  }
  return collected;
}

HeapWord* G1RemoteMemoryManager::allocate_in_fcr(size_t word_size) {
  // Try allocating in the current FCR region (CAS-based, thread-safe)
  HeapRegion* fcr = _current_fcr;
  if (fcr != nullptr) {
    size_t actual = 0;
    HeapWord* result = fcr->par_allocate(word_size, word_size, &actual);
    if (result != nullptr) {
      return result;
    }
  }

  // Current FCR is full or doesn't exist. Try to get a new one.
  // This path requires locks (cannot be called from JRT_LEAF).
  // The caller must handle nullptr gracefully.
  fcr_lock();
  // Double-check: another thread might have allocated a new FCR
  if (_current_fcr != fcr) {
    fcr = _current_fcr;
    fcr_unlock();
    if (fcr != nullptr) {
      size_t actual = 0;
      HeapWord* result = fcr->par_allocate(word_size, word_size, &actual);
      if (result != nullptr) return result;
    }
    return nullptr;  // Still full — give up for this call
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
// ============================================================
// Protocol matches remote_executor/remote_protocol.h.
// Length-prefixed TCP framing: [4-byte length][message bytes].

// Message type constants (must match remote_protocol.h)
static const uint32_t RE_CMD_HELLO              = 0x01;
static const uint32_t RE_CMD_EVICT_OBJECT       = 0x03;
static const uint32_t RE_CMD_REPORT_ROOTS       = 0x04;
static const uint32_t RE_CMD_REQUEST_COLLECTION = 0x05;
static const uint32_t RE_CMD_FETCH_OBJECT       = 0x06;
static const uint32_t RE_RESP_OK                = 0x81;
static const uint32_t RE_RESP_OBJECT_DATA       = 0x83;
static const uint32_t RE_RESP_COLLECTION_RESULT = 0x84;

bool G1RemoteMemoryManager::executor_send_all(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::send(_executor_fd, p, remaining, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      log_warning(gc)("Executor send failed: %s", os::strerror(errno));
      return false;
    }
    p += n;
    remaining -= n;
  }
  return true;
}

bool G1RemoteMemoryManager::executor_recv_all(void* buf, size_t len) {
  uint8_t* p = (uint8_t*)buf;
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::recv(_executor_fd, p, remaining, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      log_warning(gc)("Executor recv failed: %s", os::strerror(errno));
      return false;
    }
    p += n;
    remaining -= n;
  }
  return true;
}

bool G1RemoteMemoryManager::executor_send_msg(const void* data, size_t len) {
  uint32_t net_len = (uint32_t)len;
  if (!executor_send_all(&net_len, 4)) return false;
  return executor_send_all(data, len);
}

bool G1RemoteMemoryManager::executor_recv_msg(void* buf, size_t max_len, size_t* actual_len) {
  uint32_t net_len = 0;
  if (!executor_recv_all(&net_len, 4)) return false;
  if (net_len > max_len) {
    log_warning(gc)("Executor message too large: %u > %zu", net_len, max_len);
    return false;
  }
  if (!executor_recv_all(buf, net_len)) return false;
  if (actual_len) *actual_len = net_len;
  return true;
}

bool G1RemoteMemoryManager::ensure_executor_connected() {
  if (_executor_connected) return true;

  const char* host = RemoteExecutorHost;
  int port = (int)RemoteExecutorPort;

  _executor_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (_executor_fd < 0) {
    log_warning(gc)("Executor: socket() failed: %s", os::strerror(errno));
    return false;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    log_warning(gc)("Executor: invalid host %s", host);
    ::close(_executor_fd); _executor_fd = -1;
    return false;
  }

  if (::connect(_executor_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    log_warning(gc)("Executor: connect to %s:%d failed: %s", host, port, os::strerror(errno));
    ::close(_executor_fd); _executor_fd = -1;
    return false;
  }

  int nodelay = 1;
  setsockopt(_executor_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  _executor_connected = true;
  log_info(gc)("Executor: connected to %s:%d", host, port);

  return executor_hello();
}

bool G1RemoteMemoryManager::executor_hello() {
  // 12 bytes header + 20 bytes payload = 32 bytes
  uint8_t msg[32];
  memset(msg, 0, sizeof(msg));
  uint32_t* hdr = (uint32_t*)msg;
  hdr[0] = RE_CMD_HELLO;  // type
  hdr[1] = 32;            // length
  uint64_t* payload = (uint64_t*)(msg + 12);
  payload[0] = 1;  // protocol version (stored in the 4 bytes after seq_id, but simplified)
  // For prototype: heap_base and heap_size are informational only

  if (!executor_send_msg(msg, 32)) return false;

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!executor_recv_msg(resp, sizeof(resp), &resp_len)) return false;

  uint32_t resp_type = *(uint32_t*)resp;
  if (resp_type != RE_RESP_OK) {
    log_warning(gc)("Executor hello failed: response type 0x%x", resp_type);
    return false;
  }
  log_info(gc)("Executor: hello OK");
  return true;
}

// Dispatch: use executor or sim-remote based on UseRemoteExecutor flag
size_t G1RemoteMemoryManager::remote_evict(oop obj, size_t word_size, Klass* klass) {
  if (UseRemoteExecutor) {
    return executor_evict(obj, word_size, klass);
  }
  return sim_remote_evict(obj, word_size, klass);
}

Klass* G1RemoteMemoryManager::remote_fetch(size_t slot_id, void* dest, size_t* out_word_size) {
  if (UseRemoteExecutor) {
    return executor_fetch(slot_id, dest, out_word_size);
  }
  size_t ws = _sim_remote_slots[slot_id]._word_size;
  if (out_word_size) *out_word_size = ws;
  return sim_remote_fetch(slot_id, dest, ws);
}

size_t G1RemoteMemoryManager::remote_collect_dead() {
  if (UseRemoteExecutor) {
    return executor_collect_dead();
  }
  return collect_dead_remote_objects();
}

size_t G1RemoteMemoryManager::executor_evict(oop obj, size_t word_size, Klass* klass) {
  if (!ensure_executor_connected()) return (size_t)-1;

  size_t byte_size = word_size * HeapWordSize;
  size_t slot_id = _sim_remote_next_slot++;  // reuse slot_id counter

  // Build CMD_EVICT_OBJECT: header(12) + slot_id(8) + klass(8) + word_size(4) + bytes
  size_t msg_size = 12 + 8 + 8 + 4 + byte_size;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  uint32_t* hdr = (uint32_t*)msg;
  hdr[0] = RE_CMD_EVICT_OBJECT;
  hdr[1] = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _executor_seq_id++;  // seq_id at offset 8 (overlapping with hdr[2])
  *(uint64_t*)(msg + 12) = slot_id;
  *(uint64_t*)(msg + 20) = (uint64_t)klass;
  *(uint32_t*)(msg + 28) = (uint32_t)word_size;
  memcpy(msg + 32, cast_from_oop<void*>(obj), byte_size);

  bool ok = executor_send_msg(msg, msg_size);
  os::free(msg);

  if (!ok) return (size_t)-1;

  // Wait for OK response
  uint8_t resp[64];
  size_t resp_len = 0;
  if (!executor_recv_msg(resp, sizeof(resp), &resp_len)) return (size_t)-1;

  _sim_remote_evicted_count++;
  return slot_id;
}

Klass* G1RemoteMemoryManager::executor_fetch(size_t slot_id, void* dest, size_t* out_word_size) {
  if (!ensure_executor_connected()) return nullptr;

  // CMD_FETCH_OBJECT: header(12) + slot_id(8)
  uint8_t msg[20];
  uint32_t* hdr = (uint32_t*)msg;
  hdr[0] = RE_CMD_FETCH_OBJECT;
  hdr[1] = 20;
  *(uint64_t*)(msg + 8) = _executor_seq_id++;
  *(uint64_t*)(msg + 12) = slot_id;

  if (!executor_send_msg(msg, 20)) return nullptr;

  // Receive RESP_OBJECT_DATA
  uint8_t* resp = (uint8_t*)os::malloc(64 * 1024, mtGC);
  size_t resp_len = 0;
  if (!executor_recv_msg(resp, 64 * 1024, &resp_len)) {
    os::free(resp);
    return nullptr;
  }

  uint32_t resp_type = *(uint32_t*)resp;
  if (resp_type != RE_RESP_OBJECT_DATA) {
    os::free(resp);
    return nullptr;
  }

  // Parse: header(12) + slot_id(8) + klass(8) + word_size(4) + bytes
  uint64_t resp_klass = *(uint64_t*)(resp + 20);
  uint32_t resp_ws = *(uint32_t*)(resp + 28);
  size_t byte_size = resp_ws * HeapWordSize;
  memcpy(dest, resp + 32, byte_size);

  if (out_word_size) *out_word_size = resp_ws;
  os::free(resp);
  _sim_remote_fetched_count++;
  return (Klass*)resp_klass;
}

size_t G1RemoteMemoryManager::executor_collect_dead() {
  if (!ensure_executor_connected()) return 0;

  // Step 1: Determine root set — remote objects still referenced from local heap.
  // Walk Handle table, collect slot_ids of REMOTE handles whose local copy is alive.
  G1ConcurrentMark* cm = _g1h->concurrent_mark();
  size_t root_capacity = 1024;
  uint64_t* root_ids = (uint64_t*)os::malloc(root_capacity * sizeof(uint64_t), mtGC);
  uint32_t num_roots = 0;

  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      if (e->_handle != nullptr && e->_handle->is_remote()) {
        oop obj = cast_to_oop(e->_obj_addr);
        if (_g1h->is_in(obj) && cm->is_marked_in_bitmap(obj)) {
          // Object is alive locally → its remote copy is a "root"
          uintptr_t sa = e->_handle->load_state_and_addr_acquire();
          uint64_t slot_id = sa & REMOTE_HANDLE_ADDR_MASK;
          if (num_roots >= root_capacity) {
            root_capacity *= 2;
            uint64_t* new_ids = (uint64_t*)os::malloc(root_capacity * sizeof(uint64_t), mtGC);
            memcpy(new_ids, root_ids, num_roots * sizeof(uint64_t));
            os::free(root_ids);
            root_ids = new_ids;
          }
          root_ids[num_roots++] = slot_id;
        }
      }
      e = e->_next;
    }
  }
  table_unlock();

  // Step 2: CMD_REPORT_ROOTS
  size_t roots_msg_size = 12 + 4 + num_roots * 8;  // header + num_roots + slot_ids
  uint8_t* roots_msg = (uint8_t*)os::malloc(roots_msg_size, mtGC);
  uint32_t* rhdr = (uint32_t*)roots_msg;
  rhdr[0] = RE_CMD_REPORT_ROOTS;
  rhdr[1] = (uint32_t)roots_msg_size;
  *(uint64_t*)(roots_msg + 8) = _executor_seq_id++;
  *(uint32_t*)(roots_msg + 12) = num_roots;
  memcpy(roots_msg + 16, root_ids, num_roots * 8);

  executor_send_msg(roots_msg, roots_msg_size);
  os::free(roots_msg);
  os::free(root_ids);

  // Wait for OK
  uint8_t ok_resp[64];
  size_t ok_len = 0;
  executor_recv_msg(ok_resp, sizeof(ok_resp), &ok_len);

  // Step 3: CMD_REQUEST_COLLECTION
  uint8_t collect_msg[12];
  uint32_t* chdr = (uint32_t*)collect_msg;
  chdr[0] = RE_CMD_REQUEST_COLLECTION;
  chdr[1] = 12;
  *(uint64_t*)(collect_msg + 4) = _executor_seq_id++;  // fits in remaining 8 bytes
  executor_send_msg(collect_msg, 12);

  // Step 4: Receive RESP_COLLECTION_RESULT
  uint8_t* result = (uint8_t*)os::malloc(64 * 1024, mtGC);
  size_t result_len = 0;
  executor_recv_msg(result, 64 * 1024, &result_len);

  uint32_t result_type = *(uint32_t*)result;
  if (result_type != RE_RESP_COLLECTION_RESULT) {
    os::free(result);
    return 0;
  }

  // Parse: header(12) + num_dead(4) + num_live(4) + bytes_freed(8) + dead_slot_ids[]
  uint32_t num_dead = *(uint32_t*)(result + 12);
  uint32_t num_live = *(uint32_t*)(result + 16);
  uint64_t bytes_freed = *(uint64_t*)(result + 20);
  uint64_t* dead_ids = (uint64_t*)(result + 28);

  // Step 5: Clean up local Handle entries for dead remote objects
  table_lock();
  for (uint32_t i = 0; i < num_dead; i++) {
    uint64_t dead_slot = dead_ids[i];
    // Find and remove Handle entry with this slot_id
    for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
      HandleEntry** pp = &_table[idx];
      while (*pp != nullptr) {
        HandleEntry* e = *pp;
        if (e->_handle != nullptr && e->_handle->is_remote()) {
          uintptr_t sa = e->_handle->load_state_and_addr_acquire();
          if ((sa & REMOTE_HANDLE_ADDR_MASK) == dead_slot) {
            // Clear region bitmap
            if (_g1h->is_in(cast_to_oop(e->_obj_addr))) {
              HeapRegion* hr = _g1h->heap_region_containing(cast_to_oop(e->_obj_addr));
              if (hr != nullptr && hr->has_remote_class_map()) {
                hr->set_remote_class((HeapWord*)e->_obj_addr, HeapRegion::REMOTE_CLASS_UNTRACKED);
              }
            }
            *pp = e->_next;
            os::free(e);
            goto next_dead;
          }
        }
        pp = &(*pp)->_next;
      }
    }
    next_dead:;
  }
  table_unlock();

  log_info(gc)("Executor collection: %u dead (freed %lu bytes remotely), %u live retained",
               num_dead, bytes_freed, num_live);

  os::free(result);
  return num_dead;
}

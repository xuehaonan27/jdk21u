/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 *
 * Handle table for G1 disaggregated memory support.
 *
 * Each RemoteHandle is 16 bytes, tracking the location (local or remote)
 * of a managed Old object. Handles are allocated lazily — only for Shared
 * objects (RC>1) or during Unique-to-Shared upgrade.
 *
 * Phase 1 simplification: ALL remote-participating objects are Shared
 * (always get a Handle). The Unique optimization comes in Phase 2.
 *
 * Fetch deduplication: the state_and_addr field uses bits 63:62 for a
 * state machine (LOCAL / REMOTE / FETCHING) with release/acquire semantics.
 */

#ifndef SHARE_GC_G1_G1REMOTEHANDLE_HPP
#define SHARE_GC_G1_G1REMOTEHANDLE_HPP

#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"

// ============================================================
// RemoteHandle: 16-byte metadata for a managed object
// ============================================================

// Fetch state encoding in state_and_addr bits 63:62
const int      REMOTE_HANDLE_STATE_SHIFT    = 62;
const uintptr_t REMOTE_HANDLE_STATE_MASK    = uintptr_t(3) << REMOTE_HANDLE_STATE_SHIFT;
const uintptr_t REMOTE_HANDLE_ADDR_MASK     = (uintptr_t(1) << 48) - 1;

const uintptr_t REMOTE_HANDLE_LOCAL         = uintptr_t(0) << REMOTE_HANDLE_STATE_SHIFT;
const uintptr_t REMOTE_HANDLE_REMOTE        = uintptr_t(1) << REMOTE_HANDLE_STATE_SHIFT;
const uintptr_t REMOTE_HANDLE_FETCHING      = uintptr_t(2) << REMOTE_HANDLE_STATE_SHIFT;

struct RemoteHandle {
  volatile uintptr_t _state_and_addr;  // [63:62]=state, [47:0]=addr or remote_id
  uintptr_t          _reserved;        // Reserved for future use (back_ref, etc.)

  // State queries (non-atomic, for use under lock or single-threaded)
  uintptr_t state() const { return _state_and_addr & REMOTE_HANDLE_STATE_MASK; }
  uintptr_t addr()  const { return _state_and_addr & REMOTE_HANDLE_ADDR_MASK; }

  bool is_local()    const { return state() == REMOTE_HANDLE_LOCAL; }
  bool is_remote()   const { return state() == REMOTE_HANDLE_REMOTE; }
  bool is_fetching() const { return state() == REMOTE_HANDLE_FETCHING; }

  // Atomic state queries (for concurrent access)
  uintptr_t load_state_and_addr_acquire() const {
    return Atomic::load_acquire(&_state_and_addr);
  }

  // Get local address (only valid when state == LOCAL)
  void* local_addr() const {
    assert(is_local(), "must be local");
    return (void*)addr();
  }

  // Get remote id (valid when state == REMOTE or FETCHING)
  uintptr_t remote_id() const {
    assert(!is_local(), "must be remote or fetching");
    return addr();
  }

  // Atomic state transitions for fetch deduplication
  // Returns true if CAS succeeded (this thread wins the fetch race)
  bool cas_remote_to_fetching() {
    uintptr_t expected = _state_and_addr;  // must be in REMOTE state
    assert((expected & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_REMOTE, "must be REMOTE");
    uintptr_t desired = (expected & REMOTE_HANDLE_ADDR_MASK) | REMOTE_HANDLE_FETCHING;
    return Atomic::cmpxchg(&_state_and_addr, expected, desired) == expected;
  }

  // Release-store: publish local address after RDMA completion.
  // This is the publication point — threads doing acquire-load will see
  // all bytes written to FTLAB before this store.
  void set_local_release(void* local_addr) {
    uintptr_t val = REMOTE_HANDLE_LOCAL | (uintptr_t(local_addr) & REMOTE_HANDLE_ADDR_MASK);
    Atomic::release_store(&_state_and_addr, val);
  }

  // Set to remote (used during eviction)
  void set_remote(uintptr_t remote_id) {
    _state_and_addr = REMOTE_HANDLE_REMOTE | (remote_id & REMOTE_HANDLE_ADDR_MASK);
  }

  // Set to local (used during Handle creation for locally-present objects)
  void set_local(void* obj_addr) {
    _state_and_addr = REMOTE_HANDLE_LOCAL | (uintptr_t(obj_addr) & REMOTE_HANDLE_ADDR_MASK);
  }

  // Store/retrieve eviction metadata in _reserved field.
  // word_size is set at eviction time so the fetch path can allocate
  // the correct FCR buffer size without querying the (possibly remote) backend.
  void set_eviction_word_size(size_t ws) { _reserved = (uintptr_t)ws; }
  size_t eviction_word_size() const      { return (size_t)_reserved; }

  // Initialize a fresh Handle
  void initialize(void* obj_addr) {
    set_local(obj_addr);
    _reserved = 0;
  }
};


// ============================================================
// RemoteHandleChunk: fixed-size chunk of Handles
// ============================================================

const size_t REMOTE_HANDLE_CHUNK_CAPACITY = 256;  // 256 * 16B = 4KB per chunk

struct RemoteHandleChunk : public CHeapObj<mtGC> {
  RemoteHandle    _handles[REMOTE_HANDLE_CHUNK_CAPACITY];
  RemoteHandleChunk* _next;  // Linked list for global pool

  RemoteHandleChunk() : _next(nullptr) {}
};


// ============================================================
// RemoteHandleAllocBuffer: per-thread bump-pointer allocator
// ============================================================
// Follows the PLAB pattern: fast bump-pointer within a chunk,
// one CAS per chunk refill from the global pool.

class RemoteHandleAllocBuffer : public CHeapObj<mtGC> {
  RemoteHandle* _top;
  RemoteHandle* _end;
  RemoteHandleChunk* _current_chunk;

public:
  RemoteHandleAllocBuffer() : _top(nullptr), _end(nullptr), _current_chunk(nullptr) {}

  // Fast-path allocation (bump-pointer, no lock)
  RemoteHandle* allocate() {
    if (_top < _end) {
      RemoteHandle* result = _top;
      _top++;
      return result;
    }
    return nullptr;  // Chunk exhausted; caller should refill
  }

  // Install a new chunk for this buffer
  void set_chunk(RemoteHandleChunk* chunk) {
    _current_chunk = chunk;
    _top = &chunk->_handles[0];
    _end = &chunk->_handles[REMOTE_HANDLE_CHUNK_CAPACITY];
  }

  bool is_empty() const { return _top == nullptr || _top >= _end; }

  RemoteHandleChunk* current_chunk() const { return _current_chunk; }
};


// ============================================================
// RemoteHandleAllocator: global Handle allocator
// ============================================================
// Manages a pool of RemoteHandleChunks. Threads request chunks
// for their local RemoteHandleAllocBuffer.

class RemoteHandleAllocator : public CHeapObj<mtGC> {
  RemoteHandleChunk* _free_chunks;   // Free chunk list (for reuse)
  RemoteHandleChunk* _all_chunks;    // All allocated chunks (for cleanup)
  size_t _total_chunks;
  size_t _total_handles_allocated;

  // Simple lock for chunk allocation (low contention: one CAS per 256 handles)
  volatile int _lock;

  void lock()   { while (Atomic::cmpxchg(&_lock, 0, 1) != 0) { /* spin */ } }
  void unlock() { Atomic::release_store(&_lock, 0); }

public:
  RemoteHandleAllocator()
    : _free_chunks(nullptr), _all_chunks(nullptr),
      _total_chunks(0), _total_handles_allocated(0), _lock(0) {}

  ~RemoteHandleAllocator() {
    // Free all chunks
    RemoteHandleChunk* chunk = _all_chunks;
    while (chunk != nullptr) {
      RemoteHandleChunk* next = chunk->_next;
      delete chunk;
      chunk = next;
    }
  }

  // Get a chunk for a thread's HAB. Creates a new chunk if no free ones.
  RemoteHandleChunk* get_chunk() {
    lock();
    RemoteHandleChunk* chunk = _free_chunks;
    if (chunk != nullptr) {
      _free_chunks = chunk->_next;
    } else {
      chunk = new RemoteHandleChunk();
      chunk->_next = _all_chunks;
      _all_chunks = chunk;
      _total_chunks++;
    }
    unlock();
    return chunk;
  }

  // Allocate a Handle (convenience: uses a temporary buffer)
  // For hot paths, callers should use RemoteHandleAllocBuffer directly.
  RemoteHandle* allocate_handle(RemoteHandleAllocBuffer* hab) {
    RemoteHandle* h = hab->allocate();
    if (h == nullptr) {
      // Refill from global pool
      RemoteHandleChunk* chunk = get_chunk();
      hab->set_chunk(chunk);
      h = hab->allocate();
      assert(h != nullptr, "fresh chunk should have space");
    }
    _total_handles_allocated++;
    return h;
  }

  size_t total_chunks() const { return _total_chunks; }
  size_t total_handles_allocated() const { return _total_handles_allocated; }
};


// ============================================================
// RemoteObjectMetadata: tracks size/klass for remote objects
// ============================================================
// When an object is evicted to remote, its size and Klass pointer
// (both local metadata) must be recorded because the Klass pointer
// is in the object header which is now remote.

struct RemoteObjectMetadata {
  size_t  _word_size;   // Object size in HeapWords
  Klass*  _klass;       // Klass pointer (always local, in metaspace)
};

#endif // SHARE_GC_G1_G1REMOTEHANDLE_HPP

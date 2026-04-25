/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 *
 * Handle table for G1 disaggregated memory support.
 *
 * Each RemoteHandle is 16 bytes, tracking the location (local or remote)
 * of a managed object. Handles serve three roles:
 *
 * 1. Evicted objects: Handle tracks LOCAL/REMOTE/FETCHING state for load barrier.
 *    Incoming local refs use shared_oop(handle) for barrier detection.
 *
 * 2. Dormant anchors: Handle tracks current address of a local object that is
 *    referenced by a remote object's fields. Local oop fields stay CLEAN (no
 *    shared_oop). The Handle provides stable identity for relocation tracking,
 *    GC liveness rooting, and fetch-time field patching via sidecar edge tables.
 *
 * 3. Fetched-back objects: Handle transitions REMOTE → LOCAL. Subject to
 *    de-handleification ladder (shared_oop → unique_oop → clean oop) when hot.
 *
 * Handles are decoupled from classification (UNIQUE/SHARED mark word bits).
 * An object can have a Handle without being classified, and vice versa.
 *
 * Fetch deduplication: the state_and_addr field uses bits 63:62 for a
 * state machine (LOCAL / REMOTE / FETCHING / DEAD) with release/acquire semantics.
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
const uintptr_t REMOTE_HANDLE_DEAD          = uintptr_t(3) << REMOTE_HANDLE_STATE_SHIFT;

// Handle flags (stored in _flags field)
const uint32_t REMOTE_HANDLE_FLAG_DORMANT   = 0x1;  // Dormant anchor (local obj referenced by remote)

struct RemoteHandle {
  volatile uintptr_t _state_and_addr;  // [63:62]=state, [47:0]=addr or remote_loc
  uint32_t           _remote_refcount; // Count of remote oop fields pointing to this Handle
  uint32_t           _flags;           // REMOTE_HANDLE_FLAG_* bits

  // State queries (non-atomic, for use under lock or single-threaded)
  uintptr_t state() const { return _state_and_addr & REMOTE_HANDLE_STATE_MASK; }
  uintptr_t addr()  const { return _state_and_addr & REMOTE_HANDLE_ADDR_MASK; }

  bool is_local()    const { return state() == REMOTE_HANDLE_LOCAL; }
  bool is_remote()   const { return state() == REMOTE_HANDLE_REMOTE; }
  bool is_fetching() const { return state() == REMOTE_HANDLE_FETCHING; }
  bool is_dead()     const { return state() == REMOTE_HANDLE_DEAD; }
  bool is_dormant()  const { return (_flags & REMOTE_HANDLE_FLAG_DORMANT) != 0; }

  // Atomic state queries (for concurrent access)
  uintptr_t load_state_and_addr_acquire() const {
    return Atomic::load_acquire(&_state_and_addr);
  }

  // Get local address (only valid when state == LOCAL)
  void* local_addr() const {
    assert(is_local(), "must be local");
    return (void*)addr();
  }

  // Get remote id/location (valid when state == REMOTE or FETCHING)
  uintptr_t remote_id() const {
    assert(!is_local() && !is_dead(), "must be remote or fetching");
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

  // Set to remote (used during eviction).
  // Release store: ensures prior writes (e.g. set_eviction_word_size)
  // are visible to readers who see REMOTE via load_state_and_addr_acquire.
  void set_remote(uintptr_t remote_id) {
    Atomic::release_store(&_state_and_addr,
      (uintptr_t)(REMOTE_HANDLE_REMOTE | (remote_id & REMOTE_HANDLE_ADDR_MASK)));
  }

  // Set to local (used during Handle creation and GC evacuation)
  void set_local(void* obj_addr) {
    _state_and_addr = REMOTE_HANDLE_LOCAL | (uintptr_t(obj_addr) & REMOTE_HANDLE_ADDR_MASK);
  }

  // Set to dead (used when referent object dies)
  void set_dead() {
    _state_and_addr = REMOTE_HANDLE_DEAD;
  }

  // Remote refcount: tracks how many oop fields in remote objects reference this Handle.
  // Used to determine when de-handleification is safe (refcount == 0 → no remote refs).
  void increment_remote_refcount() { _remote_refcount++; }
  void decrement_remote_refcount() {
    assert(_remote_refcount > 0, "underflow");
    _remote_refcount--;
  }
  uint32_t remote_refcount() const { return _remote_refcount; }

  // Flags
  void set_dormant()   { _flags |= REMOTE_HANDLE_FLAG_DORMANT; }
  void clear_dormant() { _flags &= ~REMOTE_HANDLE_FLAG_DORMANT; }

  // Store/retrieve eviction metadata.
  // word_size is needed at fetch time for FCR allocation (without querying backend).
  // Packed into _flags upper 16 bits (max 64K words = 512KB object, sufficient).
  void set_eviction_word_size(size_t ws) {
    assert(ws <= 0xFFFF, "object too large for packed word_size");
    _flags = (_flags & 0xFFFF) | ((uint32_t)ws << 16);
  }
  size_t eviction_word_size() const {
    return (size_t)(_flags >> 16);
  }

  // Initialize a fresh Handle
  void initialize(void* obj_addr) {
    set_local(obj_addr);
    _remote_refcount = 0;
    _flags = 0;
  }

  // Initialize as dormant anchor (local object referenced by remote)
  void initialize_dormant(void* obj_addr) {
    set_local(obj_addr);
    _remote_refcount = 0;
    _flags = REMOTE_HANDLE_FLAG_DORMANT;
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
  volatile size_t _total_handles_allocated;

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
    Atomic::add(&_total_handles_allocated, (size_t)1);
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

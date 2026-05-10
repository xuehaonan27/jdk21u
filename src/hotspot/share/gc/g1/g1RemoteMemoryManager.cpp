/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 */

#include "precompiled.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1RemoteBackend.hpp"
#include "gc/g1/g1RemoteBackendSim.hpp"
#include "gc/g1/g1RemoteBackendTcp.hpp"
#include "gc/g1/g1RemoteBackendRdma.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.inline.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1NUMA.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "logging/log.hpp"
#include "memory/metaspace.hpp"
#include "oops/arrayOop.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/timer.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/threads.hpp"
#include "gc/shared/oopStorageSet.inline.hpp"
#include "utilities/spinYield.hpp"
#include "utilities/copy.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "gc/shared/workerThread.hpp"
#include "gc/g1/heapRegionManager.inline.hpp"
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "gc/shared/referenceProcessor.hpp"

static bool remote_handle_managed_local_oop(G1CollectedHeap* g1h, oop obj);

// TCP client for remote executor communication
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

G1RemoteMemoryManager::G1RemoteMemoryManager(G1CollectedHeap* g1h)
  : _g1h(g1h), _backend(nullptr), _handle_allocator(),
    _entry_chunks(nullptr), _entry_free_list(nullptr), _entry_chunk_top(ENTRY_CHUNK_CAPACITY),
    _table_lock(0), _local_handles_head(nullptr), _local_handle_count(0),
    _local_handle_region_counts(nullptr), _local_handle_region_heads(nullptr),
    _local_handle_region_capacity(0),
    _local_handle_lock(0), _array_chunk_segment_lock(0), _alloc_lock(0),
    _molecule_klass_profile(nullptr),
    _molecule_edge_profile(nullptr),
    _molecule_profile_capacity(0),
    _molecule_profile_lock(0),
    _molecule_profile_old_copies(0),
    _molecule_profile_old_copy_bytes(0),
    _molecule_profile_promotion_edges(0),
    _molecule_profile_array_edges(0),
    _molecule_profile_mutation_probes(0),
    _molecule_profile_mutation_overwrites(0),
    _molecule_profile_dropped_klass(0),
    _molecule_profile_dropped_edges(0),
    _sim_remote_next_slot(0), _sim_remote_evicted_count(0),
    _sim_remote_fetched_count(0), _gc_epoch(0),
    _eviction_backoff_until_epoch(nullptr), _eviction_backoff_capacity(0),
    _fast_phase_c_source_hints(nullptr),
    _fast_phase_c_source_hint_capacity(0),
    _fast_phase_c_source_hint_count(0),
    _fast_phase_c_source_hint_lock(0),
    _old_cset_source_hints(nullptr),
    _old_cset_source_hint_capacity(0),
    _old_cset_source_hint_count(0),
    _old_cset_source_hint_lock(0),
    _inbound_region_summary(nullptr),
    _inbound_region_summary_capacity(0),
    _inbound_region_summary_lock(0),
    _inbound_region_summary_edges(0),
    _inbound_region_summary_duplicates(0),
    _last_phase_c_tagged(0),
    _last_phase_c_no_handle(0),
    _last_phase_c_untaggable(0),
    _dense_segments(nullptr),
    _dense_segment_capacity(0),
    _dense_segment_lock(0),
    _dense_segment_next_id(1),
    _dense_segment_evict_success(0),
    _dense_segment_evict_failures(0),
    _dense_segment_fetch_success(0),
    _dense_segment_fetch_failures(0),
    _dense_segment_remote_bytes(0),
    _remote_roots(nullptr), _remote_roots_count(0), _cm_remote_roots_count(0),
    _remote_roots_capacity(0),
    _cross_roots_count(0),
    _remote_collection_has_trace(false), _remote_collection_skipped(0),
    _remote_collection_last_handles_allocated(0),
    _deferred_decrement_count(0),
    _tagged_fields(nullptr), _tagged_field_count(0), _tagged_field_capacity(0),
    _tagged_field_lock(0),
    _fcr_evac_writes(0), _fcr_fixup_nulls(0),
    _resolve_fast_local(0), _resolve_fast_remote(0), _resolve_fast_fetching(0),
    _resolve_fast_dead(0), _resolve_slow_entries(0), _resolve_no_safepoint_entries(0),
    _fetch_success(0), _fetch_failures(0), _fetch_words(0), _fetch_elapsed_counter(0),
    _fetch_retries(0), _fetch_wait_slow(0), _fetch_wait_no_safepoint(0),
    _fetch_wait_hard(0), _fetch_wait_loops(0), _fetch_progress_next(1024),
    _fetch_batch_requests(0), _fetch_batch_returned(0), _fetch_batch_installed(0),
    _fetch_prefetch_installed(0), _fetch_prefetch_raced(0), _fetch_prefetch_failed(0),
    _fetch_prefetch_words(0), _fetch_batch_elapsed_counter(0),
    _current_fcr(nullptr), _fcr_lock(0) {
  _table = NEW_C_HEAP_ARRAY(HandleEntry*, TABLE_SIZE, mtGC);
  _eviction_table = NEW_C_HEAP_ARRAY(HandleEntry*, TABLE_SIZE, mtGC);
  memset(_table, 0, TABLE_SIZE * sizeof(HandleEntry*));
  memset(_eviction_table, 0, TABLE_SIZE * sizeof(HandleEntry*));
  memset((void*)_stripe_locks, 0, sizeof(_stripe_locks));
  memset(_array_chunk_segments, 0, sizeof(_array_chunk_segments));
  memset(_edge_buckets, 0, sizeof(_edge_buckets));
  memset((void*)_edge_bucket_locks, 0, sizeof(_edge_bucket_locks));
  memset(_hotness_stats, 0, sizeof(_hotness_stats));
  memset(_prev_hotness_stats, 0, sizeof(_prev_hotness_stats));
  memset(_sim_remote_slots, 0, sizeof(_sim_remote_slots));
  if (G1RemoteMoleculeProfile && G1RemoteMoleculeProfileTableSize > 0) {
    _molecule_profile_capacity = G1RemoteMoleculeProfileTableSize;
    _molecule_klass_profile =
        NEW_C_HEAP_ARRAY(MoleculeKlassProfileEntry, _molecule_profile_capacity, mtGC);
    _molecule_edge_profile =
        NEW_C_HEAP_ARRAY(MoleculeEdgeProfileEntry, _molecule_profile_capacity, mtGC);
    memset(_molecule_klass_profile, 0,
           _molecule_profile_capacity * sizeof(MoleculeKlassProfileEntry));
    memset(_molecule_edge_profile, 0,
           _molecule_profile_capacity * sizeof(MoleculeEdgeProfileEntry));
  }

  // Create remote storage backend. A build can include the RDMA/TCP backend,
  // but normal local G1 runs must stay in-process unless the runtime flag
  // explicitly asks to connect to an executor.
  if (UseRemoteExecutor) {
#if defined(REMOTE_BACKEND_RDMA)
    _backend = new RDMAExecutorBackend();
#elif defined(REMOTE_BACKEND_TCP)
    _backend = new TCPExecutorBackend();
#elif defined(REMOTE_BACKEND_SIM)
    _backend = new SimLocalBackend();
#else
#ifdef REMOTE_EXECUTOR_USE_RDMA
    _backend = new RDMAExecutorBackend();
#else
    _backend = new TCPExecutorBackend();
#endif
#endif
  } else {
    _backend = new SimLocalBackend();
  }

  // Backend object created; connection deferred to initialize_backend()
  // (called from G1CollectedHeap::initialize() when heap info is available).
}

bool G1RemoteMemoryManager::ensure_local_handle_region_counts_locked() {
  if (_local_handle_region_counts != nullptr) {
    return true;
  }
  uint capacity = _g1h == nullptr ? 0 : _g1h->max_reserved_regions();
  if (capacity == 0) {
    return false;
  }

  size_t* counts = NEW_C_HEAP_ARRAY(size_t, capacity, mtGC);
  RemoteHandle** heads = NEW_C_HEAP_ARRAY(RemoteHandle*, capacity, mtGC);
  memset(counts, 0, capacity * sizeof(size_t));
  memset(heads, 0, capacity * sizeof(RemoteHandle*));
  _local_handle_region_counts = counts;
  _local_handle_region_heads = heads;
  _local_handle_region_capacity = capacity;

  for (RemoteHandle* cur = _local_handles_head;
       cur != nullptr;
       cur = cur->_local_next) {
    uintptr_t sa = cur->load_state_and_addr_acquire();
    if ((sa & REMOTE_HANDLE_STATE_MASK) != REMOTE_HANDLE_LOCAL) {
      continue;
    }
    uint idx = local_handle_region_index(sa & REMOTE_HANDLE_ADDR_MASK);
    if (idx < _local_handle_region_capacity) {
      cur->_region_prev = nullptr;
      cur->_region_next = _local_handle_region_heads[idx];
      if (_local_handle_region_heads[idx] != nullptr) {
        _local_handle_region_heads[idx]->_region_prev = cur;
      }
      _local_handle_region_heads[idx] = cur;
      cur->_local_region_index = idx;
      _local_handle_region_counts[idx]++;
    }
  }
  return true;
}

uint G1RemoteMemoryManager::local_handle_region_index(uintptr_t addr) const {
  if (addr == 0 || _g1h == nullptr || !_g1h->is_in_reserved((void*)addr)) {
    return UINT_MAX;
  }
  HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)addr);
  if (hr == nullptr) {
    return UINT_MAX;
  }
  return hr->hrm_index();
}

void G1RemoteMemoryManager::link_local_handle_region_locked(RemoteHandle* h,
                                                            uintptr_t addr) {
  if (h == nullptr) {
    return;
  }
  if (!ensure_local_handle_region_counts_locked()) {
    return;
  }
  uint idx = local_handle_region_index(addr);
  if (idx < _local_handle_region_capacity) {
    h->_region_prev = nullptr;
    h->_region_next = _local_handle_region_heads[idx];
    if (_local_handle_region_heads[idx] != nullptr) {
      _local_handle_region_heads[idx]->_region_prev = h;
    }
    _local_handle_region_heads[idx] = h;
    h->_local_region_index = idx;
    Atomic::add(&_local_handle_region_counts[idx], (size_t)1);
  }
}

void G1RemoteMemoryManager::unlink_local_handle_region_locked(RemoteHandle* h) {
  if (h == nullptr || _local_handle_region_counts == nullptr ||
      _local_handle_region_heads == nullptr) {
    return;
  }
  uint idx = h->_local_region_index;
  if (idx >= _local_handle_region_capacity) {
    return;
  }
  if (h->_region_prev != nullptr) {
    h->_region_prev->_region_next = h->_region_next;
  } else if (_local_handle_region_heads[idx] == h) {
    _local_handle_region_heads[idx] = h->_region_next;
  }
  if (h->_region_next != nullptr) {
    h->_region_next->_region_prev = h->_region_prev;
  }
  h->_region_prev = nullptr;
  h->_region_next = nullptr;
  h->_local_region_index = UINT_MAX;

  size_t cur = Atomic::load(&_local_handle_region_counts[idx]);
  while (cur > 0) {
    size_t next = cur - 1;
    size_t observed = Atomic::cmpxchg(&_local_handle_region_counts[idx], cur, next);
    if (observed == cur) {
      return;
    }
    cur = observed;
  }
}

void G1RemoteMemoryManager::move_local_handle_region(RemoteHandle* h,
                                                     uintptr_t old_addr,
                                                     uintptr_t new_addr) {
  if (h == nullptr || !h->_local_listed || old_addr == new_addr) {
    return;
  }
  local_handle_lock();
  if (!ensure_local_handle_region_counts_locked()) {
    local_handle_unlock();
    return;
  }
  uint old_idx = local_handle_region_index(old_addr);
  uint new_idx = local_handle_region_index(new_addr);
  if (old_idx == new_idx) {
    local_handle_unlock();
    return;
  }
  unlink_local_handle_region_locked(h);
  link_local_handle_region_locked(h, new_addr);
  local_handle_unlock();
}

void G1RemoteMemoryManager::link_local_handle_locked(RemoteHandle* h,
                                                     uintptr_t local_addr) {
  if (h == nullptr || h->_local_listed) {
    return;
  }
  uintptr_t addr = local_addr;
  if (addr == 0) {
    uintptr_t sa = h->load_state_and_addr_acquire();
    if ((sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL) {
      addr = sa & REMOTE_HANDLE_ADDR_MASK;
    }
  }
  link_local_handle_region_locked(h, addr);
  h->_local_prev = nullptr;
  h->_local_next = _local_handles_head;
  if (_local_handles_head != nullptr) {
    _local_handles_head->_local_prev = h;
  }
  _local_handles_head = h;
  h->_local_listed = true;
  _local_handle_count++;
}

void G1RemoteMemoryManager::unlink_local_handle_locked(RemoteHandle* h,
                                                       uintptr_t local_addr) {
  (void)local_addr;
  if (h == nullptr || !h->_local_listed) {
    return;
  }
  unlink_local_handle_region_locked(h);
  if (h->_local_prev != nullptr) {
    h->_local_prev->_local_next = h->_local_next;
  } else {
    _local_handles_head = h->_local_next;
  }
  if (h->_local_next != nullptr) {
    h->_local_next->_local_prev = h->_local_prev;
  }
  h->_local_prev = nullptr;
  h->_local_next = nullptr;
  h->_local_listed = false;
  _local_handle_count--;
}

void G1RemoteMemoryManager::link_local_handle(RemoteHandle* h) {
  local_handle_lock();
  link_local_handle_locked(h);
  local_handle_unlock();
}

void G1RemoteMemoryManager::unlink_local_handle(RemoteHandle* h) {
  local_handle_lock();
  unlink_local_handle_locked(h);
  local_handle_unlock();
}

void G1RemoteMemoryManager::append_pending_local_handle(RemoteHandle* h,
                                                        RemoteHandle** head,
                                                        RemoteHandle** tail,
                                                        size_t* count) {
  if (h == nullptr || head == nullptr || tail == nullptr || count == nullptr) {
    return;
  }
  assert(!h->_local_listed, "fresh handle must not already be listed");
  h->_local_prev = *tail;
  h->_local_next = nullptr;
  h->_local_listed = true;
  if (*tail != nullptr) {
    (*tail)->_local_next = h;
  } else {
    *head = h;
  }
  *tail = h;
  (*count)++;
}

void G1RemoteMemoryManager::link_local_handle_batch(RemoteHandle* head,
                                                    RemoteHandle* tail,
                                                    size_t count) {
  if (head == nullptr || tail == nullptr || count == 0) {
    return;
  }

  local_handle_lock();
  ensure_local_handle_region_counts_locked();
  for (RemoteHandle* cur = head; cur != nullptr; cur = cur->_local_next) {
    uintptr_t sa = cur->load_state_and_addr_acquire();
    if ((sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL) {
      link_local_handle_region_locked(cur, sa & REMOTE_HANDLE_ADDR_MASK);
    }
    if (cur == tail) {
      break;
    }
  }
  head->_local_prev = nullptr;
  tail->_local_next = _local_handles_head;
  if (_local_handles_head != nullptr) {
    _local_handles_head->_local_prev = tail;
  }
  _local_handles_head = head;
  _local_handle_count += count;
  local_handle_unlock();
}

void G1RemoteMemoryManager::publish_local_handle(RemoteHandle* h, void* local_addr) {
  if (h == nullptr) {
    return;
  }
  local_handle_lock();
  link_local_handle_locked(h, (uintptr_t)local_addr);
  h->set_local_release(local_addr);
  local_handle_unlock();
}

void G1RemoteMemoryManager::publish_local_handles(RemoteHandle** handles,
                                                  HeapWord** local_addrs,
                                                  uint count) {
  if (handles == nullptr || local_addrs == nullptr || count == 0) {
    return;
  }

  local_handle_lock();
  for (uint i = 0; i < count; i++) {
    RemoteHandle* h = handles[i];
    HeapWord* local_addr = local_addrs[i];
    if (h == nullptr || local_addr == nullptr) {
      continue;
    }
    link_local_handle_locked(h, (uintptr_t)local_addr);
    h->set_local_release(local_addr);
  }
  local_handle_unlock();
}

void G1RemoteMemoryManager::make_handle_remote(RemoteHandle* h, uintptr_t remote_id) {
  if (h == nullptr) {
    return;
  }
  local_handle_lock();
  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t old_addr = ((sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL)
      ? (sa & REMOTE_HANDLE_ADDR_MASK) : 0;
  h->set_remote(remote_id);
  unlink_local_handle_locked(h, old_addr);
  local_handle_unlock();
}

void G1RemoteMemoryManager::make_handle_remote_array_chunk(RemoteHandle* h,
                                                           uintptr_t segment_base,
                                                           uintptr_t segment_id,
                                                           size_t offset,
                                                           size_t byte_size,
                                                           size_t segment_byte_size,
                                                           uint32_t flags) {
  if (h == nullptr) {
    return;
  }
  local_handle_lock();
  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t old_addr = ((sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL)
      ? (sa & REMOTE_HANDLE_ADDR_MASK) : 0;
  h->set_remote_array_chunk(segment_base, segment_id, offset, byte_size,
                            segment_byte_size, flags);
  unlink_local_handle_locked(h, old_addr);
  local_handle_unlock();
}

static size_t array_chunk_segment_hash(uint64_t segment_id) {
  static const size_t ArrayChunkSegmentBuckets = 4096;
  uint64_t x = segment_id;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)x & (ArrayChunkSegmentBuckets - 1);
}

void G1RemoteMemoryManager::register_array_chunk_segment(uint64_t segment_id,
                                                         RemoteHandle** handles,
                                                         uint32_t refcount,
                                                         size_t byte_size) {
  if (segment_id == 0 || refcount == 0 || byte_size == 0) {
    if (handles != nullptr) {
      os::free(handles);
    }
    return;
  }

  size_t idx = array_chunk_segment_hash(segment_id);
  array_chunk_segment_lock();
  for (ArrayChunkSegmentEntry* e = _array_chunk_segments[idx];
       e != nullptr;
       e = e->_next) {
    if (e->_segment_id == segment_id) {
      Atomic::add(&e->_refcount, refcount);
      e->_byte_size = byte_size;
      if (e->_handles == nullptr && handles != nullptr) {
        e->_handles = handles;
        e->_handle_count = refcount;
      } else if (handles != nullptr) {
        os::free(handles);
      }
      array_chunk_segment_unlock();
      return;
    }
  }

  ArrayChunkSegmentEntry* e =
      (ArrayChunkSegmentEntry*)os::malloc(sizeof(ArrayChunkSegmentEntry), mtGC);
  if (e == nullptr) {
    array_chunk_segment_unlock();
    if (handles != nullptr) {
      os::free(handles);
    }
    log_warning(gc)("Array chunk segment registry allocation failed: segment="
                    UINT64_FORMAT " refs=%u bytes=" SIZE_FORMAT,
                    segment_id, refcount, byte_size);
    return;
  }
  e->_segment_id = segment_id;
  e->_refcount = refcount;
  e->_byte_size = byte_size;
  e->_handles = handles;
  e->_handle_count = handles == nullptr ? 0 : refcount;
  e->_next = _array_chunk_segments[idx];
  _array_chunk_segments[idx] = e;
  array_chunk_segment_unlock();
}

uint G1RemoteMemoryManager::copy_array_chunk_segment_handles(uint64_t segment_id,
                                                             RemoteHandle** out,
                                                             uint max_handles,
                                                             size_t* byte_size_out,
                                                             uint* total_handles_out) {
  if (byte_size_out != nullptr) {
    *byte_size_out = 0;
  }
  if (total_handles_out != nullptr) {
    *total_handles_out = 0;
  }
  if (segment_id == 0 || out == nullptr || max_handles == 0) {
    return 0;
  }

  uint copied = 0;
  size_t idx = array_chunk_segment_hash(segment_id);
  array_chunk_segment_lock();
  for (ArrayChunkSegmentEntry* e = _array_chunk_segments[idx];
       e != nullptr;
       e = e->_next) {
    if (e->_segment_id == segment_id) {
      if (byte_size_out != nullptr) {
        *byte_size_out = e->_byte_size;
      }
      if (total_handles_out != nullptr) {
        *total_handles_out = e->_handle_count;
      }
      uint limit = MIN2(e->_handle_count, max_handles);
      for (uint i = 0; i < limit; i++) {
        out[copied++] = e->_handles == nullptr ? nullptr : e->_handles[i];
      }
      break;
    }
  }
  array_chunk_segment_unlock();
  return copied;
}

void G1RemoteMemoryManager::release_array_chunk_segment(uint64_t segment_id) {
  if (segment_id == 0 || _backend == nullptr) {
    return;
  }

  bool discard = false;
  bool found = false;
  size_t idx = array_chunk_segment_hash(segment_id);
  array_chunk_segment_lock();
  ArrayChunkSegmentEntry* prev = nullptr;
  ArrayChunkSegmentEntry* cur = _array_chunk_segments[idx];
  while (cur != nullptr) {
    if (cur->_segment_id == segment_id) {
      found = true;
      uint32_t refs = Atomic::load(&cur->_refcount);
      if (refs <= 1) {
        if (prev == nullptr) {
          _array_chunk_segments[idx] = cur->_next;
        } else {
          prev->_next = cur->_next;
        }
        discard = true;
        if (cur->_handles != nullptr) {
          os::free(cur->_handles);
          cur->_handles = nullptr;
          cur->_handle_count = 0;
        }
        os::free(cur);
      } else {
        Atomic::release_store(&cur->_refcount, refs - 1);
      }
      break;
    }
    prev = cur;
    cur = cur->_next;
  }
  array_chunk_segment_unlock();

  if (discard || !found) {
    _backend->discard_segment(segment_id);
  }
}

void G1RemoteMemoryManager::mark_handle_dead(RemoteHandle* h) {
  if (h == nullptr) {
    return;
  }
  uintptr_t initial_sa = h->load_state_and_addr_acquire();
  uintptr_t initial_state = initial_sa & REMOTE_HANDLE_STATE_MASK;
  if (h->_remote_location.is_array_chunk() &&
      _backend != nullptr &&
      h->_remote_location._secondary_id != 0 &&
      (initial_state == REMOTE_HANDLE_REMOTE ||
       initial_state == REMOTE_HANDLE_FETCHING)) {
    release_array_chunk_segment((uint64_t)h->_remote_location._secondary_id);
  }
  local_handle_lock();
  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t old_addr = ((sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL)
      ? (sa & REMOTE_HANDLE_ADDR_MASK) : 0;
  h->set_dead();
  unlink_local_handle_locked(h, old_addr);
  local_handle_unlock();
}

bool G1RemoteMemoryManager::concurrent_marking_active() const {
  return _g1h->collector_state()->mark_or_rebuild_in_progress();
}

void G1RemoteMemoryManager::initialize_backend() {
  for (int attempt = 1; attempt <= 5; attempt++) {
    if (_backend->initialize()) {
      log_info(gc)("Remote memory backend: %s", _backend->name());
      return;
    }
    log_warning(gc)("Remote backend (%s) initialization attempt %d/5 failed, retrying in 2s...",
                    _backend->name(), attempt);
    _backend->shutdown();
    os::naked_sleep(2000);
  }
  log_warning(gc)("Remote backend (%s) initialization failed after 5 attempts, falling back to sim-local",
                  _backend->name());
  delete _backend;
  _backend = new SimLocalBackend();
  _backend->initialize();
  log_info(gc)("Remote memory backend: %s", _backend->name());
}

void G1RemoteMemoryManager::record_fetch_result(size_t word_size,
                                                jlong elapsed_counter,
                                                bool success) {
  if (!success) {
    Atomic::inc(&_fetch_failures);
    return;
  }

  Atomic::inc(&_fetch_success);
  Atomic::add(&_fetch_words, (uint64_t)word_size);
  Atomic::add(&_fetch_elapsed_counter, (uint64_t)elapsed_counter);

  const uint64_t progress_interval = 64 * 1024;
  uint64_t fetch_success = Atomic::load(&_fetch_success);
  uint64_t next = Atomic::load(&_fetch_progress_next);
  while (fetch_success >= next) {
    uint64_t new_next = next + progress_interval;
    if (Atomic::cmpxchg(&_fetch_progress_next, next, new_next) == next) {
      log_remote_access_stats();
      break;
    }
    next = Atomic::load(&_fetch_progress_next);
  }
}

void G1RemoteMemoryManager::record_fetch_batch_result(size_t requested,
                                                      size_t returned,
                                                      size_t installed,
                                                      size_t prefetched,
                                                      size_t raced,
                                                      size_t failed,
                                                      size_t prefetch_words,
                                                      jlong elapsed_counter) {
  Atomic::add(&_fetch_batch_requests, (uint64_t)requested);
  Atomic::add(&_fetch_batch_returned, (uint64_t)returned);
  Atomic::add(&_fetch_batch_installed, (uint64_t)installed);
  Atomic::add(&_fetch_prefetch_installed, (uint64_t)prefetched);
  Atomic::add(&_fetch_prefetch_raced, (uint64_t)raced);
  Atomic::add(&_fetch_prefetch_failed, (uint64_t)failed);
  Atomic::add(&_fetch_prefetch_words, (uint64_t)prefetch_words);
  Atomic::add(&_fetch_batch_elapsed_counter, (uint64_t)elapsed_counter);
}

void G1RemoteMemoryManager::log_remote_access_stats() const {
  uint64_t resolve_fast_local = Atomic::load(&_resolve_fast_local);
  uint64_t resolve_fast_remote = Atomic::load(&_resolve_fast_remote);
  uint64_t resolve_fast_fetching = Atomic::load(&_resolve_fast_fetching);
  uint64_t resolve_fast_dead = Atomic::load(&_resolve_fast_dead);
  uint64_t resolve_slow_entries = Atomic::load(&_resolve_slow_entries);
  uint64_t resolve_no_safepoint_entries = Atomic::load(&_resolve_no_safepoint_entries);
  uint64_t fetch_success = Atomic::load(&_fetch_success);
  uint64_t fetch_failures = Atomic::load(&_fetch_failures);
  uint64_t fetch_retries = Atomic::load(&_fetch_retries);
  uint64_t fetch_wait_slow = Atomic::load(&_fetch_wait_slow);
  uint64_t fetch_wait_no_safepoint = Atomic::load(&_fetch_wait_no_safepoint);
  uint64_t fetch_wait_hard = Atomic::load(&_fetch_wait_hard);
  uint64_t fetch_wait_loops = Atomic::load(&_fetch_wait_loops);
  uint64_t fetch_batch_requests = Atomic::load(&_fetch_batch_requests);
  uint64_t fetch_batch_returned = Atomic::load(&_fetch_batch_returned);
  uint64_t fetch_batch_installed = Atomic::load(&_fetch_batch_installed);
  uint64_t fetch_prefetch_installed = Atomic::load(&_fetch_prefetch_installed);
  uint64_t fetch_prefetch_raced = Atomic::load(&_fetch_prefetch_raced);
  uint64_t fetch_prefetch_failed = Atomic::load(&_fetch_prefetch_failed);
  uint64_t dense_evict_success = Atomic::load(&_dense_segment_evict_success);
  uint64_t dense_evict_failures = Atomic::load(&_dense_segment_evict_failures);
  uint64_t dense_fetch_success = Atomic::load(&_dense_segment_fetch_success);
  uint64_t dense_fetch_failures = Atomic::load(&_dense_segment_fetch_failures);
  uint64_t dense_remote_bytes = Atomic::load(&_dense_segment_remote_bytes);

  if (resolve_fast_local == 0 && resolve_fast_remote == 0 &&
      resolve_fast_fetching == 0 && resolve_fast_dead == 0 &&
      resolve_slow_entries == 0 && resolve_no_safepoint_entries == 0 &&
      fetch_success == 0 && fetch_failures == 0 && fetch_retries == 0 &&
      fetch_wait_slow == 0 && fetch_wait_no_safepoint == 0 &&
      fetch_wait_hard == 0 && fetch_wait_loops == 0 &&
      fetch_batch_requests == 0 && fetch_batch_returned == 0 &&
      fetch_batch_installed == 0 && fetch_prefetch_installed == 0 &&
      fetch_prefetch_raced == 0 && fetch_prefetch_failed == 0 &&
      dense_evict_success == 0 && dense_evict_failures == 0 &&
      dense_fetch_success == 0 && dense_fetch_failures == 0) {
    return;
  }

  uint64_t fetch_words = Atomic::load(&_fetch_words);
  uint64_t fetch_counter = Atomic::load(&_fetch_elapsed_counter);
  uint64_t fetch_prefetch_words = Atomic::load(&_fetch_prefetch_words);
  uint64_t fetch_batch_counter = Atomic::load(&_fetch_batch_elapsed_counter);
  double fetch_ms = TimeHelper::counter_to_millis((jlong)fetch_counter);
  double batch_ms = TimeHelper::counter_to_millis((jlong)fetch_batch_counter);
  double avg_us = fetch_success == 0 ? 0.0 : (fetch_ms * 1000.0) / (double)fetch_success;

  log_info(gc)("Remote access stats: resolve_fast(local=" UINT64_FORMAT
               " remote=" UINT64_FORMAT " fetching=" UINT64_FORMAT
               " dead=" UINT64_FORMAT ") slow=" UINT64_FORMAT
               " no_safepoint=" UINT64_FORMAT " fetch(ok=" UINT64_FORMAT
               " fail=" UINT64_FORMAT " retry=" UINT64_FORMAT
               " bytes=" UINT64_FORMAT " avg_us=%.1f total_ms=%.1f)"
               " batch(req=" UINT64_FORMAT " ret=" UINT64_FORMAT
               " inst=" UINT64_FORMAT " pref=" UINT64_FORMAT
               " raced=" UINT64_FORMAT " fail=" UINT64_FORMAT
               " pref_bytes=" UINT64_FORMAT " total_ms=%.1f)"
               " dense_segment(evict_ok=" UINT64_FORMAT
               " evict_fail=" UINT64_FORMAT " fetch_ok=" UINT64_FORMAT
               " fetch_fail=" UINT64_FORMAT " remote_bytes=" UINT64_FORMAT ")"
               " waits(slow=" UINT64_FORMAT " no_safepoint=" UINT64_FORMAT
               " hard=" UINT64_FORMAT " loops=" UINT64_FORMAT ")",
               resolve_fast_local,
               resolve_fast_remote,
               resolve_fast_fetching,
               resolve_fast_dead,
               resolve_slow_entries,
               resolve_no_safepoint_entries,
               fetch_success,
               fetch_failures,
               fetch_retries,
               fetch_words * HeapWordSize,
               avg_us,
               fetch_ms,
               fetch_batch_requests,
               fetch_batch_returned,
               fetch_batch_installed,
               fetch_prefetch_installed,
               fetch_prefetch_raced,
               fetch_prefetch_failed,
               fetch_prefetch_words * HeapWordSize,
               batch_ms,
               dense_evict_success,
               dense_evict_failures,
               dense_fetch_success,
               dense_fetch_failures,
               dense_remote_bytes,
               fetch_wait_slow,
               fetch_wait_no_safepoint,
               fetch_wait_hard,
               fetch_wait_loops);
}

size_t G1RemoteMemoryManager::rebuild_handle_table_from_handles() {
  size_t inserted = 0;
  size_t local = 0;
  size_t remote = 0;
  size_t fetching = 0;
  size_t skipped = 0;

  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      HandleEntry* next = e->_next;
      e->_next = _entry_free_list;
      _entry_free_list = e;
      e = next;
    }
    _table[idx] = nullptr;
  }

  class RebuildClosure {
    G1RemoteMemoryManager* _rmm;
    size_t* _inserted;
    size_t* _local;
    size_t* _remote;
    size_t* _fetching;
    size_t* _skipped;

  public:
    RebuildClosure(G1RemoteMemoryManager* rmm,
                   size_t* inserted,
                   size_t* local,
                   size_t* remote,
                   size_t* fetching,
                   size_t* skipped)
      : _rmm(rmm), _inserted(inserted), _local(local),
        _remote(remote), _fetching(fetching), _skipped(skipped) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      uintptr_t key = 0;

      if (state == REMOTE_HANDLE_LOCAL) {
        key = sa & REMOTE_HANDLE_ADDR_MASK;
        (*_local)++;
      } else if (state == REMOTE_HANDLE_REMOTE) {
        key = h->_eviction_addr;
        (*_remote)++;
      } else if (state == REMOTE_HANDLE_FETCHING) {
        key = h->_eviction_addr;
        (*_fetching)++;
      } else {
        return;
      }

      if (key == 0) {
        (*_skipped)++;
        return;
      }

      size_t idx = hash_obj(key);
      HandleEntry* entry = _rmm->alloc_entry();
      entry->init(key, h, _rmm->_table[idx]);
      _rmm->_table[idx] = entry;
      (*_inserted)++;
    }
  };

  RebuildClosure cl(this, &inserted, &local, &remote, &fetching, &skipped);
  _handle_allocator.handles_do(&cl);
  table_unlock();

  log_info(gc)("Handle table rebuild: inserted=%zu local=%zu remote=%zu "
               "fetching=%zu skipped=%zu allocated=%zu",
               inserted, local, remote, fetching, skipped,
               _handle_allocator.total_handles_allocated());
  return inserted;
}

G1RemoteMemoryManager::~G1RemoteMemoryManager() {
  // Free HandleEntry chunks (entries are pool-managed, not individually freed)
  HandleEntryChunk* ec = _entry_chunks;
  while (ec != nullptr) {
    HandleEntryChunk* next = ec->_next;
    delete ec;
    ec = next;
  }
  FREE_C_HEAP_ARRAY(HandleEntry*, _table);
  _table = nullptr;
  FREE_C_HEAP_ARRAY(HandleEntry*, _eviction_table);
  _eviction_table = nullptr;
  if (_local_handle_region_counts != nullptr) {
    FREE_C_HEAP_ARRAY(size_t, _local_handle_region_counts);
    _local_handle_region_counts = nullptr;
  }
  if (_local_handle_region_heads != nullptr) {
    FREE_C_HEAP_ARRAY(RemoteHandle*, _local_handle_region_heads);
    _local_handle_region_heads = nullptr;
  }
  _local_handle_region_capacity = 0;

  for (size_t i = 0; i < ARRAY_CHUNK_SEGMENT_BUCKETS; i++) {
    ArrayChunkSegmentEntry* e = _array_chunk_segments[i];
    while (e != nullptr) {
      ArrayChunkSegmentEntry* next = e->_next;
      if (e->_handles != nullptr) {
        os::free(e->_handles);
        e->_handles = nullptr;
      }
      os::free(e);
      e = next;
    }
    _array_chunk_segments[i] = nullptr;
  }

  if (_eviction_backoff_until_epoch != nullptr) {
    FREE_C_HEAP_ARRAY(uint32_t, _eviction_backoff_until_epoch);
    _eviction_backoff_until_epoch = nullptr;
    _eviction_backoff_capacity = 0;
  }
  if (_fast_phase_c_source_hints != nullptr) {
    FREE_C_HEAP_ARRAY(bool, _fast_phase_c_source_hints);
    _fast_phase_c_source_hints = nullptr;
    _fast_phase_c_source_hint_capacity = 0;
    _fast_phase_c_source_hint_count = 0;
  }
  if (_old_cset_source_hints != nullptr) {
    FREE_C_HEAP_ARRAY(bool, _old_cset_source_hints);
    _old_cset_source_hints = nullptr;
    _old_cset_source_hint_capacity = 0;
    _old_cset_source_hint_count = 0;
  }
  if (_inbound_region_summary != nullptr) {
    FREE_C_HEAP_ARRAY(uint64_t, _inbound_region_summary);
    _inbound_region_summary = nullptr;
    _inbound_region_summary_capacity = 0;
  }
  if (_dense_segments != nullptr) {
    for (uint i = 0; i < _dense_segment_capacity; i++) {
      if (_dense_segments[i].edges != nullptr) {
        FREE_C_HEAP_ARRAY(DenseSegmentEdge, _dense_segments[i].edges);
        _dense_segments[i].edges = nullptr;
        _dense_segments[i].edge_count = 0;
      }
    }
    FREE_C_HEAP_ARRAY(DenseSegmentEntry, _dense_segments);
    _dense_segments = nullptr;
    _dense_segment_capacity = 0;
  }
  if (_molecule_klass_profile != nullptr) {
    FREE_C_HEAP_ARRAY(MoleculeKlassProfileEntry, _molecule_klass_profile);
    _molecule_klass_profile = nullptr;
  }
  if (_molecule_edge_profile != nullptr) {
    FREE_C_HEAP_ARRAY(MoleculeEdgeProfileEntry, _molecule_edge_profile);
    _molecule_edge_profile = nullptr;
  }
  _molecule_profile_capacity = 0;

  // Free edge tables (chained hash)
  for (size_t i = 0; i < EDGE_TABLE_BUCKETS; i++) {
    EdgeTableEntry* e = _edge_buckets[i];
    while (e != nullptr) {
      EdgeTableEntry* next = e->_next;
      ObjectEdgeTable::free(e->_table);
      os::free(e);
      e = next;
    }
    _edge_buckets[i] = nullptr;
  }

  // Free simulated remote slot data
  for (size_t i = 0; i < SIM_REMOTE_MAX_SLOTS; i++) {
    if (_sim_remote_slots[i]._data != nullptr) {
      os::free(_sim_remote_slots[i]._data);
      _sim_remote_slots[i]._data = nullptr;
    }
  }
}

void G1RemoteMemoryManager::ensure_eviction_backoff_capacity(uint num_regions) {
  if (num_regions <= _eviction_backoff_capacity) {
    return;
  }
  uint new_cap = MAX2(num_regions, _eviction_backoff_capacity * 2);
  if (new_cap < 1024) {
    new_cap = 1024;
  }
  uint32_t* new_backoff = NEW_C_HEAP_ARRAY(uint32_t, new_cap, mtGC);
  memset(new_backoff, 0, new_cap * sizeof(uint32_t));
  if (_eviction_backoff_until_epoch != nullptr) {
    memcpy(new_backoff, _eviction_backoff_until_epoch,
           _eviction_backoff_capacity * sizeof(uint32_t));
    FREE_C_HEAP_ARRAY(uint32_t, _eviction_backoff_until_epoch);
  }
  _eviction_backoff_until_epoch = new_backoff;
  _eviction_backoff_capacity = new_cap;
}

void G1RemoteMemoryManager::backoff_eviction_region(uint region_idx, uint gc_cycles) {
  if (gc_cycles == 0) {
    return;
  }
  ensure_eviction_backoff_capacity(region_idx + 1);
  uint32_t until = _gc_epoch + gc_cycles;
  if (_eviction_backoff_until_epoch[region_idx] < until) {
    _eviction_backoff_until_epoch[region_idx] = until;
  }
}

static uint dense_fetch_backoff_cycles_for_shift(uint16_t shift) {
  uint base = G1RemoteEvictionAbortBackoffGCCycles;
  uint max_cycles = G1RemoteDenseFetchBackoffMaxGCCycles;
  if (base == 0 || max_cycles == 0) {
    return 0;
  }

  uint cycles = MIN2(base, max_cycles);
  for (uint16_t i = 0; i < shift && cycles < max_cycles; i++) {
    if (cycles > max_cycles / 2) {
      cycles = max_cycles;
      break;
    }
    cycles *= 2;
  }
  return MIN2(cycles, max_cycles);
}

void G1RemoteMemoryManager::ensure_fast_phase_c_source_hint_capacity(uint num_regions) {
  if (num_regions <= _fast_phase_c_source_hint_capacity) {
    return;
  }
  uint new_cap = MAX2(num_regions, _fast_phase_c_source_hint_capacity * 2);
  if (new_cap < 1024) {
    new_cap = 1024;
  }
  bool* new_hints = NEW_C_HEAP_ARRAY(bool, new_cap, mtGC);
  memset(new_hints, 0, new_cap * sizeof(bool));
  if (_fast_phase_c_source_hints != nullptr) {
    memcpy(new_hints, _fast_phase_c_source_hints,
           _fast_phase_c_source_hint_capacity * sizeof(bool));
    FREE_C_HEAP_ARRAY(bool, _fast_phase_c_source_hints);
  }
  _fast_phase_c_source_hints = new_hints;
  _fast_phase_c_source_hint_capacity = new_cap;
}

bool G1RemoteMemoryManager::is_fast_phase_c_source_hint(uint region_idx) const {
  return G1RemoteUseFastPhaseCSourceHints &&
         region_idx < _fast_phase_c_source_hint_capacity &&
         _fast_phase_c_source_hints[region_idx];
}

bool G1RemoteMemoryManager::remember_fast_phase_c_source_hint(uint region_idx) {
  if (!G1RemoteUseFastPhaseCSourceHints ||
      G1RemoteFastPhaseCSourceHintMaxRegions == 0 ||
      region_idx == (uint)-1) {
    return false;
  }

  fast_phase_c_source_hint_lock();
  ensure_fast_phase_c_source_hint_capacity(region_idx + 1);
  if (_fast_phase_c_source_hints[region_idx]) {
    fast_phase_c_source_hint_unlock();
    return true;
  }
  if (_fast_phase_c_source_hint_count >= G1RemoteFastPhaseCSourceHintMaxRegions) {
    fast_phase_c_source_hint_unlock();
    return false;
  }
  _fast_phase_c_source_hints[region_idx] = true;
  _fast_phase_c_source_hint_count++;
  log_info(gc)("Fast Phase C source hint: learned clean old source region %u (%u/%u)",
               region_idx, _fast_phase_c_source_hint_count,
               G1RemoteFastPhaseCSourceHintMaxRegions);
  fast_phase_c_source_hint_unlock();
  return true;
}

void G1RemoteMemoryManager::ensure_old_cset_source_hint_capacity(uint num_regions) {
  if (num_regions <= _old_cset_source_hint_capacity) {
    return;
  }
  uint new_cap = MAX2(num_regions, _old_cset_source_hint_capacity * 2);
  if (new_cap < 1024) {
    new_cap = 1024;
  }
  bool* new_hints = NEW_C_HEAP_ARRAY(bool, new_cap, mtGC);
  memset(new_hints, 0, new_cap * sizeof(bool));
  if (_old_cset_source_hints != nullptr) {
    memcpy(new_hints, _old_cset_source_hints,
           _old_cset_source_hint_capacity * sizeof(bool));
    FREE_C_HEAP_ARRAY(bool, _old_cset_source_hints);
  }
  _old_cset_source_hints = new_hints;
  _old_cset_source_hint_capacity = new_cap;
}

bool G1RemoteMemoryManager::is_old_cset_source_hint(uint region_idx) const {
  return region_idx < _old_cset_source_hint_capacity &&
         _old_cset_source_hints[region_idx];
}

bool G1RemoteMemoryManager::remember_old_cset_source_hint(uint region_idx,
                                                          const char* reason) {
  if (region_idx == (uint)-1) {
    return false;
  }

  old_cset_source_hint_lock();
  ensure_old_cset_source_hint_capacity(region_idx + 1);
  if (_old_cset_source_hints[region_idx]) {
    old_cset_source_hint_unlock();
    return true;
  }
  _old_cset_source_hints[region_idx] = true;
  _old_cset_source_hint_count++;
  log_info(gc)("Old/cset source hint: learned region %u reason=%s (%u total)",
               region_idx, reason != nullptr ? reason : "unknown",
               _old_cset_source_hint_count);
  old_cset_source_hint_unlock();
  return true;
}

static inline uint inbound_summary_words_per_row(uint capacity) {
  return (capacity + 63) >> 6;
}

void G1RemoteMemoryManager::ensure_inbound_region_summary_capacity(uint num_regions) {
  if (num_regions <= _inbound_region_summary_capacity) {
    return;
  }

  uint new_cap = _g1h == nullptr ? 0 : _g1h->max_reserved_regions();
  if (new_cap < num_regions) {
    new_cap = MAX2(num_regions, _inbound_region_summary_capacity * 2);
  }
  if (new_cap < 1024) {
    new_cap = 1024;
  }

  const uint old_cap = _inbound_region_summary_capacity;
  const uint old_words = inbound_summary_words_per_row(old_cap);
  const uint new_words = inbound_summary_words_per_row(new_cap);
  const size_t new_size = (size_t)new_cap * (size_t)new_words;

  uint64_t* new_summary = NEW_C_HEAP_ARRAY(uint64_t, new_size, mtGC);
  memset(new_summary, 0, new_size * sizeof(uint64_t));

  if (_inbound_region_summary != nullptr) {
    for (uint target = 0; target < old_cap; target++) {
      memcpy(&new_summary[(size_t)target * new_words],
             &_inbound_region_summary[(size_t)target * old_words],
             old_words * sizeof(uint64_t));
    }
    FREE_C_HEAP_ARRAY(uint64_t, _inbound_region_summary);
  }

  _inbound_region_summary = new_summary;
  _inbound_region_summary_capacity = new_cap;
}

void G1RemoteMemoryManager::record_inbound_region_ref(uint source_region, uint target_region) {
  if (!G1RemoteUseInboundRegionSummary ||
      !G1RemoteUseFastPhaseC ||
      source_region == target_region ||
      source_region == (uint)-1 ||
      target_region == (uint)-1) {
    return;
  }

  uint capacity = _inbound_region_summary_capacity;
  uint64_t* summary = _inbound_region_summary;
  if (summary == nullptr ||
      source_region >= capacity ||
      target_region >= capacity) {
    inbound_region_summary_lock();
    ensure_inbound_region_summary_capacity(MAX2(source_region, target_region) + 1);
    capacity = _inbound_region_summary_capacity;
    summary = _inbound_region_summary;
    inbound_region_summary_unlock();
  }

  if (summary == nullptr ||
      source_region >= capacity ||
      target_region >= capacity) {
    return;
  }

  const uint words = inbound_summary_words_per_row(capacity);
  const uint word = source_region >> 6;
  const uint64_t mask = (uint64_t)1 << (source_region & 63);
  uint64_t* entry = &summary[(size_t)target_region * words + word];
  uint64_t cur = Atomic::load(entry);
  while ((cur & mask) == 0) {
    uint64_t observed = Atomic::cmpxchg(entry, cur, cur | mask);
    if (observed == cur) {
      Atomic::inc(&_inbound_region_summary_edges);
      return;
    }
    cur = observed;
  }
  Atomic::inc(&_inbound_region_summary_duplicates);
}

void G1RemoteMemoryManager::record_inbound_ref(void* field_addr, oop target) {
  if (!G1RemoteUseInboundRegionSummary ||
      !G1RemoteUseFastPhaseC ||
      field_addr == nullptr ||
      target == nullptr ||
      !_g1h->is_in(target)) {
    return;
  }

  HeapRegion* source_hr = _g1h->heap_region_containing_or_null(field_addr);
  HeapRegion* target_hr = _g1h->heap_region_containing_or_null((void*)target);
  if (source_hr == nullptr ||
      target_hr == nullptr ||
      source_hr == target_hr ||
      source_hr->is_empty() ||
      source_hr->is_young() ||
      source_hr->is_continues_humongous() ||
      target_hr->is_empty() ||
      target_hr->is_continues_humongous()) {
    return;
  }

  record_inbound_region_ref(source_hr->hrm_index(), target_hr->hrm_index());
}

bool G1RemoteMemoryManager::is_inbound_source_for_eviction_set(uint source_region,
                                                               const bool* eviction_set,
                                                               uint num_regions) const {
  if (!G1RemoteUseInboundRegionSummary ||
      !G1RemoteUseFastPhaseC ||
      _inbound_region_summary == nullptr ||
      eviction_set == nullptr ||
      source_region >= _inbound_region_summary_capacity) {
    return false;
  }

  const uint cap = _inbound_region_summary_capacity;
  const uint words = inbound_summary_words_per_row(cap);
  const uint word = source_region >> 6;
  const uint64_t mask = (uint64_t)1 << (source_region & 63);
  const uint limit = MIN2(num_regions, cap);
  for (uint target = 0; target < limit; target++) {
    if (eviction_set[target] &&
        (_inbound_region_summary[(size_t)target * words + word] & mask) != 0) {
      return true;
    }
  }
  return false;
}

bool G1RemoteMemoryManager::ensure_dense_segment_capacity(uint num_regions) {
  if (num_regions <= _dense_segment_capacity) {
    return true;
  }
  uint new_cap = MAX2(num_regions, _dense_segment_capacity * 2);
  if (new_cap < 1024) {
    new_cap = 1024;
  }
  DenseSegmentEntry* entries = NEW_C_HEAP_ARRAY(DenseSegmentEntry, new_cap, mtGC);
  for (uint i = 0; i < new_cap; i++) {
    entries[i].clear();
  }
  if (_dense_segments != nullptr) {
    for (uint i = 0; i < _dense_segment_capacity; i++) {
      entries[i] = _dense_segments[i];
    }
    FREE_C_HEAP_ARRAY(DenseSegmentEntry, _dense_segments);
  }
  _dense_segments = entries;
  _dense_segment_capacity = new_cap;
  return true;
}

bool G1RemoteMemoryManager::dense_segments_enabled() const {
  return G1RemoteUseDenseSegments &&
         _backend != nullptr &&
         _backend->supports_segments();
}

static bool remote_eviction_valid_klass(Klass* k);
static bool remote_eviction_valid_local_oop(G1CollectedHeap* g1h,
                                            oop obj,
                                            HeapRegion* hr,
                                            Klass** klass_out);

enum DenseDirectTargetState {
  DenseDirectTargetInvalid,
  DenseDirectTargetRemote,
  DenseDirectTargetLocal,
  DenseDirectTargetAlias
};

static DenseDirectTargetState classify_dense_direct_target(G1RemoteMemoryManager* rmm,
                                                           G1CollectedHeap* g1h,
                                                           uintptr_t addr,
                                                           RemoteHandle** alias_out,
                                                           Klass** klass_out) {
  if (alias_out != nullptr) {
    *alias_out = nullptr;
  }
  if (klass_out != nullptr) {
    *klass_out = nullptr;
  }
  if (!g1_remote_oop_is_aligned(addr) ||
      g1h == nullptr ||
      !g1h->is_in_reserved((void*)addr)) {
    return DenseDirectTargetInvalid;
  }

  HeapRegion* target_hr = g1h->heap_region_containing_or_null((void*)addr);
  if ((rmm != nullptr && rmm->is_dense_segment_remote_addr(addr)) ||
      (target_hr != nullptr && target_hr->is_evict_guarded())) {
    return DenseDirectTargetRemote;
  }

  Klass* target_klass = nullptr;
  if (target_hr != nullptr &&
      remote_eviction_valid_local_oop(g1h, cast_to_oop((HeapWord*)addr),
                                      target_hr, &target_klass)) {
    if (klass_out != nullptr) {
      *klass_out = target_klass;
    }
    return DenseDirectTargetLocal;
  }

  RemoteHandle* alias = rmm == nullptr ? nullptr :
      rmm->handle_for_stale_eviction_addr(addr);
  if (alias != nullptr) {
    uintptr_t state = alias->load_state_and_addr_acquire() &
                      REMOTE_HANDLE_STATE_MASK;
    if (state != REMOTE_HANDLE_DEAD) {
      if (alias_out != nullptr) {
        *alias_out = alias;
      }
      return DenseDirectTargetAlias;
    }
  }

  return DenseDirectTargetInvalid;
}

void G1RemoteMemoryManager::release_dense_segment_edges(DenseSegmentEdge* edges,
                                                        uint32_t count) {
  if (edges == nullptr) {
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    if (edges[i].kind == DenseSegmentEdgeHandle &&
        edges[i].target_handle != nullptr) {
      edges[i].target_handle->decrement_remote_refcount();
    }
  }
  FREE_C_HEAP_ARRAY(DenseSegmentEdge, edges);
}

bool G1RemoteMemoryManager::scan_dense_segment_region(HeapRegion* hr,
                                                      bool build_edges,
                                                      DenseSegmentEdge** out_edges,
                                                      uint32_t* out_edge_count,
                                                      size_t* out_object_count,
                                                      const char** reason) {
  if (out_edges != nullptr) {
    *out_edges = nullptr;
  }
  if (out_edge_count != nullptr) {
    *out_edge_count = 0;
  }
  if (out_object_count != nullptr) {
    *out_object_count = 0;
  }
  if (reason != nullptr) {
    *reason = "ok";
  }

  if (hr == nullptr || hr->is_free() || hr->is_empty() || hr->is_evict_guarded()) {
    if (reason != nullptr) *reason = "bad-region-state";
    return false;
  }

  class DenseSegmentBoundaryClosure : public BasicOopIterateClosure {
    G1RemoteMemoryManager* _rmm;
    G1CollectedHeap*       _g1h;
    HeapWord*              _bottom;
    HeapWord*              _top;
    bool                   _build_edges;
    RemoteHandleAllocBuffer _hab;
    DenseSegmentEdge*      _edges;
    uint32_t               _edge_count;
    uint32_t               _edge_capacity;
    bool                   _ok;
    const char*            _reason;

    void fail(const char* reason) {
      if (_ok) {
        _reason = reason;
      }
      _ok = false;
    }

    bool ensure_capacity() {
      if (_edge_count < _edge_capacity) {
        return true;
      }
      if (_edge_count == UINT32_MAX) {
        fail("too-many-boundary-edges");
        return false;
      }
      uint32_t new_cap = _edge_capacity == 0 ? 128 : _edge_capacity * 2;
      if (new_cap < _edge_capacity) {
        fail("too-many-boundary-edges");
        return false;
      }
      DenseSegmentEdge* next =
          NEW_C_HEAP_ARRAY(DenseSegmentEdge, new_cap, mtGC);
      if (_edges != nullptr) {
        memcpy(next, _edges, sizeof(DenseSegmentEdge) * _edge_count);
        FREE_C_HEAP_ARRAY(DenseSegmentEdge, _edges);
      }
      _edges = next;
      _edge_capacity = new_cap;
      return true;
    }

    void add_handle_edge(oop* field, RemoteHandle* h) {
      if (!_ok || h == nullptr) {
        return;
      }
      if (_build_edges && !ensure_capacity()) {
        return;
      }
      uint32_t offset = (uint32_t)((uintptr_t)field - (uintptr_t)_bottom);
      if (_build_edges) {
        _edges[_edge_count].field_offset = offset;
        _edges[_edge_count].kind = DenseSegmentEdgeHandle;
        _edges[_edge_count].target_handle = h;
        _edges[_edge_count].target_addr = 0;
        h->increment_remote_refcount();
      }
      _edge_count++;
    }

    bool valid_local_target(uintptr_t target_addr, oop* field) {
      if (!is_aligned((address)target_addr, HeapWordSize)) {
        fail("unaligned-oop");
        return false;
      }

      HeapWord* target_word = (HeapWord*)target_addr;
      if (target_word >= _bottom && target_word < _top) {
        return true;
      }

      if (_rmm->is_dense_segment_remote_addr(target_addr)) {
        fail("dense-direct-edge-disabled");
        return false;
      }

      if (!_g1h->is_in_reserved((void*)target_addr) ||
          !_g1h->is_in((void*)target_addr)) {
        fail("outgoing-not-live");
        return false;
      }

      HeapRegion* target_hr = _g1h->heap_region_containing_or_null((void*)target_addr);
      if (target_hr == nullptr || target_hr->is_free() || target_hr->is_empty() ||
          target_hr->is_evict_guarded() || target_hr->is_continues_humongous()) {
        fail("bad-outgoing-region");
        return false;
      }

      oop target = cast_to_oop(target_word);
      Klass* k = nullptr;
      if (!remote_eviction_valid_local_oop(_g1h, target, target_hr, &k)) {
        fail("bad-outgoing-oop");
        return false;
      }

      if (_build_edges) {
        RemoteHandle* h = _rmm->ensure_dormant_anchor_for(target, &_hab);
        add_handle_edge(field, h);
      } else {
        _edge_count++;
      }
      return true;
    }

  public:
    DenseSegmentBoundaryClosure(G1RemoteMemoryManager* rmm,
                                G1CollectedHeap* g1h,
                                HeapWord* bottom,
                                HeapWord* top,
                                bool build_edges)
      : _rmm(rmm), _g1h(g1h), _bottom(bottom), _top(top),
        _build_edges(build_edges), _hab(),
        _edges(nullptr), _edge_count(0), _edge_capacity(0),
        _ok(true), _reason("ok") {}

    ~DenseSegmentBoundaryClosure() {
      if (_edges != nullptr) {
        _rmm->release_dense_segment_edges(_edges, _edge_count);
      }
    }

    void do_oop(oop* p) override {
      if (!_ok) {
        return;
      }
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) {
        return;
      }

      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
        RemoteHandle* h = (RemoteHandle*)(raw & G1_OOP_ADDR_MASK);
        uintptr_t sa = h->load_state_and_addr_acquire();
        uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
        if (state == REMOTE_HANDLE_LOCAL) {
          uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
          HeapWord* target = (HeapWord*)addr;
          if (target >= _bottom && target < _top) {
            fail("internal-handle-ref");
            return;
          }
        }
        add_handle_edge(p, h);
        return;
      }

      uintptr_t target_addr = raw;
      if ((raw & G1_OOP_TAG_MASK) != 0) {
        if ((raw & G1_OOP_MANAGED_BIT) == 0) {
          fail("unknown-tagged-ref");
          return;
        }
        target_addr = raw & G1_OOP_ADDR_MASK;
      }
      valid_local_target(target_addr, p);
    }

    void do_oop(narrowOop* p) override {
      // Dense-segment Spark experiments run with UseCompressedOops=false.
    }

    bool ok() const { return _ok; }
    const char* reason() const { return _reason; }
    uint32_t edge_count() const { return _edge_count; }

    DenseSegmentEdge* release_edges() {
      DenseSegmentEdge* result = _edges;
      _edges = nullptr;
      _edge_capacity = 0;
      return result;
    }
  };

  DenseSegmentBoundaryClosure boundary_cl(this, _g1h, hr->bottom(), hr->top(),
                                          build_edges);
  size_t objects = 0;
  HeapWord* p = hr->bottom();
  HeapWord* region_end = hr->end();
  while (p < hr->top()) {
    if (p < hr->bottom() || p >= region_end) {
      if (reason != nullptr) *reason = "parse-out-of-region";
      return false;
    }
    oop obj = cast_to_oop(p);
    if (obj->is_forwarded()) {
      if (reason != nullptr) *reason = "forwarded";
      return false;
    }
    Klass* k = obj->klass_or_null_acquire();
    if (!remote_eviction_valid_klass(k)) {
      if (reason != nullptr) *reason = "bad-klass";
      return false;
    }
    markWord mark = obj->mark();
    if (!mark.is_unlocked()) {
      if (reason != nullptr) *reason = "locked-mark";
      return false;
    }
    Klass* size_k = obj->klass_or_null_acquire();
    if (size_k != k) {
      if (reason != nullptr) *reason = "klass-changed";
      return false;
    }
    size_t sz = obj->size_given_klass(size_k);
    if (sz < (size_t)MinObjAlignment ||
        !is_object_aligned(sz) ||
        sz > (size_t)(region_end - p) ||
        sz > (size_t)(hr->top() - p)) {
      if (reason != nullptr) *reason = "bad-size";
      return false;
    }
    if (!G1CollectedHeap::is_obj_filler(obj)) {
      if (!k->is_typeArray_klass()) {
        obj->oop_iterate(&boundary_cl);
        if (!boundary_cl.ok()) {
          if (reason != nullptr) *reason = boundary_cl.reason();
          return false;
        }
      }
      objects++;
    }
    p += sz;
  }
  if (p != hr->top() || objects == 0) {
    if (reason != nullptr) *reason = "empty-or-unparseable";
    return false;
  }

  if (out_object_count != nullptr) {
    *out_object_count = objects;
  }
  if (out_edge_count != nullptr) {
    *out_edge_count = boundary_cl.edge_count();
  }
  if (build_edges && out_edges != nullptr) {
    *out_edges = boundary_cl.release_edges();
  }
  return true;
}

bool G1RemoteMemoryManager::can_evict_dense_segment_region(HeapRegion* hr,
                                                           const char** reason,
                                                           size_t* object_count) {
  if (reason != nullptr) *reason = "ok";
  if (object_count != nullptr) *object_count = 0;
  if (!dense_segments_enabled()) {
    if (reason != nullptr) *reason = "disabled";
    return false;
  }
  if (hr == nullptr || hr->is_free() || hr->is_empty() || hr->is_evict_guarded()) {
    if (reason != nullptr) *reason = "bad-region-state";
    return false;
  }
  if (hr->in_collection_set() || hr->has_index_in_opt_cset()) {
    if (reason != nullptr) *reason = "collection-set-region";
    return false;
  }
  if (_g1h->is_old_gc_alloc_region(hr)) {
    if (reason != nullptr) *reason = "old-gc-alloc-region";
    return false;
  }
  if (!hr->is_old() || hr->is_humongous() || hr->is_continues_humongous() ||
      hr->is_fetch_cache()) {
    if (reason != nullptr) *reason = "unsupported-region-kind";
    return false;
  }
  if (hr->used() == 0 || hr->used() > HeapRegion::GrainBytes) {
    if (reason != nullptr) *reason = "invalid-used";
    return false;
  }

  int local_handles = count_local_handles_in_region(hr, 0);
  if (local_handles > 0) {
    if (reason != nullptr) *reason = "local-handles";
    return false;
  }

  size_t objects = 0;
  if (!scan_dense_segment_region(hr, false, nullptr, nullptr, &objects, reason)) {
    return false;
  }
  if (object_count != nullptr) *object_count = objects;
  return true;
}

bool G1RemoteMemoryManager::evict_dense_segment_region(HeapRegion* hr, uint32_t flags) {
  return evict_dense_segment_region(hr, nullptr, nullptr, flags);
}

bool G1RemoteMemoryManager::evict_dense_segment_region(HeapRegion* hr,
                                                       const char** out_reason,
                                                       size_t* out_object_count,
                                                       uint32_t flags) {
  const char* reason = "ok";
  size_t objects = 0;
  if (out_reason != nullptr) *out_reason = reason;
  if (out_object_count != nullptr) *out_object_count = 0;

  if (!dense_segments_enabled()) {
    if (out_reason != nullptr) *out_reason = "disabled";
    return false;
  }
  if (hr == nullptr || hr->is_free() || hr->is_empty() || hr->is_evict_guarded()) {
    if (out_reason != nullptr) *out_reason = "bad-region-state";
    return false;
  }
  if (hr->in_collection_set() || hr->has_index_in_opt_cset()) {
    if (out_reason != nullptr) *out_reason = "collection-set-region";
    return false;
  }
  if (_g1h->is_old_gc_alloc_region(hr)) {
    if (out_reason != nullptr) *out_reason = "old-gc-alloc-region";
    return false;
  }
  if (!hr->is_old() || hr->is_humongous() || hr->is_continues_humongous() ||
      hr->is_fetch_cache()) {
    if (out_reason != nullptr) *out_reason = "unsupported-region-kind";
    return false;
  }
  if (hr->used() == 0 || hr->used() > HeapRegion::GrainBytes) {
    if (out_reason != nullptr) *out_reason = "invalid-used";
    return false;
  }

  int local_handles = count_local_handles_in_region(hr, 0);
  if (local_handles > 0) {
    if (out_reason != nullptr) *out_reason = "local-handles";
    return false;
  }

  if (!ensure_dense_segment_capacity(hr->hrm_index() + 1)) {
    if (out_reason != nullptr) *out_reason = "dense-segment-capacity";
    Atomic::inc(&_dense_segment_evict_failures);
    return false;
  }

  DenseSegmentEdge* edges = nullptr;
  uint32_t edge_count = 0;
  if (!scan_dense_segment_region(hr, true, &edges, &edge_count,
                                 &objects, &reason)) {
    release_dense_segment_edges(edges, edge_count);
    if (out_reason != nullptr) {
      *out_reason = reason != nullptr ? reason : "scan-failed";
    }
    Atomic::inc(&_dense_segment_evict_failures);
    log_debug(gc)("Dense segment evict rejected: region=%u reason=%s",
                  hr != nullptr ? hr->hrm_index() : (uint)-1,
                  reason != nullptr ? reason : "unknown");
    return false;
  }
  if (out_reason != nullptr) *out_reason = "ok";
  if (out_object_count != nullptr) *out_object_count = objects;

  uint region_idx = hr->hrm_index();

  uint64_t segment_id = 0;
  dense_segment_lock();
  DenseSegmentEntry* entry = &_dense_segments[region_idx];
  if (entry->state == DenseSegmentRemote || entry->state == DenseSegmentFetching) {
    dense_segment_unlock();
    release_dense_segment_edges(edges, edge_count);
    if (out_reason != nullptr) *out_reason = "already-remote-or-fetching";
    Atomic::inc(&_dense_segment_evict_failures);
    log_warning(gc)("Dense segment evict rejected: region=%u already has "
                    "segment state=%u id=" UINT64_FORMAT,
                    region_idx, entry->state, entry->segment_id);
    return false;
  }
  segment_id = ((uint64_t)region_idx << 32) | (_dense_segment_next_id++);
  dense_segment_unlock();

  size_t byte_size = hr->used();
  bool ok = _backend->evict_segment(segment_id, (uintptr_t)hr->bottom(),
                                    hr->bottom(), byte_size, flags);
  if (!ok) {
    release_dense_segment_edges(edges, edge_count);
    if (out_reason != nullptr) *out_reason = "backend-evict-failed";
    Atomic::inc(&_dense_segment_evict_failures);
    return false;
  }

  dense_segment_lock();
  entry = &_dense_segments[region_idx];
  entry->segment_id = segment_id;
  entry->base = (uintptr_t)hr->bottom();
  entry->byte_size = byte_size;
  if (entry->edges != nullptr) {
    FREE_C_HEAP_ARRAY(DenseSegmentEdge, entry->edges);
  }
  entry->edges = edges;
  entry->edge_count = edge_count;
  entry->flags = flags;
  entry->last_evict_epoch = _gc_epoch;
  Atomic::add(&_dense_segment_remote_bytes, (uint64_t)byte_size);
  Atomic::release_store(&entry->state, (uint32_t)DenseSegmentRemote);
  dense_segment_unlock();

  Atomic::inc(&_dense_segment_evict_success);
  log_info(gc)("Dense segment evicted: region=%u segment=" UINT64_FORMAT
               " objects=" SIZE_FORMAT " boundary_edges=%u bytes=" SIZE_FORMAT
               " [" PTR_FORMAT ", " PTR_FORMAT ")",
               region_idx, segment_id, objects, edge_count, byte_size,
               p2i(hr->bottom()), p2i(hr->top()));
  return true;
}

bool G1RemoteMemoryManager::is_dense_segment_remote_addr(uintptr_t addr) const {
  if (!G1RemoteUseDenseSegments || _dense_segments == nullptr ||
      _g1h == nullptr || !_g1h->is_in_reserved((void*)addr)) {
    return false;
  }
  HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)addr);
  if (hr == nullptr) {
    return false;
  }
  uint idx = hr->hrm_index();
  if (idx >= _dense_segment_capacity) {
    return false;
  }
  const DenseSegmentEntry* entry = &_dense_segments[idx];
  uint32_t state = entry->state;
  return entry->segment_id != 0 &&
         (state == DenseSegmentRemote || state == DenseSegmentFetching) &&
         addr >= entry->base &&
         addr < entry->base + entry->byte_size;
}

bool G1RemoteMemoryManager::is_dense_segment_managed_addr(uintptr_t addr) const {
  if (!G1RemoteUseDenseSegments || _dense_segments == nullptr ||
      _g1h == nullptr || !_g1h->is_in_reserved((void*)addr)) {
    return false;
  }
  HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)addr);
  if (hr == nullptr) {
    return false;
  }
  uint idx = hr->hrm_index();
  if (idx >= _dense_segment_capacity) {
    return false;
  }
  const DenseSegmentEntry* entry = &_dense_segments[idx];
  uint32_t state = entry->state;
  return entry->segment_id != 0 &&
         state != DenseSegmentNone &&
         addr >= entry->base &&
         addr < entry->base + entry->byte_size;
}

static bool rebuild_dense_segment_bot(HeapRegion* hr, size_t byte_size) {
  HeapWord* p = hr->bottom();
  HeapWord* top = hr->bottom() + byte_size / HeapWordSize;
  HeapWord* region_end = hr->end();
  while (p < top) {
    oop obj = cast_to_oop(p);
    Klass* k = obj->klass_or_null();
    if (k == nullptr) {
      log_warning(gc)("Dense segment restore: null klass at " PTR_FORMAT
                      " in region %u", p2i(p), hr->hrm_index());
      return false;
    }
    size_t sz = obj->size_given_klass(k);
    if (sz == 0 || sz > (size_t)(region_end - p)) {
      log_warning(gc)("Dense segment restore: bad object size " SIZE_FORMAT
                      " at " PTR_FORMAT " in region %u",
                      sz, p2i(p), hr->hrm_index());
      return false;
    }
    hr->update_bot_for_obj(p, sz);
    p += sz;
  }
  return p == top;
}

static void dirty_dense_segment_cards(G1CollectedHeap* g1h, HeapRegion* hr) {
  if (g1h == nullptr || hr == nullptr || hr->bottom() >= hr->top()) {
    return;
  }
  G1CardTable* ct = g1h->card_table();
  CardTable::CardValue* start_card = ct->byte_for(hr->bottom());
  CardTable::CardValue* end_card = ct->byte_for(hr->top() - 1) + 1;
  memset(start_card, CardTable::dirty_card_val(), end_card - start_card);

  G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
  G1DirtyCardQueue tmp_queue(&dcqs);
  for (CardTable::CardValue* card = start_card; card < end_card; card++) {
    dcqs.enqueue(tmp_queue, card);
  }
  dcqs.flush_queue(tmp_queue);
}

void G1RemoteMemoryManager::patch_dense_segment_boundary_edges(DenseSegmentEntry* entry,
                                                               HeapRegion* hr) {
  if (entry == nullptr || hr == nullptr || entry->edges == nullptr ||
      entry->edge_count == 0) {
    return;
  }

  DenseSegmentEdge* edges = entry->edges;
  uint32_t count = entry->edge_count;
  entry->edges = nullptr;
  entry->edge_count = 0;

  bool cm_active = concurrent_marking_active();
  int clean = 0;
  int shared = 0;
  int direct = 0;
  int direct_clean = 0;
  int direct_tracked = 0;
  int direct_shared = 0;
  int handle_tracked = 0;
  int nulled = 0;
  int invalid = 0;
  bool direct_entries_locked = false;
  RemoteHandleAllocBuffer hab;

  uintptr_t base = (uintptr_t)hr->bottom();
  for (uint32_t i = 0; i < count; i++) {
    DenseSegmentEdge* edge = &edges[i];
    if ((size_t)edge->field_offset + sizeof(uintptr_t) > entry->byte_size) {
      invalid++;
      continue;
    }

    uintptr_t* field = (uintptr_t*)(base + edge->field_offset);
    if (edge->kind == DenseSegmentEdgeDirect) {
      uintptr_t target_addr = edge->target_addr & G1_OOP_ADDR_MASK;
      RemoteHandle* alias = nullptr;
      DenseDirectTargetState target_state =
          classify_dense_direct_target(this, _g1h, target_addr, &alias, nullptr);
      if (target_state == DenseDirectTargetLocal) {
        RemoteHandle* h =
            ensure_dormant_anchor_for(cast_to_oop((HeapWord*)target_addr), &hab);
        if (h == nullptr) {
          *field = 0;
          nulled++;
          invalid++;
          continue;
        }
        h->increment_remote_refcount();
        *field = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
        if (!direct_entries_locked) {
          tagged_field_lock();
          direct_entries_locked = true;
        }
        TaggedFieldEntry tagged_entry;
        tagged_entry._field_addr = (oop*)field;
        tagged_entry._handle = h;
        tagged_entry._tagged_raw = 0;
        tagged_entry._kind = TaggedFieldDenseHandle;
        add_tagged_field_entry_locked(tagged_entry);
        shared++;
        direct_shared++;
        handle_tracked++;
        continue;
      } else if (target_state == DenseDirectTargetAlias && alias != nullptr) {
        alias->increment_remote_refcount();
        *field = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)alias;
        if (!direct_entries_locked) {
          tagged_field_lock();
          direct_entries_locked = true;
        }
        TaggedFieldEntry tagged_entry;
        tagged_entry._field_addr = (oop*)field;
        tagged_entry._handle = alias;
        tagged_entry._tagged_raw = 0;
        tagged_entry._kind = TaggedFieldDenseHandle;
        add_tagged_field_entry_locked(tagged_entry);
        shared++;
        direct_shared++;
        handle_tracked++;
        continue;
      }

      if (target_state == DenseDirectTargetRemote) {
        log_debug(gc)("Dense segment boundary patch: dropping legacy direct edge "
                      "without handle target=" PTR_FORMAT " region=%u",
                      p2i((void*)target_addr), hr->hrm_index());
      }
      *field = 0;
      nulled++;
      invalid++;
      continue;
    }

    RemoteHandle* target = edge->target_handle;
    if (target == nullptr) {
      *field = 0;
      nulled++;
      continue;
    }

    uintptr_t sa = target->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    if (state == REMOTE_HANDLE_LOCAL) {
      int stale = 0;
      uintptr_t target_addr = sa & REMOTE_HANDLE_ADDR_MASK;
      if (validate_local_handle_addr(target, "DENSE-SEGMENT-PATCH", &stale, 8)) {
        *field = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)target;
        if (!direct_entries_locked) {
          tagged_field_lock();
          direct_entries_locked = true;
        }
        TaggedFieldEntry tagged_entry;
        tagged_entry._field_addr = (oop*)field;
        tagged_entry._handle = target;
        tagged_entry._tagged_raw = 0;
        tagged_entry._kind = TaggedFieldDenseHandle;
        add_tagged_field_entry_locked(tagged_entry);
        shared++;
        handle_tracked++;
      } else {
        *field = 0;
        nulled++;
        if (cm_active) {
          defer_refcount_decrement(target);
        } else {
          target->decrement_remote_refcount();
        }
      }
    } else if (state == REMOTE_HANDLE_DEAD) {
      *field = 0;
      nulled++;
      if (cm_active) {
        defer_refcount_decrement(target);
      } else {
        target->decrement_remote_refcount();
      }
    } else {
      *field = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)target;
      if (!direct_entries_locked) {
        tagged_field_lock();
        direct_entries_locked = true;
      }
      TaggedFieldEntry tagged_entry;
      tagged_entry._field_addr = (oop*)field;
      tagged_entry._handle = target;
      tagged_entry._tagged_raw = 0;
      tagged_entry._kind = TaggedFieldDenseHandle;
      add_tagged_field_entry_locked(tagged_entry);
      shared++;
      handle_tracked++;
    }
  }

  if (direct_entries_locked) {
    tagged_field_unlock();
  }

  FREE_C_HEAP_ARRAY(DenseSegmentEdge, edges);

  log_info(gc)("Dense segment boundary patch: region=%u edges=%u clean=%d "
               "shared=%d direct=%d direct_clean=%d direct_tracked=%d "
               "direct_shared=%d handle_tracked=%d nulled=%d invalid=%d",
               hr->hrm_index(), count, clean, shared, direct, direct_clean,
               direct_tracked, direct_shared, handle_tracked, nulled, invalid);
}

bool G1RemoteMemoryManager::localize_dense_segment_for_addr(uintptr_t addr) {
  if (!G1RemoteUseDenseSegments || _dense_segments == nullptr ||
      _g1h == nullptr || !_g1h->is_in_reserved((void*)addr)) {
    return false;
  }

  HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)addr);
  if (hr == nullptr) {
    return false;
  }
  uint idx = hr->hrm_index();
  if (idx >= _dense_segment_capacity) {
    return false;
  }

  uint64_t segment_id = 0;
  uintptr_t base = 0;
  size_t byte_size = 0;
  uint32_t flags = 0;
  uint32_t last_evict_epoch = 0;
  SpinYield yield;

  while (true) {
    dense_segment_lock();
    DenseSegmentEntry* entry = &_dense_segments[idx];
    uint32_t state = Atomic::load(&entry->state);
    bool addr_in_segment = entry->segment_id != 0 &&
                           addr >= entry->base &&
                           addr < entry->base + entry->byte_size;
    if (!addr_in_segment || state == DenseSegmentNone) {
      dense_segment_unlock();
      return false;
    }
    if (state == DenseSegmentLocal) {
      dense_segment_unlock();
      return true;
    }
    if (state == DenseSegmentRemote) {
      segment_id = entry->segment_id;
      base = entry->base;
      byte_size = entry->byte_size;
      flags = entry->flags;
      last_evict_epoch = entry->last_evict_epoch;
      Atomic::release_store(&entry->state, (uint32_t)DenseSegmentFetching);
      dense_segment_unlock();
      break;
    }
    dense_segment_unlock();
    // This method is reached from resolve_tagged_oop_no_safepoint(), including
    // C1/C2 leaf load barriers.  Waiters must not process safepoints here:
    // their Java frames are not entered through a JRT_ENTRY transition.  The
    // fetching thread must make any contended Heap_lock wait visible through
    // HotSpot's Mutex blocking path below.
    yield.wait();
  }

  void* buf = os::malloc(byte_size, mtGC);
  if (buf == nullptr) {
    dense_segment_lock();
    DenseSegmentEntry* entry = &_dense_segments[idx];
    if (entry->segment_id == segment_id &&
        Atomic::load(&entry->state) == DenseSegmentFetching) {
      Atomic::release_store(&entry->state, (uint32_t)DenseSegmentRemote);
    }
    dense_segment_unlock();
    Atomic::inc(&_dense_segment_fetch_failures);
    return false;
  }

  uintptr_t fetched_base = 0;
  size_t fetched_bytes = 0;
  uint32_t fetched_flags = 0;
  bool ok = _backend != nullptr &&
            _backend->fetch_segment(segment_id, &fetched_base, buf, byte_size,
                                    &fetched_bytes, &fetched_flags);
  if (ok && (fetched_base != base || fetched_bytes != byte_size ||
             fetched_flags != flags)) {
    log_warning(gc)("Dense segment fetch metadata mismatch: segment="
                    UINT64_FORMAT " expected_base=" PTR_FORMAT
                    " actual_base=" PTR_FORMAT " expected_bytes="
                    SIZE_FORMAT " actual_bytes=" SIZE_FORMAT
                    " expected_flags=%u actual_flags=%u",
                    segment_id, p2i((void*)base), p2i((void*)fetched_base),
                    byte_size, fetched_bytes, flags, fetched_flags);
    ok = false;
  }

  if (ok) {
    hr = _g1h->region_at_or_null(idx);
  }

  if (ok) {
    ok = hr != nullptr && hr->is_evict_guarded() && !hr->is_free();
    if (!ok) {
      log_warning(gc)("Dense segment fetch rejected stale region state: "
                      "region=%u segment=" UINT64_FORMAT " hr=" PTR_FORMAT
                      " guarded=%s free=%s",
                      idx, segment_id, p2i(hr),
                      hr != nullptr && hr->is_evict_guarded() ? "true" : "false",
                      hr != nullptr && hr->is_free() ? "true" : "false");
    }
  }

  bool region_unguarded = false;
  if (ok) {
    region_unguarded = os::unguard_memory((char*)hr->bottom(), HeapRegion::GrainBytes);
    if (!region_unguarded) {
      log_warning(gc)("Dense segment fetch failed to unguard region %u "
                      "segment=" UINT64_FORMAT,
                      idx, segment_id);
      ok = false;
    }
  }
  if (ok) {
    HeapWord* expected_top = hr->bottom() + fetched_bytes / HeapWordSize;
    guarantee(hr->top() == expected_top,
              "Dense segment region metadata changed while remote");
    memcpy(hr->bottom(), buf, fetched_bytes);
    bool parse_ok = rebuild_dense_segment_bot(hr, fetched_bytes);
    guarantee(parse_ok, "Dense segment restore produced an unparseable region");
  }
  if (ok) {
    patch_dense_segment_boundary_edges(&_dense_segments[idx], hr);
    dirty_dense_segment_cards(_g1h, hr);
    _backend->discard_segment(segment_id);
    hr->clear_evict_guarded();
    hr->clear_rss_trimmed_free();
  } else if (region_unguarded && hr != nullptr) {
    bool guarded = os::guard_memory((char*)hr->bottom(), HeapRegion::GrainBytes);
    guarantee(guarded, "Dense segment restore rollback must re-guard region");
  }

  os::free(buf);

  uint32_t fetch_age = _gc_epoch - last_evict_epoch;
  uint16_t fetch_backoff_shift = 0;
  uint16_t fetch_churn_count = 0;
  uint backoff_cycles = 0;
  bool churn_fetch = false;
  dense_segment_lock();
  DenseSegmentEntry* entry = &_dense_segments[idx];
  if (entry->segment_id == segment_id) {
    if (ok) {
      fetch_age = _gc_epoch - entry->last_evict_epoch;
      churn_fetch = G1RemoteDenseFetchBackoffChurnWindowGCCycles > 0 &&
                    fetch_age <= G1RemoteDenseFetchBackoffChurnWindowGCCycles;
      if (churn_fetch) {
        if (entry->fetch_backoff_shift < 15) {
          entry->fetch_backoff_shift++;
        }
        if (entry->fetch_churn_count < UINT16_MAX) {
          entry->fetch_churn_count++;
        }
      } else {
        if (entry->fetch_backoff_shift > 0) {
          entry->fetch_backoff_shift--;
        }
        if (entry->fetch_churn_count > 0) {
          entry->fetch_churn_count--;
        }
      }
      entry->last_fetch_epoch = _gc_epoch;
      fetch_backoff_shift = entry->fetch_backoff_shift;
      fetch_churn_count = entry->fetch_churn_count;
      backoff_cycles = dense_fetch_backoff_cycles_for_shift(fetch_backoff_shift);
      backoff_eviction_region(idx, backoff_cycles);
      uint64_t remote_bytes = Atomic::load(&_dense_segment_remote_bytes);
      uint64_t restored_bytes = MIN2((uint64_t)entry->byte_size, remote_bytes);
      if (restored_bytes > 0) {
        Atomic::sub(&_dense_segment_remote_bytes, restored_bytes);
      }
    }
    Atomic::release_store(&entry->state,
                          (uint32_t)(ok ? DenseSegmentLocal : DenseSegmentRemote));
  }
  dense_segment_unlock();

  if (ok) {
    Atomic::inc(&_dense_segment_fetch_success);
    log_info(gc)("Dense segment localized: region=%u segment=" UINT64_FORMAT
                 " bytes=" SIZE_FORMAT " addr=" PTR_FORMAT
                 " fetch_age=%u churn=%s churn_count=%u backoff_shift=%u"
                 " backoff_cycles=%u",
                 idx, segment_id, fetched_bytes, p2i((void*)addr),
                 fetch_age, churn_fetch ? "yes" : "no",
                 fetch_churn_count, fetch_backoff_shift, backoff_cycles);
  } else {
    Atomic::inc(&_dense_segment_fetch_failures);
    log_warning(gc)("Dense segment fetch failed: region=%u segment="
                    UINT64_FORMAT " addr=" PTR_FORMAT,
                    idx, segment_id, p2i((void*)addr));
  }
  return ok;
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

// ============================================================
// Edge Table Construction
// ============================================================
// Closure that scans an object's oop fields and builds an edge table.
// For each non-null oop field, creates a dormant anchor Handle for the
// target and records the edge (field_offset → target_handle).

class EdgeTableBuildClosure : public BasicOopIterateClosure {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  RemoteHandleAllocBuffer* _hab;
  G1RemoteMemoryManager::HandleEntryAllocBuffer* _eab;
  RemoteHandle**        _pending_head;
  RemoteHandle**        _pending_tail;
  size_t*               _pending_count;
  oop                    _base_obj;

  // Heap-allocated, growable. Replaces the old fixed MAX_EDGES=8192 stack
  // array, which pinned whole regions whenever a single large array
  // (e.g. scala.Tuple3[N>8192]) was hit during eviction.
  G1RemoteMemoryManager::EdgeEntry* _edges;
  uint32_t _count;
  uint32_t _capacity;

  void grow() {
    uint32_t new_cap = (_capacity == 0) ? 16u : _capacity * 2u;
    G1RemoteMemoryManager::EdgeEntry* new_edges =
      NEW_C_HEAP_ARRAY(G1RemoteMemoryManager::EdgeEntry, new_cap, mtGC);
    if (_edges != nullptr) {
      memcpy(new_edges, _edges,
             (size_t)_count * sizeof(G1RemoteMemoryManager::EdgeEntry));
      FREE_C_HEAP_ARRAY(G1RemoteMemoryManager::EdgeEntry, _edges);
    }
    _edges = new_edges;
    _capacity = new_cap;
  }

  void append(uint32_t offset, RemoteHandle* h) {
    if (_count >= _capacity) grow();
    _edges[_count]._field_offset = offset;
    _edges[_count]._target_handle = h;
    _count++;
  }

  oop canonical_target(oop target) {
    if (target == nullptr || !_g1h->is_in_reserved(target)) return nullptr;
    HeapRegion* hr = _g1h->heap_region_containing_or_null(target);
    if (hr == nullptr || hr->is_free() || hr->is_evict_guarded() ||
        hr->is_empty() || hr->is_continues_humongous() ||
        !_g1h->is_in(target)) {
      return nullptr;
    }
    if (target->is_forwarded()) {
      target = target->forwardee();
      if (target == nullptr || !_g1h->is_in_reserved(target)) return nullptr;
      hr = _g1h->heap_region_containing_or_null(target);
      if (hr == nullptr || hr->is_free() || hr->is_evict_guarded() ||
          hr->is_empty() || hr->is_continues_humongous() ||
          !_g1h->is_in(target)) {
        return nullptr;
      }
    }

    Klass* k = target->klass_or_null_acquire();
    if (!remote_eviction_valid_klass(k) ||
        G1CollectedHeap::is_obj_filler(target)) {
      return nullptr;
    }
    size_t word_size = target->size_given_klass(k);
    if (word_size < (size_t)MinObjAlignment ||
        !is_object_aligned(word_size) ||
        word_size > (size_t)(hr->top() - cast_from_oop<HeapWord*>(target)) ||
        word_size > (size_t)(hr->end() - cast_from_oop<HeapWord*>(target))) {
      return nullptr;
    }
    return target;
  }

  RemoteHandle* remap_stale_raw_target(uintptr_t raw) {
    RemoteHandle* h = _rmm->handle_for_stale_eviction_addr(raw);
    if (h == nullptr) {
      return nullptr;
    }
    uintptr_t state = h->load_state_and_addr_acquire() & REMOTE_HANDLE_STATE_MASK;
    if (state == REMOTE_HANDLE_LOCAL &&
        !_rmm->validate_local_handle_addr(h, "EDGE-STALE-ALIAS", nullptr, 0)) {
      return nullptr;
    }
    return h;
  }

public:
  EdgeTableBuildClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                        RemoteHandleAllocBuffer* hab, oop base,
                        G1RemoteMemoryManager::HandleEntryAllocBuffer* eab = nullptr,
                        RemoteHandle** pending_head = nullptr,
                        RemoteHandle** pending_tail = nullptr,
                        size_t* pending_count = nullptr)
    : _rmm(rmm), _g1h(g1h), _hab(hab), _eab(eab),
      _pending_head(pending_head), _pending_tail(pending_tail),
      _pending_count(pending_count), _base_obj(base),
      _edges(nullptr), _count(0), _capacity(0) {}

  ~EdgeTableBuildClosure() {
    if (_edges != nullptr) {
      FREE_C_HEAP_ARRAY(G1RemoteMemoryManager::EdgeEntry, _edges);
    }
  }

  virtual void do_oop(oop* p) {
    // Read field as raw uintptr_t to avoid debug oop constructor checks on tagged values
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;  // null

    // If already tagged (bit 63 set), the field already has a Handle reference.
    if ((raw >> 63) != 0) {
      if (raw & G1_OOP_INDIRECT_BIT) {
        // Shared OOP → already points to a Handle
        RemoteHandle* h = (RemoteHandle*)(raw & G1_OOP_ADDR_MASK);
        uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
        append(offset, h);
        h->increment_remote_refcount();
      } else {
        // Unique OOP → strip tags, get target, create dormant anchor
        oop target = (oop)(raw & G1_OOP_ADDR_MASK);
        target = canonical_target(target);
        if (target != nullptr) {
          RemoteHandle* h = _eab != nullptr
            ? _rmm->ensure_dormant_anchor_for_parallel(target, _hab, _eab,
                                                       _pending_head, _pending_tail,
                                                       _pending_count)
            : _rmm->ensure_dormant_anchor_for(target, _hab);
          uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
          append(offset, h);
          h->increment_remote_refcount();
        }
      }
      return;
    }

    // Clean oop — create dormant anchor for the target
    uint32_t offset = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_base_obj));
    oop target = cast_to_oop(raw);
    target = canonical_target(target);
    if (target != nullptr) {
      RemoteHandle* h = _eab != nullptr
        ? _rmm->ensure_dormant_anchor_for_parallel(target, _hab, _eab,
                                                   _pending_head, _pending_tail,
                                                   _pending_count)
        : _rmm->ensure_dormant_anchor_for(target, _hab);
      append(offset, h);
      h->increment_remote_refcount();
      return;
    }

    RemoteHandle* h = remap_stale_raw_target(raw);
    if (h != nullptr) {
      append(offset, h);
      h->increment_remote_refcount();
    }
  }

  virtual void do_oop(narrowOop* p) {
    // Narrow oops: not used (UseCompressedOops=false in our config)
  }

  uint32_t count() const { return _count; }
  const G1RemoteMemoryManager::EdgeEntry* edges() const { return _edges; }
};

G1RemoteMemoryManager::ObjectEdgeTable*
G1RemoteMemoryManager::build_edge_table(oop obj, RemoteHandle* obj_handle,
                                        RemoteHandleAllocBuffer* hab,
                                        bool* zero_edges,
                                        HandleEntryAllocBuffer* eab,
                                        RemoteHandle** pending_head,
                                        RemoteHandle** pending_tail,
                                        size_t* pending_count) {
  if (zero_edges != nullptr) {
    *zero_edges = false;
  }

  EdgeTableBuildClosure cl(this, _g1h, hab, obj, eab,
                           pending_head, pending_tail, pending_count);
  obj->oop_iterate(&cl);

  if (cl.count() == 0) {
    if (zero_edges != nullptr) {
      *zero_edges = true;
    }
    return nullptr;
  }

  // Allocate exact-sized ObjectEdgeTable and copy from the (now-sized) build
  // buffer. The build buffer is freed by the closure destructor below.
  ObjectEdgeTable* et = ObjectEdgeTable::allocate(cl.count());
  et->_source_handle = obj_handle;
  et->_eviction_word_size = obj->size();
  for (uint32_t i = 0; i < cl.count(); i++) {
    et->add(cl.edges()[i]._field_offset, cl.edges()[i]._target_handle);
  }

  log_debug(gc)("Edge table built: obj=" PTR_FORMAT " edges=%u",
                p2i((void*)obj), cl.count());
  return et;
}

static volatile int _prep_fail_null = 0;
static volatile int _prep_fail_locked = 0;
static volatile int _prep_fail_array = 0;
static volatile int _prep_fail_obj_array = 0;
static volatile int _prep_fail_type_array_disabled = 0;
static volatile int _prep_fail_filler = 0;
static volatile int _prep_fail_edge = 0;
static volatile int _prep_fail_slot = 0;
static volatile int _prep_success = 0;
static volatile int _prep_success_type_array = 0;
static volatile int _prep_success_obj_array = 0;
static volatile int _prep_diag_logged = 0;

static uintptr_t array_chunk_segment_id_for(RemoteHandle* h) {
  return ((uintptr_t)RemoteLocationArrayChunk << 60) |
         ((uintptr_t)h & ((((uintptr_t)1) << 60) - 1));
}

bool G1RemoteMemoryManager::prepare_eviction_metadata(oop obj, RemoteHandleAllocBuffer* hab,
                                                      PreparedEviction* out) {
  if (obj == nullptr || out == nullptr) { Atomic::add(&_prep_fail_null, 1); return false; }

  markWord mw = obj->mark();
  if (!mw.is_unlocked()) {
    if (Atomic::add(&_prep_fail_locked, 1) <= 3 && !_prep_diag_logged) {
      log_info(gc)("prepare_eviction: locked obj=" PTR_FORMAT " mw=0x%lx klass=%s",
                   p2i((void*)obj), (unsigned long)mw.value(), obj->klass()->external_name());
    }
    return false;
  }

  Klass* klass = obj->klass();
  if (G1CollectedHeap::is_obj_filler(obj)) {
    Atomic::add(&_prep_fail_filler, 1);
    return false;
  }

  if (klass->is_array_klass()) {
    if (klass->is_typeArray_klass()) {
      if (!G1RemoteAllowTypeArrayEviction) {
        Atomic::add(&_prep_fail_array, 1);
        if (Atomic::add(&_prep_fail_type_array_disabled, 1) <= 3 && !_prep_diag_logged) {
          log_info(gc)("prepare_eviction: primitive array obj=" PTR_FORMAT
                       " klass=%s kept local (G1RemoteAllowTypeArrayEviction=false)",
                       p2i((void*)obj), klass->external_name());
        }
        return false;
      }
    } else if (klass->is_objArray_klass()) {
      if (!G1RemoteAllowObjectArrayEviction) {
        Atomic::add(&_prep_fail_array, 1);
        if (Atomic::add(&_prep_fail_obj_array, 1) <= 3 && !_prep_diag_logged) {
          log_info(gc)("prepare_eviction: object array obj=" PTR_FORMAT
                       " klass=%s kept local (G1RemoteAllowObjectArrayEviction=false)",
                       p2i((void*)obj), klass->external_name());
        }
        return false;
      }
    } else {
      Atomic::add(&_prep_fail_array, 1);
      if (Atomic::add(&_prep_fail_obj_array, 1) <= 3 && !_prep_diag_logged) {
        log_info(gc)("prepare_eviction: unknown array obj=" PTR_FORMAT
                     " klass=%s kept local",
                     p2i((void*)obj), klass->external_name());
      }
      return false;
    }
  }

  size_t word_size = obj->size_given_klass(klass);

  RemoteHandle* h = handle_for(obj);
  if (h == nullptr) h = create_handle_for(obj, hab);

  out->obj = obj;
  out->handle = h;
  out->klass = klass;
  out->word_size = word_size;
  out->slot_id = (size_t)-1;
  out->edge_table = nullptr;
  out->location_kind = RemoteLocationObjectSlot;
  out->location_flags = 0;
  out->segment_id = 0;
  out->segment_offset = 0;
  out->segment_byte_size = word_size * HeapWordSize;
  if (G1RemoteUseArrayChunkLocations &&
      klass->is_typeArray_klass() &&
      _backend != nullptr &&
      _backend->supports_segments()) {
    out->location_kind = RemoteLocationArrayChunk;
    out->segment_id = array_chunk_segment_id_for(h);
  }
  return true;
}

bool G1RemoteMemoryManager::finish_prepared_eviction(PreparedEviction* entry,
                                                     RemoteHandleAllocBuffer* hab) {
  if (entry == nullptr || entry->obj == nullptr || entry->handle == nullptr) {
    Atomic::add(&_prep_fail_null, 1);
    return false;
  }
  if (entry->edge_table != nullptr && entry->slot_id != (size_t)-1) {
    return true;
  }

  if (entry->location_kind == RemoteLocationArrayChunk) {
    return true;
  }

  if (entry->slot_id == (size_t)-1) {
    size_t slot_id = _backend->allocate_slot_id();
    if (slot_id == (size_t)-1) {
      Atomic::add(&_prep_fail_slot, 1);
      return false;
    }
    entry->slot_id = slot_id;
  }

  if (!finish_prepared_eviction_edges(entry, hab)) {
    entry->slot_id = (size_t)-1;
    return false;
  }

  return true;
}

bool G1RemoteMemoryManager::finish_prepared_eviction_edges(
    PreparedEviction* entry,
    RemoteHandleAllocBuffer* hab,
    HandleEntryAllocBuffer* eab,
    RemoteHandle** pending_head,
    RemoteHandle** pending_tail,
    size_t* pending_count,
    EdgeTableEntry** pending_edge_head,
    EdgeTableEntry** pending_edge_tail,
    size_t* pending_edge_count) {
  if (entry == nullptr || entry->obj == nullptr || entry->handle == nullptr) {
    Atomic::add(&_prep_fail_null, 1);
    return false;
  }
  if (entry->location_kind != RemoteLocationArrayChunk &&
      entry->slot_id == (size_t)-1) {
    Atomic::add(&_prep_fail_slot, 1);
    return false;
  }
  if (entry->edge_table != nullptr) {
    return true;
  }

  if (entry->location_kind == RemoteLocationArrayChunk) {
    Atomic::add(&_prep_success, 1);
    Atomic::add(&_prep_success_type_array, 1);
    return true;
  }

  if (entry->klass != nullptr && entry->klass->is_typeArray_klass()) {
    Atomic::add(&_prep_success, 1);
    Atomic::add(&_prep_success_type_array, 1);
    return true;
  }

  bool zero_edges = false;
  ObjectEdgeTable* et = build_edge_table(entry->obj, entry->handle, hab,
                                         &zero_edges, eab,
                                         pending_head, pending_tail,
                                         pending_count);
  if (et == nullptr) {
    if (zero_edges) {
      Atomic::add(&_prep_success, 1);
      if (entry->klass != nullptr && entry->klass->is_objArray_klass()) {
        Atomic::add(&_prep_success_obj_array, 1);
      }
      return true;
    }
    Atomic::add(&_prep_fail_edge, 1);
    return false;
  }

  entry->edge_table = et;
  if (pending_edge_head != nullptr && pending_edge_tail != nullptr &&
      pending_edge_count != nullptr) {
    append_pending_edge_table(et, pending_edge_head, pending_edge_tail,
                              pending_edge_count);
  } else {
    store_edge_table(et);
  }

  Atomic::add(&_prep_success, 1);
  if (entry->klass != nullptr && entry->klass->is_objArray_klass()) {
    Atomic::add(&_prep_success_obj_array, 1);
  }
  return true;
}

bool G1RemoteMemoryManager::prepare_eviction(oop obj, RemoteHandleAllocBuffer* hab,
                                             PreparedEviction* out) {
  if (!prepare_eviction_metadata(obj, hab, out)) {
    return false;
  }
  return finish_prepared_eviction(out, hab);
}

void G1RemoteMemoryManager::log_prepare_eviction_stats() {
  if (_prep_fail_null + _prep_fail_locked + _prep_fail_array +
      _prep_fail_filler + _prep_fail_edge + _prep_fail_slot + _prep_success > 0) {
    log_info(gc)("prepare_eviction stats: success=%d(type_array=%d obj_array=%d) null=%d "
                 "locked=%d array=%d(obj=%d type_disabled=%d) filler=%d edge=%d slot=%d",
                 _prep_success, _prep_success_type_array, _prep_success_obj_array,
                 _prep_fail_null, _prep_fail_locked, _prep_fail_array,
                 _prep_fail_obj_array, _prep_fail_type_array_disabled,
                 _prep_fail_filler, _prep_fail_edge, _prep_fail_slot);
    _prep_diag_logged = 1;
  }
  _prep_fail_null = _prep_fail_locked = _prep_fail_array =
      _prep_fail_obj_array = _prep_fail_type_array_disabled =
      _prep_fail_filler = _prep_fail_edge = _prep_fail_slot = _prep_success =
      _prep_success_type_array = _prep_success_obj_array = 0;
  _prep_diag_logged = 0;
}

void G1RemoteMemoryManager::finalize_eviction(PreparedEviction* entry) {
  entry->handle->set_eviction_word_size(entry->word_size);
  make_handle_remote(entry->handle, entry->slot_id);

  markWord mw = entry->obj->mark();
  if (mw.is_unlocked()) {
    entry->obj->set_mark(mw.set_remote_class(markWord::remote_class_shared));
  }
  HeapRegion* hr = _g1h->heap_region_containing(entry->obj);
  if (hr != nullptr) {
    hr->set_has_classified_objects();
    hr->set_had_remote_eviction_fillers();
  }

  CollectedHeap::fill_with_object(cast_from_oop<HeapWord*>(entry->obj), entry->word_size, false);
}

void G1RemoteMemoryManager::finalize_evictions(PreparedEviction* entries,
                                               int start,
                                               int count,
                                               HeapRegion* hr) {
  if (entries == nullptr || count <= 0) {
    return;
  }

  for (int e = start; e < start + count; e++) {
    PreparedEviction* entry = &entries[e];
    if (entry->location_kind != RemoteLocationArrayChunk ||
        entry->segment_id == 0 ||
        entry->segment_byte_size == 0) {
      continue;
    }

    bool first = true;
    uint32_t refs = 0;
    for (int f = start; f < start + count; f++) {
      if (entries[f].location_kind == RemoteLocationArrayChunk &&
          entries[f].segment_id == entry->segment_id) {
        if (f < e) {
          first = false;
          break;
        }
        refs++;
      }
    }
    if (first && refs > 0) {
      RemoteHandle** handles =
          (RemoteHandle**)os::malloc(sizeof(RemoteHandle*) * refs, mtGC);
      if (handles != nullptr) {
        uint32_t n = 0;
        for (int f = start; f < start + count && n < refs; f++) {
          if (entries[f].location_kind == RemoteLocationArrayChunk &&
              entries[f].segment_id == entry->segment_id) {
            handles[n++] = entries[f].handle;
          }
        }
        refs = n;
      } else {
        log_warning(gc)("Array chunk segment registry member allocation failed: "
                        "segment=" UINT64_FORMAT " refs=%u",
                        (uint64_t)entry->segment_id, refs);
      }
      register_array_chunk_segment((uint64_t)entry->segment_id, handles,
                                   refs, entry->segment_byte_size);
    }
  }

  local_handle_lock();
  for (int e = start; e < start + count; e++) {
    PreparedEviction* entry = &entries[e];
    RemoteHandle* h = entry->handle;
    if (h == nullptr) {
      continue;
    }
    h->set_eviction_word_size(entry->word_size);
    if (entry->location_kind == RemoteLocationArrayChunk) {
      h->set_remote_array_chunk((uintptr_t)entry->obj - entry->segment_offset,
                                entry->segment_id,
                                entry->segment_offset,
                                entry->word_size * HeapWordSize,
                                entry->segment_byte_size,
                                entry->location_flags);
    } else {
      h->set_remote(entry->slot_id);
    }
    unlink_local_handle_locked(h);
  }
  local_handle_unlock();

  if (hr != nullptr) {
    hr->set_has_classified_objects();
    hr->set_had_remote_eviction_fillers();
  }

  for (int e = start; e < start + count; e++) {
    PreparedEviction* entry = &entries[e];
    if (entry->obj == nullptr || entry->word_size == 0) {
      continue;
    }
    markWord mw = entry->obj->mark();
    if (mw.is_unlocked()) {
      entry->obj->set_mark(mw.set_remote_class(markWord::remote_class_shared));
    }

    CollectedHeap::fill_with_object(cast_from_oop<HeapWord*>(entry->obj),
                                    entry->word_size,
                                    false);
  }
}

void G1RemoteMemoryManager::abort_prepared_eviction(PreparedEviction* entry) {
  if (entry == nullptr) {
    return;
  }

  if (entry->location_kind == RemoteLocationArrayChunk &&
      entry->segment_id != 0 &&
      _backend != nullptr) {
    _backend->discard_segment((uint64_t)entry->segment_id);
  }

  if (entry->edge_table == nullptr) {
    return;
  }

  ObjectEdgeTable* et = entry->edge_table;
  for (uint32_t i = 0; i < et->_entry_count; i++) {
    RemoteHandle* target = et->_entries[i]._target_handle;
    if (target != nullptr) {
      target->decrement_remote_refcount();
    }
  }

  remove_edge_table(entry->handle);
  entry->edge_table = nullptr;
}

static bool prepared_entries_contain_handle(const G1RemoteMemoryManager::PreparedEviction* entries,
                                            int start,
                                            int count,
                                            uintptr_t addr,
                                            RemoteHandle* h) {
  int lo = start;
  int hi = start + count - 1;
  while (lo <= hi) {
    int mid = lo + ((hi - lo) >> 1);
    uintptr_t cur = cast_from_oop<uintptr_t>(entries[mid].obj);
    if (cur == addr) {
      for (int i = mid; i >= start && cast_from_oop<uintptr_t>(entries[i].obj) == addr; i--) {
        if (entries[i].handle == h) return true;
      }
      for (int i = mid + 1; i < start + count && cast_from_oop<uintptr_t>(entries[i].obj) == addr; i++) {
        if (entries[i].handle == h) return true;
      }
      return false;
    }
    if (cur < addr) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return false;
}

int G1RemoteMemoryManager::count_unprepared_local_handles_in_region(
    HeapRegion* hr,
    const PreparedEviction* entries,
    int start,
    int count,
    int log_limit) {
  if (hr == nullptr || entries == nullptr || count <= 0) {
    return 0;
  }

  class SingleRegionUnpreparedLocalHandleClosure {
    HeapRegion* _hr;
    const PreparedEviction* _entries;
    int _start;
    int _count;
    int _log_limit;
    uintptr_t _bottom;
    uintptr_t _end;
    int _blockers;

  public:
    SingleRegionUnpreparedLocalHandleClosure(HeapRegion* hr,
                                             const PreparedEviction* entries,
                                             int start,
                                             int count,
                                             int log_limit)
      : _hr(hr), _entries(entries), _start(start), _count(count),
        _log_limit(log_limit), _bottom((uintptr_t)hr->bottom()),
        _end((uintptr_t)hr->end()), _blockers(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) {
        uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
        if (addr >= _bottom && addr < _end &&
            !prepared_entries_contain_handle(_entries, _start, _count, addr, h)) {
          _blockers++;
          if (_blockers <= _log_limit) {
            log_warning(gc)("Pre-E local-handle guard: region=%u blocker handle="
                            PTR_FORMAT " local=" PTR_FORMAT
                            " listed=%d dormant=%d rc=%u",
                            _hr->hrm_index(), p2i(h), addr,
                            h->_local_listed ? 1 : 0,
                            h->is_dormant() ? 1 : 0, h->remote_refcount());
          }
        }
      }
    }

    int blockers() const { return _blockers; }
  };

  SingleRegionUnpreparedLocalHandleClosure cl(hr, entries, start, count, log_limit);
  _handle_allocator.handles_do(&cl);
  return cl.blockers();
}

int G1RemoteMemoryManager::count_unprepared_local_handles_in_regions(
    const bool* eviction_candidates,
    const bool* region_complete,
    const int* region_start,
    const int* region_count,
    uint num_regions,
    const PreparedEviction* entries,
    int* blockers_by_region,
    int log_limit) {
  if (eviction_candidates == nullptr || region_complete == nullptr ||
      region_start == nullptr || region_count == nullptr ||
      entries == nullptr || blockers_by_region == nullptr || num_regions == 0) {
    return 0;
  }

  memset(blockers_by_region, 0, num_regions * sizeof(int));
  class UnpreparedLocalHandleClosure {
    G1RemoteMemoryManager* _rmm;
    const bool* _eviction_candidates;
    const bool* _region_complete;
    const int* _region_start;
    const int* _region_count;
    uint _num_regions;
    const PreparedEviction* _entries;
    int* _blockers_by_region;
    int _log_limit;
    int _total_blockers;

  public:
    UnpreparedLocalHandleClosure(G1RemoteMemoryManager* rmm,
                                 const bool* eviction_candidates,
                                 const bool* region_complete,
                                 const int* region_start,
                                 const int* region_count,
                                 uint num_regions,
                                 const PreparedEviction* entries,
                                 int* blockers_by_region,
                                 int log_limit)
      : _rmm(rmm), _eviction_candidates(eviction_candidates),
        _region_complete(region_complete), _region_start(region_start),
        _region_count(region_count), _num_regions(num_regions),
        _entries(entries), _blockers_by_region(blockers_by_region),
        _log_limit(log_limit), _total_blockers(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) {
        uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
        if (addr != 0 && _rmm->_g1h->is_in((void*)addr)) {
          HeapRegion* hr = _rmm->_g1h->heap_region_containing((void*)addr);
          if (hr != nullptr) {
            uint ridx = hr->hrm_index();
            if (ridx < _num_regions && _eviction_candidates[ridx] &&
                _region_complete[ridx] && _region_count[ridx] > 0 &&
                !prepared_entries_contain_handle(_entries, _region_start[ridx],
                                                 _region_count[ridx], addr, h)) {
              int blockers = ++_blockers_by_region[ridx];
              _total_blockers++;
              if (blockers <= _log_limit) {
                log_warning(gc)("Pre-E local-handle guard: region=%u blocker handle="
                                PTR_FORMAT " local=" PTR_FORMAT
                                " listed=%d dormant=%d rc=%u",
                                ridx, p2i(h), addr,
                                h->_local_listed ? 1 : 0,
                                h->is_dormant() ? 1 : 0, h->remote_refcount());
              }
            }
          }
        }
      }
    }

    int total_blockers() const { return _total_blockers; }
  };

  UnpreparedLocalHandleClosure cl(this, eviction_candidates, region_complete,
                                  region_start, region_count, num_regions,
                                  entries, blockers_by_region, log_limit);
  _handle_allocator.handles_do(&cl);
  return cl.total_blockers();
}

int G1RemoteMemoryManager::collect_remote_anchor_addrs_in_regions(const bool* region_set,
                                                                  uint num_regions,
                                                                  uintptr_t* addrs,
                                                                  int max_addrs,
                                                                  bool* overflow) {
  if (overflow != nullptr) *overflow = false;
  if (region_set == nullptr || num_regions == 0) return 0;

  int count = 0;
  int stale_handles = 0;

  if (_local_handle_region_heads != nullptr &&
      _local_handle_region_counts != nullptr &&
      _local_handle_region_capacity >= num_regions) {
    for (uint idx = 0; idx < num_regions && idx < _local_handle_region_capacity; idx++) {
      if (!region_set[idx]) continue;

      RemoteHandle* cur = _local_handle_region_heads[idx];
      while (cur != nullptr) {
        RemoteHandle* next = cur->_region_next;
        uintptr_t cur_sa = cur->load_state_and_addr_acquire();
        if ((cur_sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL &&
            cur->remote_refcount() > 0 &&
            validate_local_handle_addr(cur, "ANCHOR-COLLECT-REGION",
                                       &stale_handles, 8)) {
          uintptr_t sa = cur->load_state_and_addr_acquire();
          uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
          uint ridx = local_handle_region_index(addr);
          if (ridx < num_regions && region_set[ridx]) {
            if (count < max_addrs && addrs != nullptr) {
              addrs[count] = addr;
            } else if (overflow != nullptr) {
              *overflow = true;
            }
            count++;
          }
        }
        cur = next;
      }
    }

    for (int i = 0; i < _cross_roots_count; i++) {
      RemoteHandle* h = _cross_roots[i];
      if (h == nullptr || !h->is_local()) continue;
      if (!validate_local_handle_addr(h, "CROSS-ROOT-COLLECT",
                                      &stale_handles, 8)) {
        continue;
      }

      uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
      if (addr == 0 || !_g1h->is_in((void*)addr)) continue;

      HeapRegion* hr = _g1h->heap_region_containing((void*)addr);
      if (hr == nullptr) continue;
      uint ridx = hr->hrm_index();
      if (ridx >= num_regions || !region_set[ridx]) continue;

      if (count < max_addrs && addrs != nullptr) {
        addrs[count] = addr;
      } else if (overflow != nullptr) {
        *overflow = true;
      }
      count++;
    }

    if (stale_handles > 8) {
      log_warning(gc)("Remote anchor collection marked %d stale LOCAL handles DEAD "
                      "(logged first 8)", stale_handles);
    }
    return count;
  }

  class RemoteAnchorCollectClosure {
    G1RemoteMemoryManager* _rmm;
    const bool* _region_set;
    uint _num_regions;
    uintptr_t* _addrs;
    int _max_addrs;
    bool* _overflow;
    int _count;
    int _stale_handles;

  public:
    RemoteAnchorCollectClosure(G1RemoteMemoryManager* rmm,
                               const bool* region_set,
                               uint num_regions,
                               uintptr_t* addrs,
                               int max_addrs,
                               bool* overflow)
      : _rmm(rmm), _region_set(region_set), _num_regions(num_regions),
        _addrs(addrs), _max_addrs(max_addrs), _overflow(overflow),
        _count(0), _stale_handles(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;
      if (!h->is_local() || h->remote_refcount() == 0) return;

      if (!_rmm->validate_local_handle_addr(h, "ANCHOR-COLLECT",
                                            &_stale_handles, 8)) {
        return;
      }

      uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
      if (addr != 0 && _rmm->_g1h->is_in((void*)addr)) {
        HeapRegion* hr = _rmm->_g1h->heap_region_containing((void*)addr);
        uint ridx = hr == nullptr ? _num_regions : hr->hrm_index();
        if (ridx < _num_regions && _region_set[ridx]) {
          if (_count < _max_addrs && _addrs != nullptr) {
            _addrs[_count] = addr;
          } else if (_overflow != nullptr) {
            *_overflow = true;
          }
          _count++;
        }
      }
    }

    int count() const { return _count; }
    int stale_handles() const { return _stale_handles; }
  };

  RemoteAnchorCollectClosure cl(this, region_set, num_regions, addrs,
                                max_addrs, overflow);
  _handle_allocator.handles_do(&cl);
  count = cl.count();
  stale_handles = cl.stale_handles();

  for (int i = 0; i < _cross_roots_count; i++) {
    RemoteHandle* h = _cross_roots[i];
    if (h == nullptr || !h->is_local()) continue;
    if (!validate_local_handle_addr(h, "CROSS-ROOT-COLLECT",
                                    &stale_handles, 8)) {
      continue;
    }

    uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
    if (addr == 0 || !_g1h->is_in((void*)addr)) continue;

    HeapRegion* hr = _g1h->heap_region_containing((void*)addr);
    if (hr == nullptr) continue;
    uint ridx = hr->hrm_index();
    if (ridx >= num_regions || !region_set[ridx]) continue;

    if (count < max_addrs && addrs != nullptr) {
      addrs[count] = addr;
    } else if (overflow != nullptr) {
      *overflow = true;
    }
    count++;
  }
  if (stale_handles > 8) {
    log_warning(gc)("Remote anchor collection marked %d stale LOCAL handles DEAD "
                    "(logged first 8)", stale_handles);
  }

  return count;
}

int G1RemoteMemoryManager::mark_remote_anchor_regions_in_set(const bool* region_set,
                                                             uint num_regions,
                                                             bool* anchor_regions,
                                                             int* anchors_seen,
                                                             const bool* region_set2,
                                                             bool* anchor_regions2,
                                                             int* marked_regions2,
                                                             int* anchors_seen2) {
  if (anchors_seen != nullptr) {
    *anchors_seen = 0;
  }
  if (marked_regions2 != nullptr) {
    *marked_regions2 = 0;
  }
  if (anchors_seen2 != nullptr) {
    *anchors_seen2 = 0;
  }
  if (region_set == nullptr || anchor_regions == nullptr || num_regions == 0) {
    return 0;
  }

  int marked_regions = 0;
  int seen = 0;
  int marked2 = 0;
  int seen2 = 0;
  int stale_handles = 0;

  if (_local_handle_region_heads != nullptr &&
      _local_handle_region_counts != nullptr &&
      _local_handle_region_capacity >= num_regions) {
    for (uint idx = 0; idx < num_regions && idx < _local_handle_region_capacity; idx++) {
      bool scan_region = region_set[idx] ||
          (region_set2 != nullptr && region_set2[idx]);
      if (!scan_region) continue;

      RemoteHandle* cur = _local_handle_region_heads[idx];
      while (cur != nullptr) {
        RemoteHandle* next = cur->_region_next;
        uintptr_t cur_sa = cur->load_state_and_addr_acquire();
        if ((cur_sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL &&
            cur->remote_refcount() > 0 &&
            validate_local_handle_addr(cur, "ANCHOR-MARK-REGION",
                                       &stale_handles, 8)) {
          uintptr_t addr = cur->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
          uint ridx = local_handle_region_index(addr);
          if (ridx < num_regions && region_set[ridx]) {
            seen++;
            if (!anchor_regions[ridx]) {
              anchor_regions[ridx] = true;
              marked_regions++;
            }
          }
          if (region_set2 != nullptr && anchor_regions2 != nullptr &&
              ridx < num_regions && region_set2[ridx]) {
            seen2++;
            if (!anchor_regions2[ridx]) {
              anchor_regions2[ridx] = true;
              marked2++;
            }
          }
        }
        cur = next;
      }
    }

    for (int i = 0; i < _cross_roots_count; i++) {
      RemoteHandle* h = _cross_roots[i];
      if (h == nullptr || !h->is_local()) continue;
      if (!validate_local_handle_addr(h, "CROSS-ROOT-MARK",
                                      &stale_handles, 8)) {
        continue;
      }

      uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
      if (addr == 0 || !_g1h->is_in((void*)addr)) continue;

      HeapRegion* hr = _g1h->heap_region_containing((void*)addr);
      if (hr == nullptr) continue;
      uint ridx = hr->hrm_index();
      if (ridx < num_regions && region_set[ridx]) {
        seen++;
        if (!anchor_regions[ridx]) {
          anchor_regions[ridx] = true;
          marked_regions++;
        }
      }
      if (region_set2 != nullptr && anchor_regions2 != nullptr &&
          ridx < num_regions && region_set2[ridx]) {
        seen2++;
        if (!anchor_regions2[ridx]) {
          anchor_regions2[ridx] = true;
          marked2++;
        }
      }
    }

    if (anchors_seen != nullptr) {
      *anchors_seen = seen;
    }
    if (marked_regions2 != nullptr) {
      *marked_regions2 = marked2;
    }
    if (anchors_seen2 != nullptr) {
      *anchors_seen2 = seen2;
    }
    if (stale_handles > 8) {
      log_warning(gc)("Remote anchor marking marked %d stale LOCAL handles DEAD "
                      "(logged first 8)", stale_handles);
    }
    return marked_regions;
  }

  class RemoteAnchorMarkClosure {
    G1RemoteMemoryManager* _rmm;
    const bool* _region_set;
    uint _num_regions;
    bool* _anchor_regions;
    const bool* _region_set2;
    bool* _anchor_regions2;
    int _marked_regions;
    int _seen;
    int _marked_regions2;
    int _seen2;
    int _stale_handles;

    void maybe_mark(uint ridx, const bool* region_set, bool* anchor_regions,
                    int* seen, int* marked_regions) {
      if (region_set == nullptr || anchor_regions == nullptr) return;
      if (ridx >= _num_regions || !region_set[ridx]) return;

      (*seen)++;
      if (!anchor_regions[ridx]) {
        anchor_regions[ridx] = true;
        (*marked_regions)++;
      }
    }

  public:
    RemoteAnchorMarkClosure(G1RemoteMemoryManager* rmm,
                            const bool* region_set,
                            uint num_regions,
                            bool* anchor_regions,
                            const bool* region_set2,
                            bool* anchor_regions2)
      : _rmm(rmm), _region_set(region_set), _num_regions(num_regions),
        _anchor_regions(anchor_regions),
        _region_set2(region_set2), _anchor_regions2(anchor_regions2),
        _marked_regions(0), _seen(0), _marked_regions2(0), _seen2(0),
        _stale_handles(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;
      if (!h->is_local() || h->remote_refcount() == 0) return;

      if (!_rmm->validate_local_handle_addr(h, "ANCHOR-MARK",
                                            &_stale_handles, 8)) {
        return;
      }

      uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
      if (addr == 0 || !_rmm->_g1h->is_in((void*)addr)) return;

      HeapRegion* hr = _rmm->_g1h->heap_region_containing((void*)addr);
      if (hr == nullptr) return;
      uint ridx = hr->hrm_index();
      maybe_mark(ridx, _region_set, _anchor_regions,
                 &_seen, &_marked_regions);
      maybe_mark(ridx, _region_set2, _anchor_regions2,
                 &_seen2, &_marked_regions2);
    }

    int marked_regions() const { return _marked_regions; }
    int seen() const { return _seen; }
    int marked_regions2() const { return _marked_regions2; }
    int seen2() const { return _seen2; }
    int stale_handles() const { return _stale_handles; }
  };

  RemoteAnchorMarkClosure cl(this, region_set, num_regions, anchor_regions,
                             region_set2, anchor_regions2);
  _handle_allocator.handles_do(&cl);
  marked_regions = cl.marked_regions();
  seen = cl.seen();
  marked2 = cl.marked_regions2();
  seen2 = cl.seen2();
  stale_handles = cl.stale_handles();

  for (int i = 0; i < _cross_roots_count; i++) {
    RemoteHandle* h = _cross_roots[i];
    if (h == nullptr || !h->is_local()) continue;
    if (!validate_local_handle_addr(h, "CROSS-ROOT-MARK",
                                    &stale_handles, 8)) {
      continue;
    }

    uintptr_t addr = h->load_state_and_addr_acquire() & REMOTE_HANDLE_ADDR_MASK;
    if (addr == 0 || !_g1h->is_in((void*)addr)) continue;

    HeapRegion* hr = _g1h->heap_region_containing((void*)addr);
    if (hr == nullptr) continue;
    uint ridx = hr->hrm_index();
    if (ridx < num_regions && region_set[ridx]) {
      seen++;
      if (!anchor_regions[ridx]) {
        anchor_regions[ridx] = true;
        marked_regions++;
      }
    }
    if (region_set2 != nullptr && anchor_regions2 != nullptr &&
        ridx < num_regions && region_set2[ridx]) {
      seen2++;
      if (!anchor_regions2[ridx]) {
        anchor_regions2[ridx] = true;
        marked2++;
      }
    }
  }

  if (anchors_seen != nullptr) {
    *anchors_seen = seen;
  }
  if (marked_regions2 != nullptr) {
    *marked_regions2 = marked2;
  }
  if (anchors_seen2 != nullptr) {
    *anchors_seen2 = seen2;
  }
  if (stale_handles > 8) {
    log_warning(gc)("Remote anchor marking marked %d stale LOCAL handles DEAD "
                    "(logged first 8)", stale_handles);
  }

  return marked_regions;
}

// ============================================================
// Region-Granularity Eviction
// ============================================================
// Evicts ALL objects in a region to remote. For each object:
//   1. Create Handle + build edge table (dormant anchors for outgoing refs)
//   2. Backend evict (send bytes via TCP/RDMA/SIM)
//   3. Handle → REMOTE + fill with filler
// After all objects evicted: tag incoming refs, free the region.

// Closure to tag incoming refs on a specific card range pointing into the evicted region.
class IncomingRefTagClosure : public BasicOopIterateClosure {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  HeapRegion*            _target_hr;
  int                    _tagged;
  bool                   _has_untaggable; // narrow oop or other untaggable ref found
public:
  IncomingRefTagClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h, HeapRegion* target)
    : _rmm(rmm), _g1h(g1h), _target_hr(target), _tagged(0), _has_untaggable(false) {}

  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;

    // Shared oops (bits 63+62) already go through a Handle — skip.
    if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
        (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

    // Resolve: strip Unique tag bits if present to get raw address.
    oop target;
    if ((raw >> 63) != 0) {
      target = cast_to_oop(raw & G1_OOP_ADDR_MASK);
    } else {
      target = cast_to_oop(raw);
    }
    if (!_g1h->is_in(target)) return;

    HeapRegion* target_region = _g1h->heap_region_containing(target);
    if (target_region != _target_hr) return;

    RemoteHandle* h = _rmm->handle_for(target);
    if (h != nullptr) {
      *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
      _tagged++;
    }
  }

  virtual void do_oop(narrowOop* p) {
    // Can't tag narrow oops — flag as untaggable
    narrowOop v = *p;
    if (!CompressedOops::is_null(v)) {
      oop target = CompressedOops::decode(v);
      if (_g1h->is_in(target)) {
        HeapRegion* target_region = _g1h->heap_region_containing(target);
        if (target_region == _target_hr) {
          _has_untaggable = true;
        }
      }
    }
  }

  int tagged() const { return _tagged; }
  bool has_untaggable() const { return _has_untaggable; }
};

// Remset visitor that collects card indices for a target region.
// Used to find which cards in source regions contain refs into the target.
class RemsetCardCollector {
  G1CollectedHeap* _g1h;
  G1CardTable*     _ct;
  HeapRegion*      _target_hr;
  G1RemoteMemoryManager* _rmm;
  int              _tagged;
  bool             _has_untaggable;

public:
  RemsetCardCollector(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h, HeapRegion* target)
    : _g1h(g1h), _ct(g1h->card_table()), _target_hr(target),
      _rmm(rmm), _tagged(0), _has_untaggable(false) {}

  bool start_iterate(uint tag, uint region_idx) {
    // Accept all source regions
    return true;
  }

  void do_card(uint card_idx) {
    scan_card(card_idx, 1);
  }

  void do_card_range(uint start_card_idx, uint length) {
    scan_card(start_card_idx, length);
  }

  int tagged() const { return _tagged; }
  bool has_untaggable() const { return _has_untaggable; }

private:
  void scan_card(uint card_idx, uint length) {
    // Convert card index to memory region
    HeapWord* card_start = _ct->addr_for((G1CardTable::CardValue*)(_ct->byte_for_index(card_idx)));
    HeapWord* card_end = card_start + length * G1CardTable::card_size_in_words();

    // Find the source region
    if (!_g1h->is_in(card_start)) return;
    HeapRegion* source_hr = _g1h->heap_region_containing(card_start);
    if (source_hr == nullptr || source_hr == _target_hr) return;

    // Clip to region bounds
    HeapWord* scan_start = MAX2(card_start, source_hr->bottom());
    HeapWord* scan_end = MIN2(card_end, source_hr->top());
    if (scan_start >= scan_end) return;

    // Scan objects overlapping this card range for refs into target region
    IncomingRefTagClosure cl(_rmm, _g1h, _target_hr);
    MemRegion mr(scan_start, scan_end);
    source_hr->oops_on_memregion_seq_iterate_careful<true>(mr, &cl);

    _tagged += cl.tagged();
    if (cl.has_untaggable()) _has_untaggable = true;
  }
};

void G1RemoteMemoryManager::tag_incoming_refs_to_region(HeapRegion* target_hr) {
  HeapRegionRemSet* rem_set = target_hr->rem_set();

  if (rem_set->is_complete() && !rem_set->is_empty()) {
    // Remset complete — use efficient remset-based scan
    RemsetCardCollector collector(this, _g1h, target_hr);
    rem_set->iterate_for_merge(collector);

    if (collector.has_untaggable()) {
      log_info(gc)("Region %u has untaggable incoming refs — pinning", target_hr->hrm_index());
      target_hr->set_root_pinned();
      return;
    }

    if (collector.tagged() > 0) {
      log_info(gc)("Tagged %d incoming refs to region %u via remset",
                   collector.tagged(), target_hr->hrm_index());
    }
  } else {
    // Remset not complete — cannot safely find all incoming refs.
    // Skip this region for eviction.
    log_debug(gc)("Remset incomplete for region %u — skipping eviction",
                  target_hr->hrm_index());
    target_hr->set_root_pinned();  // prevent eviction
  }
}

// Full heap scan: tag ALL heap refs pointing to any eviction candidate.
// Walks every non-candidate, non-empty region and checks each oop field.
// O(live_heap) but parallelized across GC workers. Catches refs that remset
// misses (dirty cards not yet refined, post-evacuation card dirtying, etc.).

class EvictionSetTagClosure : public BasicOopIterateClosure {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  const bool*            _eviction_set;
  uint                   _num_regions;
  int                    _tagged;
  int                    _no_handle;
  int                    _untaggable;
  int                    _untaggable_reports_left;
  oop                    _cur_obj;
  bool                   _allow_unknown_heap_source;
  RemoteHandleAllocBuffer _hab;

  typedef G1RemoteMemoryManager::TaggedFieldEntry TaggedFieldEntry;
  TaggedFieldEntry* _local_buf;
  int               _local_count;
  int               _local_capacity;

  void local_buf_add_entry(const TaggedFieldEntry& entry) {
    if (_local_count >= _local_capacity) {
      int new_cap = (_local_capacity == 0) ? 4096 : _local_capacity * 2;
      TaggedFieldEntry* nb = NEW_C_HEAP_ARRAY(TaggedFieldEntry, new_cap, mtGC);
      if (_local_buf != nullptr) {
        memcpy(nb, _local_buf, _local_count * sizeof(TaggedFieldEntry));
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _local_buf);
      }
      _local_buf = nb;
      _local_capacity = new_cap;
    }
    _local_buf[_local_count++] = entry;
  }

  void local_buf_add(oop* field_addr, RemoteHandle* h) {
    TaggedFieldEntry entry;
    entry._field_addr = field_addr;
    entry._handle = h;
    entry._tagged_raw = 0;
    entry._kind = G1RemoteMemoryManager::TaggedFieldHandle;
    local_buf_add_entry(entry);
  }

  void remember_old_cset_source(oop* p, const char* reason) {
    if (p == nullptr) {
      return;
    }
    HeapRegion* src_hr = nullptr;
    if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
      src_hr = _g1h->heap_region_containing_or_null(_cur_obj);
    }
    if (src_hr == nullptr && _g1h->is_in((void*)p)) {
      src_hr = _g1h->heap_region_containing_or_null((void*)p);
    }
    if (src_hr == nullptr || src_hr->is_empty() || src_hr->is_free() ||
        src_hr->is_young() || src_hr->is_continues_humongous()) {
      return;
    }
    _rmm->remember_old_cset_source_hint(src_hr->hrm_index(), reason);
  }

  bool heap_source_allows_handle_slot(Klass* target_klass,
                                      Klass** source_klass_out,
                                      const char** source_reason_out) {
    Klass* source_klass = (_cur_obj != nullptr) ? _cur_obj->klass_or_null() : nullptr;
    if (source_klass != nullptr && !remote_eviction_valid_klass(source_klass)) {
      source_klass = nullptr;
    }

    bool object_array_source =
        source_klass != nullptr && source_klass->is_objArray_klass();
    bool target_type_array =
        target_klass != nullptr && target_klass->is_typeArray_klass();
    bool target_object =
        target_klass != nullptr && !target_klass->is_array_klass();
    bool unsafe_obj_array_source =
        object_array_source &&
        ((!target_type_array || !G1RemoteTagObjArraySources) &&
         (!target_object || !G1RemoteTagObjArrayObjectSources));
    bool unknown_source = _cur_obj == nullptr && !_allow_unknown_heap_source;
    bool untaggable_source = unknown_source ||
        (source_klass != nullptr && source_klass->is_array_klass() &&
         (!object_array_source || unsafe_obj_array_source));

    if (source_klass_out != nullptr) {
      *source_klass_out = source_klass;
    }
    if (source_reason_out != nullptr) {
      *source_reason_out = unknown_source ? "unknown" : "untaggable-array";
    }
    return !untaggable_source;
  }

public:
  EvictionSetTagClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                        const bool* eset, uint nregions,
                        bool allow_unknown_heap_source = false)
    : _rmm(rmm), _g1h(g1h), _eviction_set(eset),
      _num_regions(nregions), _tagged(0), _no_handle(0),
      _untaggable(0), _untaggable_reports_left(10), _cur_obj(nullptr),
      _allow_unknown_heap_source(allow_unknown_heap_source), _hab(),
      _local_buf(nullptr), _local_count(0), _local_capacity(0) {}

  ~EvictionSetTagClosure() {
    // Don't free _local_buf here — caller takes ownership via release_local_buf()
  }

  TaggedFieldEntry* release_local_buf() {
    TaggedFieldEntry* buf = _local_buf;
    _local_buf = nullptr;
    return buf;
  }

  void set_cur_obj(oop obj) { _cur_obj = obj; }

  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;
    if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
        (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

    oop target;
    if ((raw >> 63) != 0) {
      target = cast_to_oop(raw & G1_OOP_ADDR_MASK);
    } else {
      target = cast_to_oop(raw);
    }
    if (!_g1h->is_in(target)) return;

    HeapRegion* target_hr = _g1h->heap_region_containing(target);
    uint idx = target_hr->hrm_index();
    if (idx >= _num_regions || !_eviction_set[idx]) return;

    Klass* target_klass = nullptr;
    if (!remote_eviction_valid_local_oop(_g1h, target, target_hr, &target_klass)) {
      _untaggable++;
      if (_untaggable_reports_left > 0) {
        log_warning(gc)("Tagging: kept raw ref to invalid target field="
                        PTR_FORMAT " raw=0x%lx -> target=" PTR_FORMAT
                        " in candidate region %u",
                        p2i(p), (unsigned long)raw, p2i((void*)target), idx);
        _untaggable_reports_left--;
      }
      return;
    }

    bool heap_source = _g1h->is_in((void*)p);

    // Evacuation or an earlier guarded path installed a forwarding pointer.
    // Redirect the reference to the current object address. If this is a
    // heap slot that can safely hold a remote Handle, canonicalize directly
    // to the HIT representation instead of preserving a legacy direct tag.
    if (target->is_forwarded()) {
      oop fwd = target->forwardee();
      if (fwd == nullptr || !_g1h->is_in(fwd)) {
        _untaggable++;
        if (_untaggable_reports_left > 0) {
          log_warning(gc)("Tagging: kept raw ref to invalid forwarded target field="
                          PTR_FORMAT " raw=0x%lx -> target=" PTR_FORMAT
                          " in candidate region %u",
                          p2i(p), (unsigned long)raw, p2i((void*)fwd), idx);
          _untaggable_reports_left--;
        }
        return;
      }

      bool handle_slot_allowed =
          heap_source && heap_source_allows_handle_slot(target_klass, nullptr, nullptr);
      if (handle_slot_allowed && remote_handle_managed_local_oop(_g1h, fwd)) {
        RemoteHandle* h = _rmm->handle_for(fwd);
        if (h == nullptr) {
          h = _rmm->ensure_handle_for(fwd, &_hab);
        }
        if (h != nullptr) {
          *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
          local_buf_add(p, h);
          remember_old_cset_source(p, "phase-c-forwarded-handle");
          _tagged++;
          return;
        }
        _no_handle++;
        if (_no_handle <= 10) {
          log_warning(gc)("Tagging: no handle for forwarded target " PTR_FORMAT
                          " in candidate region %u (field at " PTR_FORMAT ")",
                          p2i((void*)fwd), idx, p2i(p));
        }
      }

      uintptr_t new_addr = cast_from_oop<uintptr_t>(fwd) & G1_OOP_ADDR_MASK;
      *(uintptr_t*)p = new_addr;
      _tagged++;
      return;
    }

    if (!heap_source) {
      // Non-heap root slots (thread stacks, JNI handles, OopStorage, CLD
      // handles) are not stable enough to keep in the persistent tagged-field
      // side list. Let the normal Phase-C untaggable abort path keep the
      // candidate region local instead of leaving a tagged root behind.
      _untaggable++;
      if (_untaggable_reports_left > 0) {
        log_warning(gc)("Tagging: kept raw ref from non-heap root field="
                        PTR_FORMAT " -> target=" PTR_FORMAT
                        " in candidate region %u",
                        p2i(p), p2i((void*)target), idx);
        _untaggable_reports_left--;
      }
      return;
    }

    {
      Klass* source_klass = nullptr;
      const char* source_reason = nullptr;
      if (!heap_source_allows_handle_slot(target_klass, &source_klass, &source_reason)) {
        _untaggable++;
        if (_untaggable_reports_left > 0) {
          log_warning(gc)("Tagging: kept raw ref from %s heap source field=" PTR_FORMAT
                          " -> target=" PTR_FORMAT " in candidate region %u "
                          "(src_obj=" PTR_FORMAT " src_klass=%s)",
                          source_reason != nullptr ? source_reason : "untaggable",
                          p2i(p), p2i((void*)target), idx, p2i((void*)_cur_obj),
                          source_klass != nullptr ? source_klass->external_name() : "unknown");
          _untaggable_reports_left--;
        }
        return;
      }
    }

    RemoteHandle* h = _rmm->handle_for(target);
    if (h == nullptr) {
      h = _rmm->ensure_handle_for(target, &_hab);
    }
    if (h != nullptr) {
      *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
      local_buf_add(p, h);
      remember_old_cset_source(p, "phase-c-handle");
      _tagged++;
    } else {
      _no_handle++;
      if (_no_handle <= 10) {
        log_warning(gc)("Tagging: no handle for target " PTR_FORMAT " in candidate region %u "
                        "(field at " PTR_FORMAT ")",
                        p2i((void*)target), idx, p2i(p));
      }
    }
  }

  virtual void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }

  int tagged() const { return _tagged; }
  int no_handle() const { return _no_handle; }
  int untaggable() const { return _untaggable; }
  const TaggedFieldEntry* local_buf() const { return _local_buf; }
  int local_count() const { return _local_count; }
};

static bool remote_eviction_valid_klass(Klass* k) {
  if (k == nullptr) return false;
  if ((uintptr_t)k < os::min_page_size()) return false;
  if (!is_aligned((address)k, sizeof(MetaWord))) return false;
  if (!Metaspace::contains(k)) return false;
  return k->is_klass();
}

static bool remote_eviction_valid_local_oop(G1CollectedHeap* g1h,
                                            oop obj,
                                            HeapRegion* hr,
                                            Klass** klass_out) {
  if (klass_out != nullptr) {
    *klass_out = nullptr;
  }
  if (g1h == nullptr || obj == nullptr) {
    return false;
  }
  HeapWord* addr = cast_from_oop<HeapWord*>(obj);
  if (!is_object_aligned((void*)addr) ||
      !g1h->is_in_reserved(obj) ||
      !g1h->is_in(obj)) {
    return false;
  }
  HeapRegion* actual_hr = g1h->heap_region_containing_or_null(obj);
  if (actual_hr == nullptr || (hr != nullptr && actual_hr != hr)) {
    return false;
  }
  hr = actual_hr;
  if (hr->is_free() || hr->is_empty() || hr->is_evict_guarded() ||
      hr->is_continues_humongous()) {
    return false;
  }
  if (addr < hr->bottom() || addr >= hr->top()) {
    return false;
  }

  Klass* k = obj->klass_or_null_acquire();
  if (!remote_eviction_valid_klass(k)) {
    return false;
  }
  // Do not call HeapRegion::block_start() here. Most callers are validating
  // conservative values read from object fields after remote-memory tagging;
  // G1's block-start path may parse interior primitive-array payload before
  // proving the address is a real object boundary.
  if (hr->is_starts_humongous() && addr != hr->bottom()) {
    return false;
  }
  if (G1CollectedHeap::is_obj_filler(obj)) {
    return false;
  }
  Klass* size_k = obj->klass_or_null_acquire();
  if (size_k != k) {
    return false;
  }
  size_t word_size = obj->size_given_klass(size_k);
  if (word_size < (size_t)MinObjAlignment ||
      !is_object_aligned(word_size) ||
      word_size > (size_t)(hr->top() - addr) ||
      word_size > (size_t)(hr->end() - addr)) {
    return false;
  }
  if (klass_out != nullptr) {
    *klass_out = k;
  }
  return true;
}

size_t G1RemoteMemoryManager::molecule_profile_klass_hash(Klass* klass) const {
  return ((uintptr_t)klass >> 4) % _molecule_profile_capacity;
}

size_t G1RemoteMemoryManager::molecule_profile_edge_hash(Klass* from,
                                                         Klass* to) const {
  uintptr_t h = ((uintptr_t)from >> 4) ^ (((uintptr_t)to >> 4) * 1103515245u);
  return h % _molecule_profile_capacity;
}

void G1RemoteMemoryManager::record_molecule_profile_klass_locked(
    Klass* klass,
    uint64_t old_copies,
    uint64_t old_copy_bytes,
    uint64_t out_edges,
    uint64_t mutation_overwrites) {
  if (klass == nullptr || _molecule_klass_profile == nullptr ||
      _molecule_profile_capacity == 0) {
    return;
  }

  size_t idx = molecule_profile_klass_hash(klass);
  for (uint probe = 0; probe < _molecule_profile_capacity; probe++) {
    MoleculeKlassProfileEntry* e =
        &_molecule_klass_profile[(idx + probe) % _molecule_profile_capacity];
    if (e->_klass == nullptr) {
      e->_klass = klass;
    }
    if (e->_klass == klass) {
      e->_old_copies += old_copies;
      e->_old_copy_bytes += old_copy_bytes;
      e->_out_edges += out_edges;
      e->_mutation_overwrites += mutation_overwrites;
      return;
    }
  }
  _molecule_profile_dropped_klass++;
}

void G1RemoteMemoryManager::record_molecule_profile_edge_locked(Klass* from,
                                                                Klass* to,
                                                                bool array_source,
                                                                bool mutation) {
  if (from == nullptr || to == nullptr || _molecule_edge_profile == nullptr ||
      _molecule_profile_capacity == 0) {
    return;
  }

  size_t idx = molecule_profile_edge_hash(from, to);
  for (uint probe = 0; probe < _molecule_profile_capacity; probe++) {
    MoleculeEdgeProfileEntry* e =
        &_molecule_edge_profile[(idx + probe) % _molecule_profile_capacity];
    if (e->_from == nullptr) {
      e->_from = from;
      e->_to = to;
    }
    if (e->_from == from && e->_to == to) {
      if (mutation) {
        e->_mutation_overwrites++;
      } else {
        e->_promotion_edges++;
        if (array_source) {
          e->_array_source_edges++;
        }
      }
      return;
    }
  }
  _molecule_profile_dropped_edges++;
}

static Klass* molecule_profile_klass_for_raw(G1CollectedHeap* g1h,
                                             uintptr_t raw) {
  if (g1h == nullptr || raw == 0) {
    return nullptr;
  }
  if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
      (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
    return nullptr;
  }

  uintptr_t addr = (raw & G1_OOP_TAG_MASK) != 0 ? (raw & G1_OOP_ADDR_MASK) : raw;
  if (!is_aligned((address)addr, HeapWordSize) ||
      !g1h->is_in_reserved((void*)addr)) {
    return nullptr;
  }

  HeapRegion* hr = g1h->heap_region_containing_or_null((void*)addr);
  Klass* klass = nullptr;
  if (!remote_eviction_valid_local_oop(g1h, cast_to_oop((HeapWord*)addr),
                                       hr, &klass)) {
    return nullptr;
  }
  return klass;
}

void G1RemoteMemoryManager::record_molecule_profile_edge(Klass* from,
                                                         Klass* to,
                                                         bool array_source,
                                                         bool mutation) {
  if (!molecule_profile_ready() || from == nullptr || to == nullptr) {
    return;
  }

  molecule_profile_lock();
  if (mutation) {
    _molecule_profile_mutation_overwrites++;
    record_molecule_profile_klass_locked(from, 0, 0, 0, 1);
  } else {
    _molecule_profile_promotion_edges++;
    if (array_source) {
      _molecule_profile_array_edges++;
    }
    record_molecule_profile_klass_locked(from, 0, 0, 1, 0);
  }
  record_molecule_profile_edge_locked(from, to, array_source, mutation);
  molecule_profile_unlock();
}

void G1RemoteMemoryManager::record_molecule_profile_old_copy(oop obj,
                                                             size_t word_size) {
  if (!molecule_profile_ready() || obj == nullptr) {
    return;
  }

  Klass* from = obj->klass_or_null_acquire();
  if (!remote_eviction_valid_klass(from) || G1CollectedHeap::is_obj_filler(obj)) {
    return;
  }

  molecule_profile_lock();
  _molecule_profile_old_copies++;
  _molecule_profile_old_copy_bytes += (uint64_t)word_size * HeapWordSize;
  record_molecule_profile_klass_locked(from, 1,
                                       (uint64_t)word_size * HeapWordSize,
                                       0, 0);
  molecule_profile_unlock();

  if (from->is_typeArray_klass() || G1RemoteMoleculeProfileEdgeSampleLimit == 0) {
    return;
  }

  class RecordEdgeClosure : public BasicOopIterateClosure {
    G1RemoteMemoryManager* _rmm;
    G1CollectedHeap*       _g1h;
    Klass*                 _from;
    uint                   _remaining;
  public:
    RecordEdgeClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                      Klass* from, uint limit)
      : _rmm(rmm), _g1h(g1h), _from(from), _remaining(limit) {}

    void do_oop(oop* p) override {
      if (_remaining == 0) {
        return;
      }
      Klass* to = molecule_profile_klass_for_raw(_g1h, *(uintptr_t*)p);
      if (to == nullptr) {
        return;
      }
      _remaining--;
      _rmm->record_molecule_profile_edge(_from, to, false, false);
    }

    void do_oop(narrowOop* p) override {
      // Spark/RDMA experiments run with UseCompressedOops=false. Keep the
      // profiler conservative for other configurations.
    }
  };

  if (from->is_objArray_klass()) {
    if (UseCompressedOops) {
      return;
    }
    objArrayOop array = objArrayOop(obj);
    int len = array->length();
    if (len <= 0) {
      return;
    }
    uint limit = MIN2((uint)len, G1RemoteMoleculeProfileEdgeSampleLimit);
    int step = MAX2(1, len / (int)limit);
    uint sampled = 0;
    for (int i = 0; i < len && sampled < limit; i += step) {
      address slot_addr = cast_from_oop<address>(array) +
                          objArrayOopDesc::base_offset_in_bytes() +
                          (size_t)i * sizeof(oop);
      Klass* to = molecule_profile_klass_for_raw(_g1h, *(uintptr_t*)slot_addr);
      if (to == nullptr) {
        continue;
      }
      sampled++;
      record_molecule_profile_edge(from, to, true, false);
    }
    return;
  }

  RecordEdgeClosure cl(this, _g1h, from, G1RemoteMoleculeProfileEdgeSampleLimit);
  obj->oop_iterate(&cl);
}

void G1RemoteMemoryManager::record_molecule_profile_ref_overwrite(void* field,
                                                                  bool is_narrow) {
  if (!molecule_profile_ready() || field == nullptr || is_narrow) {
    return;
  }

  uint64_t probe = Atomic::add(&_molecule_profile_mutation_probes, (uint64_t)1);
  if (G1RemoteMoleculeProfileMutationSampleRate > 1 &&
      (probe % G1RemoteMoleculeProfileMutationSampleRate) != 0) {
    return;
  }

  if (!_g1h->is_in_reserved(field)) {
    return;
  }
  HeapRegion* src_hr = _g1h->heap_region_containing_or_null(field);
  if (src_hr == nullptr || !src_hr->is_old() || src_hr->is_free() ||
      src_hr->is_empty() || src_hr->is_evict_guarded()) {
    return;
  }

  HeapWord* src_start = src_hr->block_start(field);
  if (src_start == nullptr || src_start < src_hr->bottom() ||
      src_start >= src_hr->top()) {
    return;
  }

  Klass* from = nullptr;
  oop src_obj = cast_to_oop(src_start);
  if (!remote_eviction_valid_local_oop(_g1h, src_obj, src_hr, &from)) {
    return;
  }

  uintptr_t raw = *(uintptr_t*)field;
  Klass* to = molecule_profile_klass_for_raw(_g1h, raw);
  if (to == nullptr) {
    return;
  }

  record_molecule_profile_edge(from, to, false, true);
}

static const char* molecule_profile_klass_name(Klass* klass) {
  return remote_eviction_valid_klass(klass) ? klass->external_name() : "?";
}

void G1RemoteMemoryManager::log_molecule_profile_summary() {
  if (!molecule_profile_ready()) {
    return;
  }

  molecule_profile_lock();
  uint64_t old_copies = _molecule_profile_old_copies;
  uint64_t promotion_edges = _molecule_profile_promotion_edges;
  uint64_t mutation_overwrites = _molecule_profile_mutation_overwrites;
  uint64_t mutation_probes = Atomic::load(&_molecule_profile_mutation_probes);
  if (old_copies == 0 && promotion_edges == 0 && mutation_overwrites == 0) {
    molecule_profile_unlock();
    return;
  }

  log_info(gc)("Molecule profile: old_copies=" UINT64_FORMAT
               " old_copy_bytes=" UINT64_FORMAT
               " promotion_edges=" UINT64_FORMAT
               " array_edges=" UINT64_FORMAT
               " mutation_samples=" UINT64_FORMAT "/" UINT64_FORMAT
               " dropped(klass=" UINT64_FORMAT " edge=" UINT64_FORMAT ")"
               " table=%u",
               old_copies,
               _molecule_profile_old_copy_bytes,
               promotion_edges,
               _molecule_profile_array_edges,
               mutation_overwrites,
               mutation_probes,
               _molecule_profile_dropped_klass,
               _molecule_profile_dropped_edges,
               _molecule_profile_capacity);

  uint top_k = MIN2(G1RemoteMoleculeProfileTopK, (uint)64);
  uint selected[64];
  for (uint i = 0; i < 64; i++) {
    selected[i] = UINT_MAX;
  }

  for (uint rank = 0; rank < top_k; rank++) {
    uint best = UINT_MAX;
    uint64_t best_bytes = 0;
    for (uint i = 0; i < _molecule_profile_capacity; i++) {
      if (_molecule_klass_profile[i]._klass == nullptr) {
        continue;
      }
      bool used = false;
      for (uint s = 0; s < rank; s++) {
        if (selected[s] == i) {
          used = true;
          break;
        }
      }
      if (used) {
        continue;
      }
      if (_molecule_klass_profile[i]._old_copy_bytes > best_bytes) {
        best = i;
        best_bytes = _molecule_klass_profile[i]._old_copy_bytes;
      }
    }
    if (best == UINT_MAX || best_bytes == 0) {
      break;
    }
    selected[rank] = best;
    MoleculeKlassProfileEntry* e = &_molecule_klass_profile[best];
    log_info(gc)("Molecule profile klass top_bytes[%u]: klass=%s copies="
                 UINT64_FORMAT " bytes=" UINT64_FORMAT " out_edges="
                 UINT64_FORMAT " mutations=" UINT64_FORMAT,
                 rank + 1, molecule_profile_klass_name(e->_klass),
                 e->_old_copies, e->_old_copy_bytes, e->_out_edges,
                 e->_mutation_overwrites);
  }

  for (uint i = 0; i < 64; i++) {
    selected[i] = UINT_MAX;
  }
  for (uint rank = 0; rank < top_k; rank++) {
    uint best = UINT_MAX;
    uint64_t best_edges = 0;
    for (uint i = 0; i < _molecule_profile_capacity; i++) {
      if (_molecule_edge_profile[i]._from == nullptr) {
        continue;
      }
      bool used = false;
      for (uint s = 0; s < rank; s++) {
        if (selected[s] == i) {
          used = true;
          break;
        }
      }
      if (used) {
        continue;
      }
      if (_molecule_edge_profile[i]._promotion_edges > best_edges) {
        best = i;
        best_edges = _molecule_edge_profile[i]._promotion_edges;
      }
    }
    if (best == UINT_MAX || best_edges == 0) {
      break;
    }
    selected[rank] = best;
    MoleculeEdgeProfileEntry* e = &_molecule_edge_profile[best];
    log_info(gc)("Molecule profile edge top_promoted[%u]: from=%s to=%s "
                 "edges=" UINT64_FORMAT " array_edges=" UINT64_FORMAT
                 " mutations=" UINT64_FORMAT,
                 rank + 1, molecule_profile_klass_name(e->_from),
                 molecule_profile_klass_name(e->_to),
                 e->_promotion_edges, e->_array_source_edges,
                 e->_mutation_overwrites);
  }

  for (uint i = 0; i < 64; i++) {
    selected[i] = UINT_MAX;
  }
  for (uint rank = 0; rank < top_k; rank++) {
    uint best = UINT_MAX;
    uint64_t best_mutations = 0;
    for (uint i = 0; i < _molecule_profile_capacity; i++) {
      if (_molecule_edge_profile[i]._from == nullptr) {
        continue;
      }
      bool used = false;
      for (uint s = 0; s < rank; s++) {
        if (selected[s] == i) {
          used = true;
          break;
        }
      }
      if (used) {
        continue;
      }
      if (_molecule_edge_profile[i]._mutation_overwrites > best_mutations) {
        best = i;
        best_mutations = _molecule_edge_profile[i]._mutation_overwrites;
      }
    }
    if (best == UINT_MAX || best_mutations == 0) {
      break;
    }
    selected[rank] = best;
    MoleculeEdgeProfileEntry* e = &_molecule_edge_profile[best];
    log_info(gc)("Molecule profile edge top_mutated[%u]: from=%s to=%s "
                 "mutations=" UINT64_FORMAT " promoted_edges=" UINT64_FORMAT,
                 rank + 1, molecule_profile_klass_name(e->_from),
                 molecule_profile_klass_name(e->_to),
                 e->_mutation_overwrites, e->_promotion_edges);
  }

  molecule_profile_unlock();
}

static bool remote_eviction_parse_obj(HeapRegion* hr, HeapWord* p,
                                      HeapWord* limit, size_t* obj_words) {
  if (p < hr->bottom() || p >= limit || p >= hr->end()) return false;
  if (!is_object_aligned((void*)p)) return false;

  // Do not call HeapRegion::block_start()/block_is_obj() here. Phase C may
  // probe conservative addresses while looking for missed object starts, and
  // G1's block-start path can parse interior primitive-array payload as an
  // object header before it has proved that the address is a real boundary.

  oop obj = cast_to_oop(p);
  Klass* k = obj->klass_or_null_acquire();
  if (!remote_eviction_valid_klass(k)) return false;

  Klass* size_k = obj->klass_or_null_acquire();
  if (size_k != k) return false;

  size_t sz = obj->size_given_klass(size_k);
  if (sz < (size_t)MinObjAlignment) return false;
  if (!is_object_aligned(sz)) return false;
  if (sz > (size_t)(limit - p)) return false;
  if (sz > (size_t)(hr->end() - p)) return false;

  *obj_words = sz;
  return true;
}

template <class ObjectClosure>
static int remote_eviction_scan_region_objects(HeapRegion* hr,
                                               const G1CMBitMap* bitmap,
                                               const char* phase,
                                               ObjectClosure* cl) {
  // Dense segments that have been pushed remote remain logical old regions,
  // but their backing pages are protected and not locally parsable.  All
  // remote-eviction heap walks must treat them like absent heap contents.
  if (hr == nullptr || hr->is_empty() || hr->is_free() ||
      hr->is_evict_guarded() || hr->is_continues_humongous()) {
    return 0;
  }

  if (hr->is_starts_humongous()) {
    oop obj = cast_to_oop(hr->bottom());
    Klass* k = obj->klass_or_null_acquire();
    if (!remote_eviction_valid_klass(k)) {
      log_warning(gc)("%s humongous scan skipped: region %u invalid klass at "
                      PTR_FORMAT,
                      phase, hr->hrm_index(), p2i((void*)obj));
      return 0;
    }
    cl->do_object(obj);
    return 1;
  }

  HeapWord* const pb = hr->parsable_bottom_acquire();
  HeapWord* const region_top = hr->top();
  HeapWord* const region_end = hr->end();
  int objects_scanned = 0;
  int unmarked_scanned = 0;
  size_t invalid_words = 0;
  bool truncated = false;
  bool marked_parse_failed = false;

  // Below parsable_bottom: G1 normally uses the mark bitmap because dead
  // objects may have unloaded klasses. Remote eviction is less forgiving:
  // if a live object was allocated/copied outside the current bitmap's view
  // and we miss it here, a raw reference can survive into a guarded region.
  // Walk marked objects first, then conservatively parse unmarked gaps and
  // scan only words that look like valid object starts.
  HeapWord* p = hr->bottom();
  HeapWord* const below_limit = MIN2(pb, region_top);
  while (p < below_limit) {
    if (bitmap->is_marked(p)) {
      size_t sz = 0;
      if (remote_eviction_parse_obj(hr, p, below_limit, &sz)) {
        oop obj = cast_to_oop(p);
        cl->do_object(obj);
        objects_scanned++;
        p += sz;
      } else {
        marked_parse_failed = true;
        HeapWord* next = bitmap->get_next_marked_addr(p + MinObjAlignment, below_limit);
        p = (next > p) ? next : below_limit;
      }
    } else {
      HeapWord* next_mark = bitmap->get_next_marked_addr(p, below_limit);
      HeapWord* q = p;
      while (q < next_mark) {
        size_t sz = 0;
        if (remote_eviction_parse_obj(hr, q, next_mark, &sz)) {
          oop obj = cast_to_oop(q);
          cl->do_object(obj);
          objects_scanned++;
          unmarked_scanned++;
          q += sz;
        } else {
          q += MinObjAlignment;
          invalid_words += MinObjAlignment;
        }
      }
      p = next_mark;
    }
  }

  // Above parsable_bottom: all objects are live, sequential scan is safe.
  if (p < pb) p = pb;
  while (p < region_top) {
    if (p >= region_end) break;
    size_t sz = 0;
    if (!remote_eviction_parse_obj(hr, p, region_top, &sz)) {
      size_t skipped_words = pointer_delta(region_top, p);
      log_warning(gc)("%s scan TRUNCATED: region %u type=%s invalid object at " PTR_FORMAT
                      " (scanned %d objs, unmarked=%d, skipping " SIZE_FORMAT
                      " words to top " PTR_FORMAT ", pb=" PTR_FORMAT ")",
                      phase, hr->hrm_index(), hr->get_short_type_str(),
                      p2i(p), objects_scanned, unmarked_scanned, skipped_words,
                      p2i(region_top), p2i(pb));
      truncated = true;
      break;
    }
    oop obj = cast_to_oop(p);
    cl->do_object(obj);
    objects_scanned++;
    p += sz;
  }

  if (unmarked_scanned > 0 || marked_parse_failed || truncated) {
    log_warning(gc)("%s conservative scan: region %u type=%s pb=" PTR_FORMAT
                    " top=" PTR_FORMAT " scanned=%d unmarked_valid=%d"
                    " invalid_gap_words=" SIZE_FORMAT " marked_parse_failed=%s",
                    phase, hr->hrm_index(), hr->get_short_type_str(),
                    p2i(pb), p2i(region_top), objects_scanned, unmarked_scanned,
                    invalid_words, marked_parse_failed ? "yes" : "no");
  }

  return objects_scanned;
}

class EvictionTagObjectClosure {
  EvictionSetTagClosure* _cl;
public:
  EvictionTagObjectClosure(EvictionSetTagClosure* cl) : _cl(cl) {}
  void do_object(oop obj) {
    _cl->set_cur_obj(obj);
    obj->oop_iterate(_cl);
    _cl->set_cur_obj(nullptr);
  }
};

static void scan_region_for_eviction_tags(HeapRegion* hr, EvictionSetTagClosure* cl,
                                          const G1CMBitMap* bitmap) {
  EvictionTagObjectClosure obj_cl(cl);
  remote_eviction_scan_region_objects(hr, bitmap, "Phase C", &obj_cl);
}

class ObjArrayContainerPrescanClosure : public BasicOopIterateClosure {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  bool*                  _eviction_set;
  uint                   _num_regions;
  uint                   _src_idx;
  oop                    _cur_obj;
  Klass*                 _cur_source_klass;
  int                    _arrays_scanned;
  int                    _objects_scanned;
  size_t                 _elements_scanned;
  size_t                 _field_refs_scanned;
  int                    _candidate_hits;
  int                    _source_hints;
  int                    _unsafe_hits;
  int                    _hint_failures;
  int                    _removed_candidates;
  int                    _reports_left;
  bool                   _limit_reached;

  bool element_budget_exhausted() const {
    return G1RemoteObjArrayContainerPrescanMaxElements > 0 &&
           _elements_scanned >= G1RemoteObjArrayContainerPrescanMaxElements;
  }

  bool remove_candidate(uint idx, const char* reason, objArrayOop array,
                        oop target, Klass* target_klass) {
    if (idx >= _num_regions || !_eviction_set[idx]) {
      return false;
    }

    _eviction_set[idx] = false;
    HeapRegion* target_hr = _g1h->region_at_or_null(idx);
    if (target_hr != nullptr) {
      target_hr->clear_cold_destination();
    }
    _rmm->backoff_eviction_region(idx, G1RemoteEvictionAbortBackoffGCCycles);
    _removed_candidates++;

    if (_reports_left > 0) {
      log_info(gc)("ObjArray container pre-scan: removed candidate region %u "
                   "reason=%s src_region=%u src_obj=" PTR_FORMAT
                   "src_klass=%s array=" PTR_FORMAT
                   " target=" PTR_FORMAT " target_klass=%s",
                   idx, reason, _src_idx, p2i((void*)_cur_obj),
                   _cur_source_klass != nullptr ? _cur_source_klass->external_name() : "?",
                   p2i((void*)array), p2i((void*)target),
                   target_klass != nullptr ? target_klass->external_name() : "?");
      _reports_left--;
    }
    return true;
  }

  void scan_oop_slot(oop* slot, objArrayOop array) {
    if (_limit_reached || slot == nullptr) {
      return;
    }
    if (element_budget_exhausted()) {
      _limit_reached = true;
      return;
    }
    _elements_scanned++;
    if (array == nullptr) {
      _field_refs_scanned++;
    }

    uintptr_t raw = *(uintptr_t*)slot;
    if (raw == 0) {
      return;
    }

    if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
        (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
      return;
    }
    if ((raw & G1_OOP_MANAGED_BIT) != 0) {
      return;
    }

    uintptr_t target_addr = raw;
    if (!is_aligned((address)target_addr, HeapWordSize)) {
      return;
    }
    if (!_g1h->is_in((void*)target_addr)) {
      return;
    }

    HeapRegion* target_hr =
        _g1h->heap_region_containing_or_null((void*)target_addr);
    if (target_hr == nullptr) {
      return;
    }
    uint target_idx = target_hr->hrm_index();
    if (target_idx >= _num_regions || !_eviction_set[target_idx]) {
      return;
    }

    oop target = cast_to_oop((HeapWord*)target_addr);
    Klass* target_klass = nullptr;
    if (!remote_eviction_valid_local_oop(_g1h, target, target_hr, &target_klass)) {
      return;
    }
    if (target->is_forwarded()) {
      return;
    }

    _candidate_hits++;

    bool source_obj_array =
        _cur_source_klass != nullptr && _cur_source_klass->is_objArray_klass();
    bool source_non_array =
        _cur_source_klass != nullptr && !_cur_source_klass->is_array_klass();
    bool target_type_array =
        target_klass != nullptr && target_klass->is_typeArray_klass();
    bool target_object =
        target_klass != nullptr && !target_klass->is_array_klass();
    bool taggable =
        source_non_array ||
        (source_obj_array &&
         ((target_type_array && G1RemoteTagObjArraySources) ||
          (target_object && G1RemoteTagObjArrayObjectSources)));

    if (taggable) {
      bool already_hint = _rmm->is_fast_phase_c_source_hint(_src_idx);
      bool remembered = _rmm->remember_fast_phase_c_source_hint(_src_idx);
      if (remembered) {
        if (!already_hint) {
          _source_hints++;
        }
      } else {
        _hint_failures++;
        if (G1RemoteObjArrayContainerPrescanBackoffUnsafe) {
          remove_candidate(target_idx, "source-hint-limit", array, target,
                           target_klass);
        }
      }
    } else {
      _unsafe_hits++;
      if (G1RemoteObjArrayContainerPrescanBackoffUnsafe) {
        remove_candidate(target_idx, "unsafe-source", array, target,
                         target_klass);
      }
    }
  }

public:
  ObjArrayContainerPrescanClosure(G1RemoteMemoryManager* rmm,
                                  G1CollectedHeap* g1h,
                                  bool* eviction_set,
                                  uint num_regions)
    : _rmm(rmm), _g1h(g1h), _eviction_set(eviction_set),
      _num_regions(num_regions), _src_idx((uint)-1), _cur_obj(nullptr),
      _cur_source_klass(nullptr), _arrays_scanned(0), _objects_scanned(0),
      _elements_scanned(0), _field_refs_scanned(0), _candidate_hits(0),
      _source_hints(0), _unsafe_hits(0), _hint_failures(0),
      _removed_candidates(0), _reports_left(12), _limit_reached(false) {}

  void set_source_region(uint src_idx) { _src_idx = src_idx; }

  void do_object(oop obj) {
    if (_limit_reached || obj == nullptr || UseCompressedOops) {
      return;
    }

    Klass* source_klass = obj->klass_or_null_acquire();
    if (!remote_eviction_valid_klass(source_klass) ||
        (source_klass->is_array_klass() &&
         !source_klass->is_objArray_klass())) {
      return;
    }

    _cur_obj = obj;
    _cur_source_klass = source_klass;
    _objects_scanned++;

    if (!source_klass->is_objArray_klass()) {
      obj->oop_iterate(this);
      _cur_obj = nullptr;
      _cur_source_klass = nullptr;
      return;
    }

    objArrayOop array = objArrayOop(obj);
    int len = array->length();
    if (len <= 0) {
      _cur_obj = nullptr;
      _cur_source_klass = nullptr;
      return;
    }
    _arrays_scanned++;

    for (int i = 0; i < len; i++) {
      if (element_budget_exhausted()) {
        _limit_reached = true;
        break;
      }

      address slot_addr = cast_from_oop<address>(array) +
                          objArrayOopDesc::base_offset_in_bytes() +
                          (size_t)i * sizeof(oop);
      scan_oop_slot((oop*)slot_addr, array);
    }
    _cur_obj = nullptr;
    _cur_source_klass = nullptr;
  }

  virtual void do_oop(oop* p) { scan_oop_slot(p, nullptr); }
  virtual void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }

  bool limit_reached() const { return _limit_reached; }
  int arrays_scanned() const { return _arrays_scanned; }
  int objects_scanned() const { return _objects_scanned; }
  size_t elements_scanned() const { return _elements_scanned; }
  size_t field_refs_scanned() const { return _field_refs_scanned; }
  int candidate_hits() const { return _candidate_hits; }
  int source_hints() const { return _source_hints; }
  int unsafe_hits() const { return _unsafe_hits; }
  int hint_failures() const { return _hint_failures; }
  int removed_candidates() const { return _removed_candidates; }
};

int G1RemoteMemoryManager::prescan_old_objarray_sources_to_eviction_set(
    bool* eviction_set, uint num_regions) {
  if (!G1RemoteUseObjArrayContainerPrescan ||
      !G1RemoteUseFastPhaseC ||
      !dense_segments_enabled() ||
      UseCompressedOops ||
      eviction_set == nullptr ||
      num_regions == 0 ||
      G1RemoteObjArrayContainerPrescanMaxRegions == 0) {
    return 0;
  }

  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();
  ObjArrayContainerPrescanClosure cl(this, _g1h, eviction_set, num_regions);
  bool* scanned_sources = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
  memset(scanned_sources, 0, num_regions * sizeof(bool));

  uint candidate_regions = 0;
  for (uint i = 0; i < num_regions; i++) {
    if (eviction_set[i]) {
      candidate_regions++;
    }
  }

  uint regions_scanned = 0;
  uint hint_regions_scanned = 0;
  uint neighbor_regions_scanned = 0;
  uint fallback_regions_scanned = 0;
  bool region_limit_reached = false;

  auto scan_source_region = [&](uint i, uint* pass_counter) -> bool {
    if (i >= num_regions) {
      return false;
    }
    if (cl.limit_reached()) {
      return true;
    }
    if (regions_scanned >= G1RemoteObjArrayContainerPrescanMaxRegions) {
      region_limit_reached = true;
      return true;
    }
    if (scanned_sources[i]) {
      return false;
    }
    scanned_sources[i] = true;
    if (!G1RemoteObjArrayContainerPrescanScanSourceHints &&
        is_fast_phase_c_source_hint(i)) {
      return false;
    }

    HeapRegion* hr = _g1h->region_at_or_null(i);
    if (hr == nullptr) return false;
    if (hr->is_empty() || hr->is_free()) return false;
    if (hr->is_young() || hr->is_continues_humongous()) return false;
    if (eviction_set[i]) return false;
    if (!hr->is_old() && !hr->is_starts_humongous()) return false;
    if (regions_scanned >= G1RemoteObjArrayContainerPrescanMaxRegions) {
      region_limit_reached = true;
      return false;
    }

    cl.set_source_region(i);
    remote_eviction_scan_region_objects(hr, bitmap,
                                        "ObjArray container pre-scan", &cl);
    regions_scanned++;
    if (pass_counter != nullptr) {
      (*pass_counter)++;
    }
    return cl.limit_reached();
  };

  if (G1RemoteObjArrayContainerPrescanScanSourceHints) {
    for (uint i = 0; i < num_regions; i++) {
      if (!is_fast_phase_c_source_hint(i)) {
        continue;
      }
      if (scan_source_region(i, &hint_regions_scanned)) {
        break;
      }
    }
  }

  uint window = G1RemoteObjArrayContainerPrescanCandidateWindow;
  if (!cl.limit_reached() && !region_limit_reached && window > 0) {
    for (uint candidate_idx = 0; candidate_idx < num_regions; candidate_idx++) {
      if (!eviction_set[candidate_idx]) {
        continue;
      }

      for (uint distance = 1; distance <= window; distance++) {
        if (candidate_idx >= distance) {
          if (scan_source_region(candidate_idx - distance,
                                 &neighbor_regions_scanned)) {
            break;
          }
        }

        if (distance < num_regions - candidate_idx) {
          uint upper_idx = candidate_idx + distance;
          if (scan_source_region(upper_idx, &neighbor_regions_scanned)) {
            break;
          }
        }
      }

      if (cl.limit_reached() || region_limit_reached) {
        break;
      }
    }
  }

  uint fallback_limit = MIN2(num_regions,
                             G1RemoteObjArrayContainerPrescanLowPrefixRegions);
  if (!cl.limit_reached() && !region_limit_reached && fallback_limit > 0) {
    for (uint i = 0; i < fallback_limit; i++) {
      if (is_fast_phase_c_source_hint(i)) {
        continue;
      }
      if (scan_source_region(i, &fallback_regions_scanned)) {
        break;
      }
    }
  }

  if (candidate_regions > 0 || regions_scanned > 0 ||
      cl.removed_candidates() > 0) {
    log_info(gc)("ObjArray container pre-scan: candidates=%u regions=%u "
                 "hint_regions=%u neighbor_regions=%u fallback_regions=%u "
                 "objects=%d arrays=%d refs=" SIZE_FORMAT
                 " obj_fields=" SIZE_FORMAT " candidate_hits=%d "
                 "hints=%d unsafe=%d hint_failures=%d removed=%d "
                 "element_limit=%s region_limit=%s window=%u fallback_limit=%u",
                 candidate_regions, regions_scanned, hint_regions_scanned,
                 neighbor_regions_scanned, fallback_regions_scanned,
                 cl.objects_scanned(), cl.arrays_scanned(),
                 cl.elements_scanned(), cl.field_refs_scanned(),
                 cl.candidate_hits(), cl.source_hints(), cl.unsafe_hits(),
                 cl.hint_failures(), cl.removed_candidates(),
                 cl.limit_reached() ? "yes" : "no",
                 region_limit_reached ? "yes" : "no",
                 window, fallback_limit);
  }

  int removed = cl.removed_candidates();
  FREE_C_HEAP_ARRAY(bool, scanned_sources);
  return removed;
}

class TagAllHeapRefsTask : public WorkerTask {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  const bool*            _eviction_set;
  uint                   _num_regions;
  const G1CMBitMap*      _bitmap;
  HeapRegionClaimer      _claimer;
  volatile int           _total_tagged;
  volatile int           _total_no_handle;
  volatile int           _total_untaggable;

  typedef G1RemoteMemoryManager::TaggedFieldEntry TaggedFieldEntry;
  TaggedFieldEntry** _worker_bufs;
  int*               _worker_counts;
  uint               _num_workers;

public:
  TagAllHeapRefsTask(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                     const bool* eset, uint nregions, uint num_workers,
                     const G1CMBitMap* bitmap)
    : WorkerTask("Tag eviction refs"),
      _rmm(rmm), _g1h(g1h), _eviction_set(eset), _num_regions(nregions),
      _bitmap(bitmap),
      _claimer(num_workers), _total_tagged(0), _total_no_handle(0),
      _total_untaggable(0),
      _num_workers(num_workers) {
    _worker_bufs = NEW_C_HEAP_ARRAY(TaggedFieldEntry*, num_workers, mtGC);
    _worker_counts = NEW_C_HEAP_ARRAY(int, num_workers, mtGC);
    memset(_worker_bufs, 0, num_workers * sizeof(TaggedFieldEntry*));
    memset(_worker_counts, 0, num_workers * sizeof(int));
  }

  ~TagAllHeapRefsTask() {
    for (uint i = 0; i < _num_workers; i++) {
      if (_worker_bufs[i] != nullptr) {
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _worker_bufs[i]);
      }
    }
    FREE_C_HEAP_ARRAY(TaggedFieldEntry*, _worker_bufs);
    FREE_C_HEAP_ARRAY(int, _worker_counts);
  }

  void work(uint worker_id) {
    EvictionSetTagClosure cl(_rmm, _g1h, _eviction_set, _num_regions);
    for (uint i = _claimer.offset_for_worker(worker_id); i < _claimer.n_regions(); i++) {
      if (!_claimer.claim_region(i)) continue;
      HeapRegion* hr = _g1h->region_at_or_null(i);
      if (hr == nullptr) continue;
      if (hr->is_empty() || hr->is_free()) continue;
      if (hr->is_continues_humongous()) continue;
      scan_region_for_eviction_tags(hr, &cl, _bitmap);
    }
    Atomic::add(&_total_tagged, cl.tagged());
    Atomic::add(&_total_no_handle, cl.no_handle());
    Atomic::add(&_total_untaggable, cl.untaggable());
    _worker_bufs[worker_id] = cl.release_local_buf();
    _worker_counts[worker_id] = cl.local_count();
  }

  void flush_to_rmm() {
    for (uint i = 0; i < _num_workers; i++) {
      for (int j = 0; j < _worker_counts[i]; j++) {
        _rmm->add_tagged_field_entry(_worker_bufs[i][j]);
      }
    }
  }

  int total_tagged() const { return _total_tagged; }
  int total_no_handle() const { return _total_no_handle; }
  int total_untaggable() const { return _total_untaggable; }
};

int G1RemoteMemoryManager::tag_all_heap_refs_to_eviction_set(
    const bool* eviction_set, uint num_regions,
    WorkerThreads* workers, uint num_workers) {

  int total_tagged, total_no_handle, total_untaggable;

  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();

  if (workers != nullptr && num_workers > 1) {
    TagAllHeapRefsTask task(this, _g1h, eviction_set, num_regions, num_workers, bitmap);
    workers->run_task(&task, num_workers);
    task.flush_to_rmm();
    total_tagged = task.total_tagged();
    total_no_handle = task.total_no_handle();
    total_untaggable = task.total_untaggable();
  } else {
    EvictionSetTagClosure cl(this, _g1h, eviction_set, num_regions);
    for (uint i = 0; i < _g1h->max_reserved_regions(); i++) {
      HeapRegion* hr = _g1h->region_at_or_null(i);
      if (hr == nullptr) continue;
      if (hr->is_empty() || hr->is_free()) continue;
      if (hr->is_continues_humongous()) continue;
      scan_region_for_eviction_tags(hr, &cl, bitmap);
    }
    for (int j = 0; j < cl.local_count(); j++) {
      add_tagged_field_entry(cl.local_buf()[j]);
    }
    total_tagged = cl.tagged();
    total_no_handle = cl.no_handle();
    total_untaggable = cl.untaggable();
  }

  if (total_tagged > 0 || total_no_handle > 0 || total_untaggable > 0) {
    log_info(gc)("Full heap scan (%u workers): tagged %d refs, %d refs had no handle, "
                 "%d refs from untaggable sources kept raw",
                 (workers != nullptr ? num_workers : 1), total_tagged,
                 total_no_handle, total_untaggable);
  }
  if (total_untaggable > 0) {
    log_warning(gc)("Full heap scan: verifier will keep candidate regions local if "
                    "array/unknown/root refs still point into them");
  }
  record_phase_c_counts(total_tagged, total_no_handle, total_untaggable);
  return total_tagged;
}

int G1RemoteMemoryManager::tag_evacuated_area_refs_to_eviction_set(
    const bool* eviction_set, uint num_regions,
    HeapWord* const* pre_evac_tops) {

  EvictionSetTagClosure cl(this, _g1h, eviction_set, num_regions);
  int regions_rescanned = 0;

  for (uint i = 0; i < num_regions; i++) {
    HeapRegion* hr = _g1h->region_at_or_null(i);
    if (hr == nullptr) continue;
    if (hr->is_empty() || hr->is_free()) continue;
    if (hr->is_continues_humongous()) continue;

    HeapWord* pre_top = pre_evac_tops[i];
    if (pre_top == nullptr) continue;
    HeapWord* cur_top = hr->top();
    if (pre_top >= cur_top) continue;

    HeapWord* p = pre_top;
    HeapWord* region_end = hr->end();
    while (p < cur_top) {
      if (p >= region_end) break;
      oop obj = cast_to_oop(p);
      Klass* k = obj->klass_or_null();
      if (k == nullptr) {
        log_warning(gc)("Phase C.1: null klass at " PTR_FORMAT " in region %u "
                        "(pre_top=" PTR_FORMAT " cur_top=" PTR_FORMAT ")",
                        p2i(p), i, p2i(pre_top), p2i(cur_top));
        break;
      }
      size_t sz = obj->size();
      if (sz == 0 || sz > (size_t)(region_end - p)) break;
      cl.set_cur_obj(obj);
      obj->oop_iterate(&cl);
      cl.set_cur_obj(nullptr);
      p += sz;
    }
    regions_rescanned++;
  }

  int total_tagged = cl.tagged();
  if (total_tagged > 0 || cl.untaggable() > 0) {
    for (int j = 0; j < cl.local_count(); j++) {
      add_tagged_field_entry(cl.local_buf()[j]);
    }
    log_warning(gc)("Phase C.1: re-scanned %d regions, tagged %d missed refs "
                    "(%d no handle, %d untaggable)",
                    regions_rescanned, total_tagged, cl.no_handle(), cl.untaggable());
  }
  return total_tagged;
}

// RSet visitor: for each card in a candidate's RSet, scan with
// EvictionSetTagClosure to tag refs pointing to ANY candidate.
class EvictionSetRsetScanner {
  G1CollectedHeap*       _g1h;
  G1CardTable*           _ct;
  EvictionSetTagClosure* _cl;
  const bool*            _eviction_set;
  uint                   _num_regions;

public:
  EvictionSetRsetScanner(G1CollectedHeap* g1h, EvictionSetTagClosure* cl,
                         const bool* eviction_set, uint num_regions)
    : _g1h(g1h), _ct(g1h->card_table()), _cl(cl),
      _eviction_set(eviction_set), _num_regions(num_regions) {}

  bool start_iterate(uint tag, uint region_idx) { return true; }
  void do_card(uint card_idx) { scan_card(card_idx, 1); }
  void do_card_range(uint start_card_idx, uint length) { scan_card(start_card_idx, length); }

  int scan_dirty_cards_in_region(HeapRegion* source_hr) {
    if (source_hr == nullptr || source_hr->is_empty() || source_hr->is_free()) return 0;
    if (source_hr->is_young() || source_hr->is_continues_humongous()) return 0;
    if (source_hr->bottom() >= source_hr->top()) return 0;

    CardTable::CardValue* card = _ct->byte_for(source_hr->bottom());
    CardTable::CardValue* end_card = _ct->byte_for(source_hr->top() - 1) + 1;
    int dirty_cards = 0;

    while (card < end_card) {
      while (card < end_card && *card != G1CardTable::dirty_card_val()) {
        card++;
      }
      CardTable::CardValue* run_start = card;
      while (card < end_card && *card == G1CardTable::dirty_card_val()) {
        card++;
        dirty_cards++;
      }
      if (run_start < card) {
        uint card_idx = (uint)_ct->index_for_cardvalue(run_start);
        uint length = (uint)(card - run_start);
        scan_card(card_idx, length);
      }
    }
    return dirty_cards;
  }

private:
  void scan_card(uint card_idx, uint length) {
    HeapWord* card_start = _ct->addr_for(_ct->byte_for_index(card_idx));
    HeapWord* card_end = card_start + length * CardTable::card_size_in_words();
    if (!_g1h->is_in(card_start)) return;
    HeapRegion* source_hr = _g1h->heap_region_containing(card_start);
    if (source_hr == nullptr || source_hr->is_empty() || source_hr->is_free()) return;
    uint src_idx = source_hr->hrm_index();
    // Skip regions already scanned directly (candidates + young/survivors)
    if (src_idx < _num_regions && _eviction_set[src_idx]) return;
    if (source_hr->is_young()) return;

    HeapWord* scan_start = MAX2(card_start, source_hr->bottom());
    HeapWord* scan_end = MIN2(card_end, source_hr->top());
    if (scan_start >= scan_end) return;

    MemRegion mr(scan_start, scan_end);
    source_hr->oops_on_memregion_seq_iterate_careful<true>(mr, _cl);
  }
};

static bool remote_fast_phase_c_is_candidate_neighbor(uint region_idx,
                                                      const bool* eviction_set,
                                                      uint num_regions) {
  static const uint NeighborWindow = 8;
  if (eviction_set == nullptr || region_idx >= num_regions) {
    return false;
  }

  uint start = region_idx > NeighborWindow ? region_idx - NeighborWindow : 0;
  uint end = MIN2(num_regions - 1, region_idx + NeighborWindow);
  for (uint i = start; i <= end; i++) {
    if (eviction_set[i]) {
      return true;
    }
  }
  return false;
}

class TagFastRefsTask : public WorkerTask {
  G1RemoteMemoryManager* _rmm;
  G1CollectedHeap*       _g1h;
  const bool*            _eviction_set;
  uint                   _num_regions;
  HeapWord* const*       _pre_evac_tops;
  const G1CMBitMap*      _bitmap;
  HeapRegionClaimer      _claimer;
  volatile int           _total_tagged;
  volatile int           _total_no_handle;
  volatile int           _total_untaggable;
  volatile int           _regions_scanned;
  volatile int           _dirty_cards_scanned;
  volatile int           _inbound_summary_regions_scanned;
  volatile int           _source_hint_regions_scanned;
  volatile int           _neighbor_regions_scanned;
  volatile int           _old_prefix_regions_scanned;

  typedef G1RemoteMemoryManager::TaggedFieldEntry TaggedFieldEntry;
  TaggedFieldEntry** _worker_bufs;
  int*               _worker_counts;
  uint               _num_workers;

public:
  TagFastRefsTask(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                  const bool* eset, uint nregions, HeapWord* const* pre_evac_tops,
                  uint num_workers, const G1CMBitMap* bitmap)
    : WorkerTask("Tag eviction refs (fast)"),
      _rmm(rmm), _g1h(g1h), _eviction_set(eset), _num_regions(nregions),
      _pre_evac_tops(pre_evac_tops), _bitmap(bitmap),
      _claimer(num_workers), _total_tagged(0), _total_no_handle(0),
      _total_untaggable(0),
      _regions_scanned(0), _dirty_cards_scanned(0),
      _inbound_summary_regions_scanned(0),
      _source_hint_regions_scanned(0), _neighbor_regions_scanned(0),
      _old_prefix_regions_scanned(0),
      _num_workers(num_workers) {
    _worker_bufs = NEW_C_HEAP_ARRAY(TaggedFieldEntry*, num_workers, mtGC);
    _worker_counts = NEW_C_HEAP_ARRAY(int, num_workers, mtGC);
    memset(_worker_bufs, 0, num_workers * sizeof(TaggedFieldEntry*));
    memset(_worker_counts, 0, num_workers * sizeof(int));
  }

  ~TagFastRefsTask() {
    for (uint i = 0; i < _num_workers; i++) {
      if (_worker_bufs[i] != nullptr) {
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _worker_bufs[i]);
      }
    }
    FREE_C_HEAP_ARRAY(TaggedFieldEntry*, _worker_bufs);
    FREE_C_HEAP_ARRAY(int, _worker_counts);
  }

  void work(uint worker_id) {
    EvictionSetTagClosure cl(_rmm, _g1h, _eviction_set, _num_regions,
                             true /* allow_unknown_heap_source for RSet card scans */);
    EvictionSetRsetScanner rset_scanner(_g1h, &cl, _eviction_set, _num_regions);
    int scanned = 0;
    int dirty_cards = 0;
    int inbound_summary_regions = 0;
    int source_hint_regions = 0;
    int neighbor_regions = 0;
    int old_prefix_regions = 0;

    for (uint i = _claimer.offset_for_worker(worker_id); i < _claimer.n_regions(); i++) {
      if (!_claimer.claim_region(i)) continue;
      HeapRegion* hr = _g1h->region_at_or_null(i);
      if (hr == nullptr) continue;

      if (_eviction_set[i]) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        HeapRegionRemSet* rem_set = hr->rem_set();
        if (rem_set->is_complete() && !rem_set->is_empty()) {
          rem_set->iterate_for_merge(rset_scanner);
        }
        scanned++;
        continue;
      }

      if (hr->is_young()) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        continue;
      }

      // Destination regions that received evacuated/promoted objects after the
      // snapshot may contain fresh refs into candidates but are not yet fully
      // represented by remembered sets.
      if (_pre_evac_tops != nullptr &&
          _pre_evac_tops[i] != nullptr &&
          _pre_evac_tops[i] < hr->top() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        continue;
      }

      if (_rmm->is_inbound_source_for_eviction_set(i, _eviction_set, _num_regions)) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        inbound_summary_regions++;
        continue;
      }

      if (_rmm->is_fast_phase_c_source_hint(i)) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        source_hint_regions++;
        continue;
      }

      if (remote_fast_phase_c_is_candidate_neighbor(i, _eviction_set, _num_regions) &&
          hr->is_old() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        neighbor_regions++;
        continue;
      }

      if (G1RemoteFastPhaseCOldPrefixRegions > 0 &&
          i < G1RemoteFastPhaseCOldPrefixRegions &&
          hr->is_old() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, _bitmap);
        scanned++;
        old_prefix_regions++;
        continue;
      }

      int region_dirty_cards = rset_scanner.scan_dirty_cards_in_region(hr);
      if (region_dirty_cards > 0) {
        dirty_cards += region_dirty_cards;
        scanned++;
      }
    }

    Atomic::add(&_total_tagged, cl.tagged());
    Atomic::add(&_total_no_handle, cl.no_handle());
    Atomic::add(&_total_untaggable, cl.untaggable());
    Atomic::add(&_regions_scanned, scanned);
    Atomic::add(&_dirty_cards_scanned, dirty_cards);
    Atomic::add(&_inbound_summary_regions_scanned, inbound_summary_regions);
    Atomic::add(&_source_hint_regions_scanned, source_hint_regions);
    Atomic::add(&_neighbor_regions_scanned, neighbor_regions);
    Atomic::add(&_old_prefix_regions_scanned, old_prefix_regions);
    _worker_bufs[worker_id] = cl.release_local_buf();
    _worker_counts[worker_id] = cl.local_count();
  }

  void flush_to_rmm() {
    for (uint i = 0; i < _num_workers; i++) {
      for (int j = 0; j < _worker_counts[i]; j++) {
        _rmm->add_tagged_field_entry(_worker_bufs[i][j]);
      }
    }
  }

  int total_tagged() const { return _total_tagged; }
  int total_no_handle() const { return _total_no_handle; }
  int total_untaggable() const { return _total_untaggable; }
  int regions_scanned() const { return _regions_scanned; }
  int dirty_cards_scanned() const { return _dirty_cards_scanned; }
  int inbound_summary_regions_scanned() const { return _inbound_summary_regions_scanned; }
  int source_hint_regions_scanned() const { return _source_hint_regions_scanned; }
  int neighbor_regions_scanned() const { return _neighbor_regions_scanned; }
  int old_prefix_regions_scanned() const { return _old_prefix_regions_scanned; }
};

int G1RemoteMemoryManager::tag_refs_to_eviction_set_fast(
    const bool* eviction_set, uint num_regions,
    HeapWord* const* pre_evac_tops,
    WorkerThreads* workers, uint num_workers) {

  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();
  int total_tagged, total_no_handle, total_untaggable;

  if (workers != nullptr && num_workers > 1) {
    TagFastRefsTask task(this, _g1h, eviction_set, num_regions,
                         pre_evac_tops, num_workers, bitmap);
    workers->run_task(&task, num_workers);
    task.flush_to_rmm();
    total_tagged = task.total_tagged();
    total_no_handle = task.total_no_handle();
    total_untaggable = task.total_untaggable();

    if (total_tagged > 0 || total_no_handle > 0 || total_untaggable > 0) {
      log_info(gc)("Fast Phase C (%u workers, %d regions scanned, %d dirty cards, "
                   "%d inbound summaries, %d source hints, %d neighbors, "
                   "%d old-prefix): "
                   "tagged %d refs, %d no handle, %d untaggable",
                   num_workers, task.regions_scanned(), task.dirty_cards_scanned(),
                   task.inbound_summary_regions_scanned(),
                   task.source_hint_regions_scanned(),
                   task.neighbor_regions_scanned(),
                   task.old_prefix_regions_scanned(),
                   total_tagged, total_no_handle, total_untaggable);
    }
  } else {
    EvictionSetTagClosure cl(this, _g1h, eviction_set, num_regions,
                             true /* allow_unknown_heap_source for RSet card scans */);
    EvictionSetRsetScanner rset_scanner(_g1h, &cl, eviction_set, num_regions);
    int scanned = 0;
    int dirty_cards = 0;
    int inbound_summary_regions = 0;
    int source_hint_regions = 0;
    int neighbor_regions = 0;
    int old_prefix_regions = 0;

    for (uint i = 0; i < num_regions; i++) {
      HeapRegion* hr = _g1h->region_at_or_null(i);
      if (hr == nullptr) continue;

      if (eviction_set[i]) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        HeapRegionRemSet* rem_set = hr->rem_set();
        if (rem_set->is_complete() && !rem_set->is_empty()) {
          rem_set->iterate_for_merge(rset_scanner);
        }
        scanned++;
        continue;
      }

      if (hr->is_young()) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        continue;
      }

      if (pre_evac_tops != nullptr &&
          pre_evac_tops[i] != nullptr &&
          pre_evac_tops[i] < hr->top() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        continue;
      }

      if (is_inbound_source_for_eviction_set(i, eviction_set, num_regions)) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        inbound_summary_regions++;
        continue;
      }

      if (is_fast_phase_c_source_hint(i)) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        source_hint_regions++;
        continue;
      }

      if (remote_fast_phase_c_is_candidate_neighbor(i, eviction_set, num_regions) &&
          hr->is_old() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        neighbor_regions++;
        continue;
      }

      if (G1RemoteFastPhaseCOldPrefixRegions > 0 &&
          i < G1RemoteFastPhaseCOldPrefixRegions &&
          hr->is_old() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        scan_region_for_eviction_tags(hr, &cl, bitmap);
        scanned++;
        old_prefix_regions++;
        continue;
      }

      int region_dirty_cards = rset_scanner.scan_dirty_cards_in_region(hr);
      if (region_dirty_cards > 0) {
        dirty_cards += region_dirty_cards;
        scanned++;
      }
    }

    for (int j = 0; j < cl.local_count(); j++) {
      add_tagged_field_entry(cl.local_buf()[j]);
    }
    total_tagged = cl.tagged();
    total_no_handle = cl.no_handle();
    total_untaggable = cl.untaggable();

    if (total_tagged > 0 || total_no_handle > 0 || total_untaggable > 0) {
      log_info(gc)("Fast Phase C (1 worker, %d regions scanned, %d dirty cards, "
                   "%d inbound summaries, %d source hints, %d neighbors, "
                   "%d old-prefix): "
                   "tagged %d refs, %d no handle, %d untaggable",
                   scanned, dirty_cards, inbound_summary_regions,
                   source_hint_regions, neighbor_regions, old_prefix_regions,
                   total_tagged, total_no_handle, total_untaggable);
    }
  }

  // Phase C root scan: tag references from non-heap root sources
  // (thread stacks, JNI handles, ClassLoaderData, OopStorages)
  // NOTE: CodeCache is NOT scanned here. Nmethod oop constants are raw
  // machine-code immediates — tagging them corrupts compiled code (the
  // movabs constant becomes a non-canonical tagged address that #GPs on
  // dereference). Phase D root-catch already relocates objects referenced
  // by nmethod oops to the catch region, so no tagging is needed.
  {
    EvictionSetTagClosure root_cl(this, _g1h, eviction_set, num_regions);

    Threads::oops_do(&root_cl, nullptr);
    JNIHandles::oops_do(&root_cl);
    OopStorageSet::strong_oops_do(&root_cl);
    for (auto id : EnumRange<OopStorageSet::WeakId>()) {
      OopStorageSet::storage(id)->oops_do(&root_cl);
    }
    oops_do_remote_anchors(&root_cl);
    {
      CLDToOopClosure cld_cl(&root_cl, ClassLoaderData::_claim_none);
      ClassLoaderDataGraph::cld_do(&cld_cl);
    }

    for (int j = 0; j < root_cl.local_count(); j++) {
      add_tagged_field_entry(root_cl.local_buf()[j]);
    }

    int root_tagged = root_cl.tagged();
    total_tagged += root_tagged;
    total_no_handle += root_cl.no_handle();
    total_untaggable += root_cl.untaggable();

    if (root_tagged > 0 || root_cl.no_handle() > 0 || root_cl.untaggable() > 0) {
      log_info(gc)("Phase C root scan: tagged %d refs, %d no handle, %d untaggable",
                   root_tagged, root_cl.no_handle(), root_cl.untaggable());
    }

    TaggedFieldEntry* buf = root_cl.release_local_buf();
    if (buf != nullptr) FREE_C_HEAP_ARRAY(TaggedFieldEntry, buf);
  }

  record_phase_c_counts(total_tagged, total_no_handle, total_untaggable);
  return total_tagged;
}

class G1RemoteRollbackDirtyCards : public StackObj {
  G1CollectedHeap* _g1h;
  G1CardTable* _ct;
  G1DirtyCardQueueSet& _dcqs;
  G1DirtyCardQueue _queue;
  int _dirtied;

public:
  G1RemoteRollbackDirtyCards(G1CollectedHeap* g1h)
    : _g1h(g1h),
      _ct(g1h->card_table()),
      _dcqs(G1BarrierSet::dirty_card_queue_set()),
      _queue(&_dcqs),
      _dirtied(0) {}

  void dirty_field(oop* field_addr) {
    if (field_addr == nullptr || !_g1h->is_in_reserved((void*)field_addr)) {
      return;
    }

    HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)field_addr);
    if (hr == nullptr || hr->is_free() || hr->is_evict_guarded()) {
      return;
    }

    CardTable::CardValue* card = _ct->byte_for((HeapWord*)field_addr);
    if (*card == G1CardTable::g1_young_card_val() ||
        *card == G1CardTable::dirty_card_val()) {
      return;
    }

    *card = G1CardTable::dirty_card_val();
    _dcqs.enqueue(_queue, card);
    _dirtied++;
  }

  void flush() {
    _dcqs.flush_queue(_queue);
  }

  int dirtied() const { return _dirtied; }
};

static void release_dense_tagged_handle_ref(G1RemoteMemoryManager* rmm,
                                            const G1RemoteMemoryManager::TaggedFieldEntry& entry) {
  if (rmm == nullptr || !entry.is_dense_handle() || entry._handle == nullptr) {
    return;
  }
  if (rmm->concurrent_marking_active()) {
    rmm->defer_refcount_decrement(entry._handle);
  } else {
    entry._handle->decrement_remote_refcount();
  }
}

static bool remote_handle_managed_local_oop(G1CollectedHeap* g1h, oop obj) {
  if (g1h == nullptr || obj == nullptr) {
    return false;
  }
  if (!g1h->is_in_reserved(obj) || !g1h->is_in(obj)) {
    return false;
  }
  HeapRegion* hr = g1h->heap_region_containing_or_null(obj);
  return hr != nullptr &&
         (hr->is_old() || hr->is_starts_humongous() ||
          hr->is_fetch_cache());
}

int G1RemoteMemoryManager::untag_recorded_local_refs() {
  int restored = 0;
  int direct_restored = 0;
  int direct_converted = 0;
  int direct_nulled = 0;
  int dense_handle_retained = 0;
  int dense_handle_released = 0;
  int removed = 0;
  int retained = 0;
  G1RemoteRollbackDirtyCards dirty_cards(_g1h);
  RemoteHandleAllocBuffer hab;

  for (int i = 0; i < _tagged_field_count; i++) {
    TaggedFieldEntry entry = _tagged_fields[i];
    oop* field_addr = _tagged_fields[i]._field_addr;
    if (field_addr == nullptr) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
      }
      removed++;
      continue;
    }

    if (!_g1h->is_in_reserved((void*)field_addr)) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
      }
      removed++;
      continue;
    }

    HeapRegion* field_hr = _g1h->heap_region_containing((HeapWord*)field_addr);
    if (field_hr != nullptr && (field_hr->is_free() || field_hr->is_evict_guarded())) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
      }
      removed++;
      continue;
    }

    uintptr_t raw = *(uintptr_t*)field_addr;
    if (_tagged_fields[i].is_direct()) {
      bool direct = (raw & G1_OOP_MANAGED_BIT) != 0 &&
                    (raw & G1_OOP_INDIRECT_BIT) == 0;
      if (!direct) {
        removed++;
        continue;
      }
      if (_tagged_fields[i]._tagged_raw != 0 &&
          raw != _tagged_fields[i]._tagged_raw) {
        _tagged_fields[i]._tagged_raw = raw;
      }
      uintptr_t addr = raw & G1_OOP_ADDR_MASK;
      if (!g1_remote_oop_is_aligned(addr) ||
          !_g1h->is_in_reserved((void*)addr)) {
        removed++;
        continue;
      }

      RemoteHandle* alias = nullptr;
      DenseDirectTargetState target_state =
          classify_dense_direct_target(this, _g1h, addr, &alias, nullptr);
      if (target_state == DenseDirectTargetRemote) {
        *(uintptr_t*)field_addr = 0;
        dirty_cards.dirty_field(field_addr);
        direct_nulled++;
        removed++;
        continue;
      }
      if (target_state == DenseDirectTargetAlias && alias != nullptr) {
        *(uintptr_t*)field_addr =
            G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)alias;
        dirty_cards.dirty_field(field_addr);
        TaggedFieldEntry handle_entry;
        handle_entry._field_addr = field_addr;
        handle_entry._handle = alias;
        handle_entry._tagged_raw = 0;
        handle_entry._kind = TaggedFieldHandle;
        _tagged_fields[retained++] = handle_entry;
        direct_converted++;
        continue;
      }
      if (target_state != DenseDirectTargetLocal) {
        *(uintptr_t*)field_addr = 0;
        dirty_cards.dirty_field(field_addr);
        direct_nulled++;
        removed++;
        continue;
      }

      oop target_oop = cast_to_oop((HeapWord*)addr);
      RemoteHandle* direct_target = remote_handle_managed_local_oop(_g1h, target_oop)
          ? ensure_handle_for(target_oop, &hab) : nullptr;
      if (direct_target != nullptr) {
        *(uintptr_t*)field_addr =
            G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)direct_target;
        dirty_cards.dirty_field(field_addr);
        TaggedFieldEntry handle_entry;
        handle_entry._field_addr = field_addr;
        handle_entry._handle = direct_target;
        handle_entry._tagged_raw = 0;
        handle_entry._kind = TaggedFieldHandle;
        _tagged_fields[retained++] = handle_entry;
        direct_converted++;
      } else {
        *(uintptr_t*)field_addr = addr;
        dirty_cards.dirty_field(field_addr);
        restored++;
        direct_restored++;
      }
      continue;
    }

    RemoteHandle* h = _tagged_fields[i]._handle;
    if (h == nullptr) {
      if (entry.is_dense_handle()) {
        dense_handle_released++;
      }
      removed++;
      continue;
    }

    if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) !=
        (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
      }
      removed++;
      continue;
    }

    if ((RemoteHandle*)(raw & G1_OOP_ADDR_MASK) != h) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
      }
      removed++;
      continue;
    }

    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    if (state == REMOTE_HANDLE_LOCAL) {
      if (entry.is_dense_handle() ||
          is_dense_segment_managed_addr((uintptr_t)field_addr)) {
        _tagged_fields[retained++] = _tagged_fields[i];
        dense_handle_retained++;
        continue;
      }
      *(uintptr_t*)field_addr = sa & REMOTE_HANDLE_ADDR_MASK;
      dirty_cards.dirty_field(field_addr);
      restored++;
      continue;
    }

    _tagged_fields[retained++] = _tagged_fields[i];
  }

  dirty_cards.flush();
  _tagged_field_count = retained;
  if (restored > 0 || direct_converted > 0 ||
      direct_nulled > 0 || dense_handle_retained > 0 ||
      dense_handle_released > 0 || removed > 0 || dirty_cards.dirtied() > 0) {
    log_info(gc)("Recorded untag cleanup: restored %d local refs (%d direct), "
                 "converted %d stale direct refs, "
                 "nulled %d invalid direct refs, retained %d dense handle refs, "
                 "released %d dense handle refs, removed %d stale entries, "
                 "%d remote-tag entries retained, dirtied %d cards",
                 restored, direct_restored, direct_converted, direct_nulled,
                 dense_handle_retained, dense_handle_released, removed, retained,
                 dirty_cards.dirtied());
  }
  return restored;
}

int G1RemoteMemoryManager::cleanup_recorded_direct_refs_after_dense_phase() {
  int restored = 0;
  int dropped_remote = 0;
  int converted = 0;
  int nulled = 0;
  int dense_handle_retained = 0;
  int dense_handle_released = 0;
  int removed = 0;
  int retained = 0;
  G1RemoteRollbackDirtyCards dirty_cards(_g1h);
  RemoteHandleAllocBuffer hab;

  for (int i = 0; i < _tagged_field_count; i++) {
    TaggedFieldEntry entry = _tagged_fields[i];
    if (entry.is_dense_handle()) {
      oop* field_addr = entry._field_addr;
      RemoteHandle* h = entry._handle;
      if (field_addr == nullptr || h == nullptr ||
          !_g1h->is_in_reserved((void*)field_addr)) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
        removed++;
        continue;
      }

      HeapRegion* field_hr = _g1h->heap_region_containing((HeapWord*)field_addr);
      if (field_hr != nullptr &&
          (field_hr->is_free() || field_hr->is_evict_guarded())) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
        removed++;
        continue;
      }

      uintptr_t raw = *(uintptr_t*)field_addr;
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) !=
              (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT) ||
          (RemoteHandle*)(raw & G1_OOP_ADDR_MASK) != h) {
        release_dense_tagged_handle_ref(this, entry);
        dense_handle_released++;
        removed++;
        continue;
      }

      _tagged_fields[retained++] = entry;
      dense_handle_retained++;
      continue;
    }

    if (!entry.is_direct()) {
      _tagged_fields[retained++] = entry;
      continue;
    }

    oop* field_addr = entry._field_addr;
    if (field_addr == nullptr || !_g1h->is_in_reserved((void*)field_addr)) {
      removed++;
      continue;
    }

    HeapRegion* field_hr = _g1h->heap_region_containing((HeapWord*)field_addr);
    if (field_hr != nullptr && (field_hr->is_free() || field_hr->is_evict_guarded())) {
      removed++;
      continue;
    }

    uintptr_t raw = *(uintptr_t*)field_addr;
    bool direct = (raw & G1_OOP_MANAGED_BIT) != 0 &&
                  (raw & G1_OOP_INDIRECT_BIT) == 0;
    if (!direct) {
      removed++;
      continue;
    }
    if (entry._tagged_raw != 0 && raw != entry._tagged_raw) {
      entry._tagged_raw = raw;
    }

    uintptr_t addr = raw & G1_OOP_ADDR_MASK;
    if (!g1_remote_oop_is_aligned(addr) ||
        !_g1h->is_in_reserved((void*)addr)) {
      removed++;
      continue;
    }

    RemoteHandle* alias = nullptr;
    DenseDirectTargetState target_state =
        classify_dense_direct_target(this, _g1h, addr, &alias, nullptr);
    if (target_state == DenseDirectTargetRemote) {
      dropped_remote++;
      *(uintptr_t*)field_addr = 0;
      dirty_cards.dirty_field(field_addr);
      nulled++;
      removed++;
      continue;
    }
    if (target_state == DenseDirectTargetAlias && alias != nullptr) {
      *(uintptr_t*)field_addr =
          G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)alias;
      dirty_cards.dirty_field(field_addr);
      TaggedFieldEntry handle_entry;
      handle_entry._field_addr = field_addr;
      handle_entry._handle = alias;
      handle_entry._tagged_raw = 0;
      handle_entry._kind = TaggedFieldHandle;
      _tagged_fields[retained++] = handle_entry;
      converted++;
      continue;
    }
    if (target_state != DenseDirectTargetLocal) {
      *(uintptr_t*)field_addr = 0;
      dirty_cards.dirty_field(field_addr);
      nulled++;
      removed++;
      continue;
    }

    oop target_oop = cast_to_oop((HeapWord*)addr);
    RemoteHandle* direct_target = remote_handle_managed_local_oop(_g1h, target_oop)
        ? ensure_handle_for(target_oop, &hab) : nullptr;
    if (direct_target != nullptr) {
      *(uintptr_t*)field_addr =
          G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)direct_target;
      dirty_cards.dirty_field(field_addr);
      TaggedFieldEntry handle_entry;
      handle_entry._field_addr = field_addr;
      handle_entry._handle = direct_target;
      handle_entry._tagged_raw = 0;
      handle_entry._kind = TaggedFieldHandle;
      _tagged_fields[retained++] = handle_entry;
      converted++;
    } else {
      *(uintptr_t*)field_addr = addr;
      dirty_cards.dirty_field(field_addr);
      restored++;
    }
  }

  dirty_cards.flush();
  _tagged_field_count = retained;
  if (restored > 0 || dropped_remote > 0 || converted > 0 ||
      nulled > 0 || dense_handle_retained > 0 ||
      dense_handle_released > 0 || removed > 0 || dirty_cards.dirtied() > 0) {
    log_info(gc)("Dense direct tagged-field cleanup: restored %d local refs, "
                 "dropped %d remote direct refs, "
                 "converted %d stale direct refs, nulled %d invalid direct refs, "
                 "retained %d dense handle refs, released %d dense handle refs, "
                 "removed %d stale entries, %d entries retained, dirtied %d cards",
                 restored, dropped_remote, converted, nulled,
                 dense_handle_retained, dense_handle_released, removed, retained,
                 dirty_cards.dirtied());
  }
  return restored;
}

int G1RemoteMemoryManager::untag_all_heap_refs(WorkerThreads* workers, uint num_workers) {
  class UntagClosure : public BasicOopIterateClosure {
    G1RemoteMemoryManager* _rmm;
    G1CollectedHeap* _g1h;
    G1RemoteRollbackDirtyCards* _dirty_cards;
    int _untagged;
    int _converted;
    int _nulled;
  public:
    UntagClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                 G1RemoteRollbackDirtyCards* dirty_cards)
      : _rmm(rmm), _g1h(g1h), _dirty_cards(dirty_cards),
        _untagged(0), _converted(0), _nulled(0) {}

    virtual void do_oop(oop* p) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) return;
      bool shared = (raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
                    (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT);
      bool direct = (raw & G1_OOP_MANAGED_BIT) != 0 &&
                    (raw & G1_OOP_INDIRECT_BIT) == 0;
      if (!shared && !direct) return;

      if (shared) {
        RemoteHandle* h = (RemoteHandle*)(raw & G1_OOP_ADDR_MASK);
        uintptr_t sa = h->load_state_and_addr_acquire();
        uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
        if (state != REMOTE_HANDLE_LOCAL) return;
        if (_rmm->is_dense_segment_managed_addr((uintptr_t)p)) {
          return;
        }
        uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
        *(uintptr_t*)p = addr;
        if (_dirty_cards != nullptr) {
          _dirty_cards->dirty_field(p);
        }
        _untagged++;
        return;
      }

      uintptr_t addr = raw & G1_OOP_ADDR_MASK;
      if (!g1_remote_oop_is_aligned(addr) ||
          _g1h == nullptr ||
          !_g1h->is_in_reserved((void*)addr)) {
        return;
      }
      RemoteHandle* alias = nullptr;
      DenseDirectTargetState target_state =
          classify_dense_direct_target(_rmm, _g1h, addr, &alias, nullptr);
      if (target_state == DenseDirectTargetRemote) {
        *(uintptr_t*)p = 0;
        if (_dirty_cards != nullptr) {
          _dirty_cards->dirty_field(p);
        }
        _nulled++;
        return;
      }
      if (target_state == DenseDirectTargetAlias && alias != nullptr) {
        *(uintptr_t*)p =
            G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)alias;
        if (_dirty_cards != nullptr) {
          _dirty_cards->dirty_field(p);
        }
        _converted++;
        return;
      }
      if (target_state != DenseDirectTargetLocal) {
        *(uintptr_t*)p = 0;
        if (_dirty_cards != nullptr) {
          _dirty_cards->dirty_field(p);
        }
        _nulled++;
        return;
      }
      if (_rmm->is_dense_segment_managed_addr(addr)) {
        return;
      }
      *(uintptr_t*)p = addr;
      if (_dirty_cards != nullptr) {
        _dirty_cards->dirty_field(p);
      }
      _untagged++;
    }
    virtual void do_oop(narrowOop* p) { }
    int untagged() const { return _untagged; }
    int converted() const { return _converted; }
    int nulled() const { return _nulled; }
  };

  G1RemoteRollbackDirtyCards dirty_cards(_g1h);
  UntagClosure cl(this, _g1h, &dirty_cards);
  class UntagObjectClosure {
    UntagClosure* _cl;
  public:
    UntagObjectClosure(UntagClosure* cl) : _cl(cl) {}
    void do_object(oop obj) {
      obj->oop_iterate(_cl);
    }
  } obj_cl(&cl);
  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();
  for (uint i = 0; i < _g1h->max_reserved_regions(); i++) {
    HeapRegion* hr = _g1h->region_at_or_null(i);
    if (hr == nullptr) continue;
    if (hr->is_empty() || hr->is_free()) continue;
    remote_eviction_scan_region_objects(hr, bitmap, "Untag", &obj_cl);
  }

  dirty_cards.flush();
  if (cl.untagged() > 0 || cl.converted() > 0 || cl.nulled() > 0 ||
      dirty_cards.dirtied() > 0) {
    log_info(gc)("Untag cleanup: restored %d tagged refs to clean oops, "
                 "converted %d stale direct refs, nulled %d invalid direct refs, "
                 "dirtied %d cards",
                 cl.untagged(), cl.converted(), cl.nulled(),
                 dirty_cards.dirtied());
  }
  return cl.untagged();
}

int G1RemoteMemoryManager::verify_no_untagged_refs_to_eviction_set(
    const bool* eviction_set, uint num_regions,
    HeapWord* const* pre_evac_tops,
    bool full_heap) {

  class VerifyTagClosure : public BasicOopIterateClosure {
    G1RemoteMemoryManager* _rmm;
    G1CollectedHeap*       _g1h;
    G1CardTable*           _ct;
    const bool*            _eviction_set;
    uint                   _num_regions;
    HeapWord* const*       _pre_evac_tops;
    bool                   _repair;
    uint                   _repair_limit;
    int                    _missed;
    int                    _repaired;
    int                    _repair_no_handle;
    int                    _repair_untaggable;
    int                    _repair_limit_skipped;
    int                    _stale_alias_repaired;
    int                    _stale_alias_no_handle;
    int                    _stale_alias_nulled;
    int                    _heap_source;
    int                    _root_source;
    int                    _candidate_source;
    int                    _young_source;
    int                    _destination_source;
    int                    _direct_scanned_source;
    int                    _dirty_card_source;
    int                    _clean_old_source;
    int                    _same_region;
    int                    _array_source;
    int                    _obj_array_source;
    int                    _non_array_source;
    int                    _continue_humongous_source;
    uint                   _region_count;
    int*                   _src_region_counts;
    int*                   _target_region_counts;
    oop                    _cur_obj;
    volatile int*          _repair_budget;
    volatile int*          _report_budget;
    RemoteHandleAllocBuffer _hab;

    typedef G1RemoteMemoryManager::TaggedFieldEntry TaggedFieldEntry;
    TaggedFieldEntry*      _local_buf;
    int                    _local_count;
    int                    _local_capacity;

    void local_buf_add_entry(const TaggedFieldEntry& entry) {
      if (_local_count >= _local_capacity) {
        int new_cap = (_local_capacity == 0) ? 256 : _local_capacity * 2;
        TaggedFieldEntry* nb = NEW_C_HEAP_ARRAY(TaggedFieldEntry, new_cap, mtGC);
        if (_local_buf != nullptr) {
          memcpy(nb, _local_buf, _local_count * sizeof(TaggedFieldEntry));
          FREE_C_HEAP_ARRAY(TaggedFieldEntry, _local_buf);
        }
        _local_buf = nb;
        _local_capacity = new_cap;
      }
      _local_buf[_local_count++] = entry;
    }

    void local_buf_add(oop* field_addr, RemoteHandle* h) {
      TaggedFieldEntry entry;
      entry._field_addr = field_addr;
      entry._handle = h;
      entry._tagged_raw = 0;
      entry._kind = G1RemoteMemoryManager::TaggedFieldHandle;
      local_buf_add_entry(entry);
    }

    void dirty_field(oop* p) {
      if (p == nullptr || !_g1h->is_in((void*)p)) {
        return;
      }
      CardTable::CardValue* card = _ct->byte_for((HeapWord*)p);
      if (*card == G1CardTable::g1_young_card_val()) {
        return;
      }
      *card = G1CardTable::dirty_card_val();
      G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
      G1DirtyCardQueue tmp_queue(&dcqs);
      dcqs.enqueue(tmp_queue, card);
      dcqs.flush_queue(tmp_queue);
    }

    void remember_old_cset_source(oop* p, const char* reason) {
      if (p == nullptr) {
        return;
      }
      HeapRegion* src_hr = nullptr;
      if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
        src_hr = _g1h->heap_region_containing_or_null(_cur_obj);
      }
      if (src_hr == nullptr && _g1h->is_in((void*)p)) {
        src_hr = _g1h->heap_region_containing_or_null((void*)p);
      }
      if (src_hr == nullptr || src_hr->is_empty() || src_hr->is_free() ||
          src_hr->is_young() || src_hr->is_continues_humongous()) {
        return;
      }
      _rmm->remember_old_cset_source_hint(src_hr->hrm_index(), reason);
    }

    static bool take_budget(volatile int* budget) {
      if (budget == nullptr) {
        return false;
      }
      int current = Atomic::load(budget);
      while (current > 0) {
        if (Atomic::cmpxchg(budget, current, current - 1) == current) {
          return true;
        }
        current = Atomic::load(budget);
      }
      return false;
    }

    bool can_repair_next() {
      if (_repair_budget != nullptr) {
        return take_budget(_repair_budget);
      }
      return (uint)_repaired < _repair_limit;
    }

    bool should_report_miss() {
      if (_report_budget != nullptr) {
        return take_budget(_report_budget);
      }
      return _missed <= 20;
    }

    bool is_selected(uint idx, const uint* selected, int selected_len) const {
      for (int i = 0; i < selected_len; i++) {
        if (selected[i] == idx) return true;
      }
      return false;
    }

    bool is_destination_region(uint idx, HeapRegion* hr) const {
      return hr != nullptr &&
             idx < _num_regions &&
             _pre_evac_tops != nullptr &&
             _pre_evac_tops[idx] != nullptr &&
             _pre_evac_tops[idx] < hr->top() &&
             !hr->is_empty() &&
             !hr->is_continues_humongous();
    }

    const char* stale_reason(uintptr_t addr, HeapRegion* hr) const {
      if (hr == nullptr) return "NO-HR";
      if (hr->is_free()) return "FREE";
      if (hr->is_evict_guarded()) return "GUARDED";
      if (!_g1h->is_in((void*)addr)) return "OUTSIDE-LIVE";

      oop obj = cast_to_oop((HeapWord*)addr);
      Klass* k = obj->klass_or_null();
      if (k == nullptr) return "NULL-KLASS";
      if (G1CollectedHeap::is_obj_filler(obj)) return "FILLER";
      return nullptr;
    }

    bool repair_stale_alias(oop* p, uintptr_t addr, HeapRegion* hr,
                            const char* reason) {
      if (_cur_obj == nullptr || !_g1h->is_in((void*)p)) {
        return false;
      }
      remember_old_cset_source(p, "verify-stale-alias");

      RemoteHandle* h = _rmm->handle_for_stale_eviction_addr(addr);
      if (h == nullptr) {
        if (_g1h->is_in((void*)p)) {
          *(uintptr_t*)p = 0;
          dirty_field(p);
          _rmm->record_fcr_fixup_null();
          _stale_alias_nulled++;
          if (_stale_alias_nulled <= 20) {
            log_warning(gc)("VERIFY stale-alias: nulled stale field="
                            PTR_FORMAT " raw=" PTR_FORMAT " reason=%s region=%u "
                            "src_obj=" PTR_FORMAT,
                            p2i(p), p2i((void*)addr), reason,
                            hr == nullptr ? 9999 : hr->hrm_index(),
                            p2i((void*)_cur_obj));
          }
          return true;
        } else {
          _stale_alias_no_handle++;
          if (_stale_alias_no_handle <= 20) {
            log_warning(gc)("VERIFY stale-alias: no handle for stale field="
                            PTR_FORMAT " raw=" PTR_FORMAT " reason=%s region=%u "
                            "src_obj=" PTR_FORMAT,
                            p2i(p), p2i((void*)addr), reason,
                            hr == nullptr ? 9999 : hr->hrm_index(),
                            p2i((void*)_cur_obj));
          }
        }
        return false;
      }

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_DEAD) {
        if (_g1h->is_in((void*)p)) {
          *(uintptr_t*)p = 0;
          dirty_field(p);
          _rmm->record_fcr_fixup_null();
          _stale_alias_nulled++;
          return true;
        } else {
          _stale_alias_no_handle++;
        }
        return false;
      }

      *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
      local_buf_add(p, h);
      _stale_alias_repaired++;
      if (_stale_alias_repaired <= 20) {
        log_warning(gc)("VERIFY stale-alias: repaired field=" PTR_FORMAT
                        " raw=" PTR_FORMAT " reason=%s region=%u -> handle="
                        PTR_FORMAT " state=0x%lx src_obj=" PTR_FORMAT,
                        p2i(p), p2i((void*)addr), reason,
                        hr == nullptr ? 9999 : hr->hrm_index(),
                        p2i(h), (unsigned long)state, p2i((void*)_cur_obj));
      }
      return true;
    }

    void log_top_regions(const char* phase, const char* kind, const int* counts) const {
      const int limit = 8;
      uint selected[limit];
      for (int i = 0; i < limit; i++) selected[i] = (uint)-1;

      for (int rank = 0; rank < limit; rank++) {
        int best_count = 0;
        uint best_idx = (uint)-1;
        for (uint i = 0; i < _region_count; i++) {
          if (counts[i] > best_count && !is_selected(i, selected, rank)) {
            best_count = counts[i];
            best_idx = i;
          }
        }
        if (best_count == 0 || best_idx == (uint)-1) break;

        selected[rank] = best_idx;
        HeapRegion* hr = best_idx < _g1h->max_reserved_regions()
          ? _g1h->region_at_or_null(best_idx) : nullptr;
        bool candidate = best_idx < _num_regions && _eviction_set[best_idx];
        bool young = hr != nullptr && hr->is_young();
        bool dest = is_destination_region(best_idx, hr);
        log_warning(gc)("VERIFY detail (%s): top_%s[%d] region=%u count=%d type=%s "
                        "candidate=%s young=%s dest=%s",
                        phase, kind, rank + 1, best_idx, best_count,
                        hr != nullptr ? hr->get_short_type_str() : "?",
                        candidate ? "yes" : "no",
                        young ? "yes" : "no",
                        dest ? "yes" : "no");
      }
    }

  public:
    VerifyTagClosure(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                     const bool* eset, uint nregions,
                     HeapWord* const* pre_evac_tops,
                     bool repair, uint repair_limit,
                     volatile int* repair_budget = nullptr,
                     volatile int* report_budget = nullptr)
      : _rmm(rmm), _g1h(g1h), _ct(g1h->card_table()), _eviction_set(eset),
        _num_regions(nregions), _pre_evac_tops(pre_evac_tops),
        _repair(repair), _repair_limit(repair_limit),
        _missed(0), _repaired(0), _repair_no_handle(0),
        _repair_untaggable(0), _repair_limit_skipped(0),
        _stale_alias_repaired(0), _stale_alias_no_handle(0),
        _stale_alias_nulled(0),
        _heap_source(0), _root_source(0),
        _candidate_source(0), _young_source(0), _destination_source(0),
        _direct_scanned_source(0), _dirty_card_source(0), _clean_old_source(0),
        _same_region(0), _array_source(0), _obj_array_source(0),
        _non_array_source(0), _continue_humongous_source(0),
        _region_count(nregions),
        _src_region_counts(NEW_C_HEAP_ARRAY(int, nregions, mtGC)),
        _target_region_counts(NEW_C_HEAP_ARRAY(int, nregions, mtGC)),
        _cur_obj(nullptr),
        _repair_budget(repair_budget), _report_budget(report_budget), _hab(),
        _local_buf(nullptr), _local_count(0), _local_capacity(0) {
      memset(_src_region_counts, 0, nregions * sizeof(int));
      memset(_target_region_counts, 0, nregions * sizeof(int));
    }

    ~VerifyTagClosure() {
      if (_local_buf != nullptr) {
        FREE_C_HEAP_ARRAY(TaggedFieldEntry, _local_buf);
      }
      FREE_C_HEAP_ARRAY(int, _src_region_counts);
      FREE_C_HEAP_ARRAY(int, _target_region_counts);
    }

    void set_cur_obj(oop obj) { _cur_obj = obj; }

    virtual void do_oop(oop* p) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) return;
      // Shared oops (bits 63+62) already go through a Handle — OK.
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;
      bool dense_direct_ref = _rmm->dense_segments_enabled() &&
                              (raw & G1_OOP_MANAGED_BIT) != 0 &&
                              (raw & G1_OOP_INDIRECT_BIT) == 0;

      uintptr_t target_addr;
      if ((raw >> 63) != 0) {
        target_addr = raw & G1_OOP_ADDR_MASK;
      } else {
        target_addr = raw;
      }
      if (!is_aligned((address)target_addr, HeapWordSize)) return;
      if (!_g1h->is_in_reserved((void*)target_addr)) return;

      if (dense_direct_ref && _rmm->is_dense_segment_remote_addr(target_addr)) {
        return;
      }

      HeapRegion* target_hr =
          _g1h->heap_region_containing_or_null((void*)target_addr);
      const char* stale = stale_reason(target_addr, target_hr);
      if (stale != nullptr) {
        if (!repair_stale_alias(p, target_addr, target_hr, stale)) {
          _missed++;
          _repair_no_handle++;
          if (target_hr != nullptr && target_hr->hrm_index() < _region_count) {
            _target_region_counts[target_hr->hrm_index()]++;
          }
          HeapRegion* src_hr = (_cur_obj != nullptr && _g1h->is_in(_cur_obj))
            ? _g1h->heap_region_containing(_cur_obj) : nullptr;
          if (src_hr != nullptr && src_hr->hrm_index() < _region_count) {
            _heap_source++;
            _src_region_counts[src_hr->hrm_index()]++;
          } else {
            _root_source++;
          }
        }
        return;
      }

      oop target = cast_to_oop((HeapWord*)target_addr);
      uint idx = target_hr->hrm_index();
      if (idx >= _num_regions || !_eviction_set[idx]) return;
      if (dense_direct_ref) return;

      Klass* target_klass = nullptr;
      if (!remote_eviction_valid_local_oop(_g1h, target, target_hr, &target_klass)) {
        _missed++;
        if (idx < _region_count) _target_region_counts[idx]++;
        if (_repair) {
          _repair_untaggable++;
        }
        if (should_report_miss()) {
          log_warning(gc)("VERIFY: invalid target oop field=" PTR_FORMAT
                          " raw=0x%lx -> target=" PTR_FORMAT
                          " in candidate region %u",
                          p2i(p), (unsigned long)raw, p2i((void*)target), idx);
        }
        return;
      }

      // Forwarded objects have already been redirected by the active GC path.
      if (target->is_forwarded()) return;

      HeapRegion* src_hr = (_cur_obj != nullptr && _g1h->is_in(_cur_obj))
        ? _g1h->heap_region_containing(_cur_obj) : nullptr;
      uint src_idx = src_hr != nullptr ? src_hr->hrm_index() : (uint)-1;
      bool src_candidate = src_idx < _num_regions && _eviction_set[src_idx];
      bool src_young = src_hr != nullptr && src_hr->is_young();
      bool src_destination = false;
      if (src_hr != nullptr &&
          src_idx < _num_regions &&
          _pre_evac_tops != nullptr &&
          _pre_evac_tops[src_idx] != nullptr &&
          _pre_evac_tops[src_idx] < src_hr->top() &&
          !src_hr->is_empty() &&
          !src_hr->is_continues_humongous()) {
        src_destination = true;
      }
      bool src_direct = src_candidate || src_young || src_destination;
      bool src_continue_humongous = src_hr != nullptr && src_hr->is_continues_humongous();

      int card_val = -1;
      bool src_dirty_card = false;
      if (src_hr != nullptr && _g1h->is_in((void*)p)) {
        CardTable::CardValue* card = _ct->byte_for((HeapWord*)p);
        card_val = (int)(*card);
        src_dirty_card = (*card == G1CardTable::dirty_card_val());
      }

      Klass* source_klass = (_cur_obj != nullptr) ? _cur_obj->klass_or_null() : nullptr;
      if (source_klass != nullptr && !remote_eviction_valid_klass(source_klass)) {
        source_klass = nullptr;
      }
      bool source_array = source_klass != nullptr && source_klass->is_array_klass();
      bool source_obj_array = source_klass != nullptr && source_klass->is_objArray_klass();
      bool target_type_array = target_klass != nullptr && target_klass->is_typeArray_klass();
      bool target_object = target_klass != nullptr && !target_klass->is_array_klass();
      bool unsafe_obj_array_source =
          source_obj_array &&
          ((!target_type_array || !G1RemoteTagObjArraySources) &&
           (!target_object || !G1RemoteTagObjArrayObjectSources));
      bool heap_source = src_hr != nullptr && _g1h->is_in((void*)p);
      bool untaggable_source =
          !heap_source ||
          source_klass == nullptr ||
          (source_array && (!source_obj_array || unsafe_obj_array_source));

      _missed++;
      if (src_hr != nullptr) {
        _heap_source++;
      } else {
        _root_source++;
      }
      if (src_candidate) _candidate_source++;
      if (src_young) _young_source++;
      if (src_destination) _destination_source++;
      if (src_direct) _direct_scanned_source++;
      if (src_dirty_card) _dirty_card_source++;
      if (!src_direct && src_hr != nullptr && !src_dirty_card && !src_young) _clean_old_source++;
      if (src_idx == idx) _same_region++;
      if (source_array) _array_source++;
      if (source_obj_array) _obj_array_source++;
      if (source_klass != nullptr && !source_array) _non_array_source++;
      if (src_continue_humongous) _continue_humongous_source++;
      if (src_idx < _region_count) _src_region_counts[src_idx]++;
      if (idx < _region_count) _target_region_counts[idx]++;
      if (!src_direct && src_hr != nullptr && !src_dirty_card && !src_young) {
        _rmm->remember_fast_phase_c_source_hint(src_idx);
      }
      if (src_hr != nullptr && !src_young && !src_continue_humongous) {
        _rmm->remember_old_cset_source_hint(src_idx, "verify-missed-candidate");
      }

      if (_repair) {
        if (untaggable_source) {
          _repair_untaggable++;
        } else {
          RemoteHandle* h = _rmm->handle_for(target);
          if (h == nullptr) {
            h = _rmm->ensure_handle_for(target, &_hab);
          }
          if (h != nullptr) {
            if (can_repair_next()) {
              *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
              local_buf_add(p, h);
              _repaired++;
            } else {
              _repair_limit_skipped++;
            }
          } else {
            _repair_no_handle++;
          }
        }
      }

      if (should_report_miss()) {
        log_warning(gc)("VERIFY: untagged ref field=" PTR_FORMAT " -> target=" PTR_FORMAT
                        " in candidate region %u, src_obj=" PTR_FORMAT " klass=%s src_region=%u"
                        " src_type=%s src_candidate=%s src_young=%s src_destination=%s"
                        " src_direct=%s card=0x%02x dirty=%s same_region=%s raw=0x%lx",
                        p2i(p), p2i((void*)target), idx,
                        p2i((void*)_cur_obj),
                        (source_klass != nullptr ? source_klass->external_name() : "root"),
                        (src_hr != nullptr ? src_hr->hrm_index() : 9999),
                        (src_hr != nullptr ? src_hr->get_short_type_str() : "?"),
                        src_candidate ? "yes" : "no",
                        src_young ? "yes" : "no",
                        src_destination ? "yes" : "no",
                        src_direct ? "yes" : "no",
                        (unsigned)card_val & 0xff,
                        src_dirty_card ? "yes" : "no",
                        (src_idx == idx) ? "yes" : "no",
                        (unsigned long)raw);
      }
    }

    virtual void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }

    void merge_from(const VerifyTagClosure& other) {
      _missed += other._missed;
      _repaired += other._repaired;
      _repair_no_handle += other._repair_no_handle;
      _repair_untaggable += other._repair_untaggable;
      _repair_limit_skipped += other._repair_limit_skipped;
      _stale_alias_repaired += other._stale_alias_repaired;
      _stale_alias_no_handle += other._stale_alias_no_handle;
      _stale_alias_nulled += other._stale_alias_nulled;
      _heap_source += other._heap_source;
      _root_source += other._root_source;
      _candidate_source += other._candidate_source;
      _young_source += other._young_source;
      _destination_source += other._destination_source;
      _direct_scanned_source += other._direct_scanned_source;
      _dirty_card_source += other._dirty_card_source;
      _clean_old_source += other._clean_old_source;
      _same_region += other._same_region;
      _array_source += other._array_source;
      _obj_array_source += other._obj_array_source;
      _non_array_source += other._non_array_source;
      _continue_humongous_source += other._continue_humongous_source;
      for (uint i = 0; i < _region_count; i++) {
        _src_region_counts[i] += other._src_region_counts[i];
        _target_region_counts[i] += other._target_region_counts[i];
      }
    }

    void flush_repaired_fields() {
      for (int i = 0; i < _local_count; i++) {
        _rmm->add_tagged_field_entry(_local_buf[i]);
      }
    }

    int missed() const { return _missed; }
    int repaired() const { return _repaired; }
    int stale_alias_repaired() const { return _stale_alias_repaired; }
    int stale_alias_no_handle() const { return _stale_alias_no_handle; }
    int unrepaired() const {
      int unrepaired_count = _missed - _repaired;
      return unrepaired_count > 0 ? unrepaired_count : 0;
    }
    void log_summary(const char* phase, int phase_missed) const {
      if (phase_missed <= 0) return;
      log_warning(gc)("VERIFY detail (%s): missed=%d heap_src=%d root_src=%d "
                      "direct_src=%d candidate_src=%d young_src=%d dest_src=%d "
                      "dirty_card_src=%d clean_old_src=%d same_region=%d "
                      "array_src=%d obj_array_src=%d non_array_src=%d cont_hum_src=%d",
                      phase, phase_missed, _heap_source, _root_source,
                      _direct_scanned_source, _candidate_source, _young_source,
                      _destination_source, _dirty_card_source, _clean_old_source,
                      _same_region, _array_source, _obj_array_source,
                      _non_array_source, _continue_humongous_source);
      if (_repair) {
        log_warning(gc)("VERIFY repair (%s): repaired=%d unrepaired=%d "
                        "no_handle=%d untaggable=%d limit_skipped=%d limit=%u",
                        phase, _repaired, unrepaired(), _repair_no_handle,
                        _repair_untaggable, _repair_limit_skipped, _repair_limit);
      }
      if (_stale_alias_repaired > 0 || _stale_alias_no_handle > 0 ||
          _stale_alias_nulled > 0) {
        log_warning(gc)("VERIFY stale-alias (%s): repaired=%d nulled=%d no_handle=%d",
                        phase, _stale_alias_repaired, _stale_alias_nulled,
                        _stale_alias_no_handle);
      }
      log_top_regions(phase, "src", _src_region_counts);
      log_top_regions(phase, "target", _target_region_counts);
    }
  };

  bool repair_misses = G1RemoteUseFastPhaseC && G1RemoteRepairFastPhaseCMisses;
  VerifyTagClosure cl(this, _g1h, eviction_set, num_regions, pre_evac_tops,
                      repair_misses, G1RemoteFastPhaseCRepairMissLimit);
  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();

  class VerifyObjectClosure {
    VerifyTagClosure* _cl;
  public:
    VerifyObjectClosure(VerifyTagClosure* cl) : _cl(cl) {}
    void do_object(oop obj) {
      _cl->set_cur_obj(obj);
      obj->oop_iterate(_cl);
    }
  };

  class VerifySourceSelector {
    G1CollectedHeap*       _g1h;
    G1CardTable*           _ct;
    uint                   _num_regions;
    bool*                  _selected;
    bool                   _requires_full_heap;
    int                    _selected_regions;
    int                    _candidate_regions;
    int                    _young_regions;
    int                    _destination_regions;
    int                    _inbound_summary_regions;
    int                    _source_hint_regions;
    int                    _old_cset_source_hint_regions;
    int                    _neighbor_regions;
    int                    _old_prefix_regions;
    int                    _rset_cards;
    int                    _rset_source_regions;
    int                    _dirty_cards;
    int                    _dirty_source_regions;
    int                    _incomplete_rsets;

    bool select_region(uint region_idx) {
      if (region_idx >= _num_regions) {
        return false;
      }
      if (!_selected[region_idx]) {
        _selected[region_idx] = true;
        _selected_regions++;
        return true;
      }
      return false;
    }

    bool select_card_source(uint card_idx, bool rset_card) {
      HeapWord* card_start = _ct->addr_for(_ct->byte_for_index(card_idx));
      if (!_g1h->is_in_reserved(card_start)) {
        return false;
      }
      HeapRegion* source_hr = _g1h->heap_region_containing_or_null(card_start);
      if (source_hr == nullptr || source_hr->is_empty() || source_hr->is_free()) {
        return false;
      }
      uint src_idx = source_hr->hrm_index();
      bool added = select_region(src_idx);
      if (added && rset_card) {
        _rset_source_regions++;
      }
      return added;
    }

  public:
    VerifySourceSelector(G1CollectedHeap* g1h,
                         uint num_regions,
                         bool* selected)
      : _g1h(g1h), _ct(g1h->card_table()),
        _num_regions(num_regions), _selected(selected),
        _requires_full_heap(false), _selected_regions(0),
        _candidate_regions(0), _young_regions(0), _destination_regions(0),
        _inbound_summary_regions(0), _source_hint_regions(0),
        _old_cset_source_hint_regions(0), _neighbor_regions(0),
        _old_prefix_regions(0),
        _rset_cards(0), _rset_source_regions(0),
        _dirty_cards(0), _dirty_source_regions(0),
        _incomplete_rsets(0) {}

    bool start_iterate(uint tag, uint region_idx) { return true; }

    void do_card(uint card_idx) {
      _rset_cards++;
      select_card_source(card_idx, true /* rset_card */);
    }

    void do_card_range(uint start_card_idx, uint length) {
      for (uint i = 0; i < length; i++) {
        do_card(start_card_idx + i);
      }
    }

    void select_candidate(HeapRegion* hr) {
      if (hr == nullptr) {
        return;
      }
      select_region(hr->hrm_index());
      _candidate_regions++;

      HeapRegionRemSet* rem_set = hr->rem_set();
      if (rem_set == nullptr) {
        _incomplete_rsets++;
        return;
      }
      if (!rem_set->is_complete()) {
        // Incomplete RSets are common during the pressure-driven dense path.
        // Treat them as a missing signal and rely on the other bounded source
        // sets instead of turning the performance verifier back into a
        // full-heap STW scan.
        _incomplete_rsets++;
        return;
      }
      if (!rem_set->is_empty()) {
        rem_set->iterate_for_merge(*this);
      }
    }

    void select_young(uint region_idx) {
      if (select_region(region_idx)) {
        _young_regions++;
      }
    }

    void select_destination(uint region_idx) {
      if (select_region(region_idx)) {
        _destination_regions++;
      }
    }

    void select_inbound_summary(uint region_idx) {
      if (select_region(region_idx)) {
        _inbound_summary_regions++;
      }
    }

    void select_source_hint(uint region_idx) {
      if (select_region(region_idx)) {
        _source_hint_regions++;
      }
    }

    void select_old_cset_source_hint(uint region_idx) {
      if (select_region(region_idx)) {
        _old_cset_source_hint_regions++;
      }
    }

    void select_neighbor(uint region_idx) {
      if (select_region(region_idx)) {
        _neighbor_regions++;
      }
    }

    void select_old_prefix(uint region_idx) {
      if (select_region(region_idx)) {
        _old_prefix_regions++;
      }
    }

    int scan_dirty_cards_in_region(HeapRegion* source_hr) {
      if (source_hr == nullptr || source_hr->is_empty() || source_hr->is_free()) {
        return 0;
      }
      if (source_hr->is_young() || source_hr->is_continues_humongous()) {
        return 0;
      }
      if (source_hr->bottom() >= source_hr->top()) {
        return 0;
      }

      CardTable::CardValue* card = _ct->byte_for(source_hr->bottom());
      CardTable::CardValue* end_card = _ct->byte_for(source_hr->top() - 1) + 1;
      int dirty_cards = 0;
      while (card < end_card) {
        if (*card == G1CardTable::dirty_card_val()) {
          dirty_cards++;
        }
        card++;
      }
      if (dirty_cards > 0 && select_region(source_hr->hrm_index())) {
        _dirty_source_regions++;
      }
      _dirty_cards += dirty_cards;
      return dirty_cards;
    }

    bool requires_full_heap() const { return _requires_full_heap; }
    int selected_regions() const { return _selected_regions; }
    int incomplete_rsets() const { return _incomplete_rsets; }

    void log_summary(bool fallback_full_heap) const {
      log_info(gc)("Phase C.5 targeted sources: selected=%d candidates=%d young=%d "
                   "destinations=%d inbound=%d hints=%d old_cset_hints=%d "
                   "neighbors=%d old-prefix=%d rset_cards=%d rset_sources=%d "
                   "dirty_cards=%d dirty_sources=%d incomplete_rsets=%d mode=%s",
                   _selected_regions, _candidate_regions, _young_regions,
                   _destination_regions, _inbound_summary_regions,
                   _source_hint_regions, _old_cset_source_hint_regions,
                   _neighbor_regions, _old_prefix_regions, _rset_cards,
                   _rset_source_regions, _dirty_cards, _dirty_source_regions,
                   _incomplete_rsets,
                   fallback_full_heap ? "fallback-full" : "targeted");
    }
  };

  bool* selected_sources = nullptr;
  bool targeted_heap_verify = !full_heap;
  if (targeted_heap_verify) {
    selected_sources = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
    memset(selected_sources, 0, num_regions * sizeof(bool));
    VerifySourceSelector selector(_g1h, num_regions, selected_sources);

    for (uint i = 0; i < num_regions; i++) {
      HeapRegion* hr = _g1h->region_at_or_null(i);
      if (hr == nullptr) {
        continue;
      }

      if (eviction_set[i]) {
        selector.select_candidate(hr);
        continue;
      }

      if (hr->is_young()) {
        selector.select_young(i);
        continue;
      }

      if (pre_evac_tops != nullptr &&
          pre_evac_tops[i] != nullptr &&
          pre_evac_tops[i] < hr->top() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        selector.select_destination(i);
        continue;
      }

      if (is_inbound_source_for_eviction_set(i, eviction_set, num_regions)) {
        selector.select_inbound_summary(i);
        continue;
      }

      if (is_fast_phase_c_source_hint(i)) {
        selector.select_source_hint(i);
        continue;
      }

      if (is_old_cset_source_hint(i)) {
        selector.select_old_cset_source_hint(i);
        continue;
      }

      if (remote_fast_phase_c_is_candidate_neighbor(i, eviction_set, num_regions) &&
          hr->is_old() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        selector.select_neighbor(i);
        continue;
      }

      if (G1RemoteFastPhaseCOldPrefixRegions > 0 &&
          i < G1RemoteFastPhaseCOldPrefixRegions &&
          hr->is_old() &&
          !hr->is_empty() &&
          !hr->is_continues_humongous()) {
        selector.select_old_prefix(i);
        continue;
      }

      selector.scan_dirty_cards_in_region(hr);
    }

    if (selector.requires_full_heap()) {
      targeted_heap_verify = false;
    }
    selector.log_summary(!targeted_heap_verify);
  }

  // 1. Verify heap: use the same conservative walk as Phase C tagging so the
  // verifier can catch bitmap/parser blind spots instead of repeating them.
  WorkerThreads* verify_workers = _g1h->workers();
  uint active_workers = verify_workers != nullptr ? verify_workers->active_workers() : 0;
  if (G1RemoteParallelVerifyEvictionRefs &&
      verify_workers != nullptr &&
      active_workers > 1) {
    class VerifyHeapRefsTask : public WorkerTask {
      G1RemoteMemoryManager* _rmm;
      G1CollectedHeap*       _g1h;
      const bool*            _eviction_set;
      const bool*            _selected_sources;
      uint                   _num_regions;
      HeapWord* const*       _pre_evac_tops;
      const G1CMBitMap*      _bitmap;
      bool                   _repair;
      uint                   _repair_limit;
      VerifyTagClosure*      _summary;
      HeapRegionClaimer      _claimer;
      volatile int           _merge_lock;
      volatile int           _repair_budget;
      volatile int           _report_budget;

      void merge_lock() {
        while (Atomic::cmpxchg(&_merge_lock, 0, 1) != 0) { /* spin */ }
      }

      void merge_unlock() {
        Atomic::release_store(&_merge_lock, 0);
      }

    public:
      VerifyHeapRefsTask(G1RemoteMemoryManager* rmm,
                         G1CollectedHeap* g1h,
                         const bool* eviction_set,
                         const bool* selected_sources,
                         uint num_regions,
                         HeapWord* const* pre_evac_tops,
                         const G1CMBitMap* bitmap,
                         bool repair,
                         uint repair_limit,
                         VerifyTagClosure* summary,
                         uint num_workers)
        : WorkerTask("Verify remote eviction refs"),
          _rmm(rmm), _g1h(g1h), _eviction_set(eviction_set),
          _selected_sources(selected_sources),
          _num_regions(num_regions), _pre_evac_tops(pre_evac_tops),
          _bitmap(bitmap), _repair(repair), _repair_limit(repair_limit),
          _summary(summary), _claimer(num_workers), _merge_lock(0),
          _repair_budget((int)repair_limit), _report_budget(20) {}

      void work(uint worker_id) {
        VerifyTagClosure worker_cl(_rmm, _g1h, _eviction_set, _num_regions,
                                   _pre_evac_tops, _repair, _repair_limit,
                                   &_repair_budget, &_report_budget);
        VerifyObjectClosure obj_cl(&worker_cl);

        for (uint i = _claimer.offset_for_worker(worker_id);
             i < _claimer.n_regions();
             i++) {
          if (!_claimer.claim_region(i)) continue;
          if (_selected_sources != nullptr &&
              (i >= _num_regions || !_selected_sources[i])) {
            continue;
          }
          HeapRegion* hr = _g1h->region_at_or_null(i);
          if (hr == nullptr) continue;
          if (hr->is_empty() || hr->is_free()) continue;
          if (hr->is_continues_humongous()) continue;
          remote_eviction_scan_region_objects(hr, _bitmap, "VERIFY", &obj_cl);
        }

        merge_lock();
        worker_cl.flush_repaired_fields();
        _summary->merge_from(worker_cl);
        merge_unlock();
      }
    };

    VerifyHeapRefsTask task(this, _g1h, eviction_set,
                            targeted_heap_verify ? selected_sources : nullptr,
                            num_regions, pre_evac_tops, bitmap, repair_misses,
                            G1RemoteFastPhaseCRepairMissLimit, &cl,
                            active_workers);
    verify_workers->run_task(&task, active_workers);
  } else {
    for (uint i = 0; i < _g1h->max_reserved_regions(); i++) {
      if (targeted_heap_verify &&
          (i >= num_regions || selected_sources == nullptr || !selected_sources[i])) {
        continue;
      }
      HeapRegion* hr = _g1h->region_at_or_null(i);
      if (hr == nullptr) continue;
      if (hr->is_empty() || hr->is_free()) continue;
      if (hr->is_continues_humongous()) continue;
      VerifyObjectClosure obj_cl(&cl);
      remote_eviction_scan_region_objects(hr, bitmap, "VERIFY", &obj_cl);
    }
    cl.flush_repaired_fields();
  }

  int heap_missed = cl.missed();
  int heap_unrepaired = cl.unrepaired();
  cl.log_summary("heap", heap_missed);

  // 2. Verify roots (informational only — root refs are handled by
  // root-catch relocation and Pre-E guard, not by Phase C tagging).
  cl.set_cur_obj(nullptr);
  Threads::oops_do(&cl, nullptr);
  JNIHandles::oops_do(&cl);
  OopStorageSet::strong_oops_do(&cl);
  for (auto id : EnumRange<OopStorageSet::WeakId>()) {
    OopStorageSet::storage(id)->oops_do(&cl);
  }
  oops_do_remote_anchors(&cl);
  {
    CLDToOopClosure cld_cl(&cl, ClassLoaderData::_claim_none);
    ClassLoaderDataGraph::cld_do(&cld_cl);
  }
  _g1h->ref_processor_cm()->weak_oops_do(&cl);

  int root_missed = cl.missed() - heap_missed;
  if (heap_unrepaired > 0) {
    log_warning(gc)("VERIFY: %d untagged HEAP refs to eviction candidates AFTER tagging! "
                    "(%d repaired)",
                    heap_unrepaired, cl.repaired());
  } else if (cl.repaired() > 0) {
    log_info(gc)("VERIFY: repaired %d heap refs to eviction candidates after Fast Phase C",
                 cl.repaired());
  }
  if (root_missed > 0) {
    log_info(gc)("VERIFY: %d root refs to candidates (handled by Pre-E guard, not Phase C)",
                 root_missed);
  }
  if (selected_sources != nullptr) {
    FREE_C_HEAP_ARRAY(bool, selected_sources);
  }
  return heap_unrepaired;
}

int G1RemoteMemoryManager::verify_no_stale_refs_to_freed_regions() {
  class StaleRefSweepClosure : public BasicOopIterateClosure {
    G1CollectedHeap* _g1h;
    int              _stale;
    int              _raw_guarded;
    oop              _cur_obj;
    bool             _is_root;
    const char*      _root_kind;
  public:
    StaleRefSweepClosure(G1CollectedHeap* g1h)
      : _g1h(g1h), _stale(0), _raw_guarded(0), _cur_obj(nullptr),
        _is_root(false), _root_kind("ROOT") {}

    void set_cur_obj(oop obj) {
      _cur_obj = obj;
      _is_root = false;
      _root_kind = "ROOT";
    }

    void set_root_mode(const char* root_kind) {
      _cur_obj = nullptr;
      _is_root = true;
      _root_kind = root_kind;
    }

    bool decode_raw_value(uintptr_t raw, uintptr_t* addr) {
      if (raw == 0) return false;
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
        return false;
      }
      *addr = ((raw & G1_OOP_TAG_MASK) != 0) ? (raw & G1_OOP_ADDR_MASK) : raw;
      if ((*addr & (HeapWordSize - 1)) != 0) return false;
      return true;
    }

    virtual void do_oop(oop* p) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw == 0) return;
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

      bool dense_direct_ref =
          (raw & G1_OOP_MANAGED_BIT) != 0 &&
          (raw & G1_OOP_INDIRECT_BIT) == 0;
      oop target;
      if ((raw >> 63) != 0) {
        target = cast_to_oop(raw & G1_OOP_ADDR_MASK);
      } else {
        target = cast_to_oop(raw);
      }
      if (!_g1h->is_in_reserved(target)) {
        // Non-heap, non-tagged value in an oop slot — heap corruption
        _stale++;
        if (_stale <= 50) {
          const char* src_kind = _is_root ? _root_kind : "HEAP";
          const char* src_klass = "?";
          uint src_region = 9999;
          if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
            Klass* sk = _cur_obj->klass_or_null();
            if (sk != nullptr) src_klass = sk->external_name();
            src_region = _g1h->heap_region_containing(_cur_obj)->hrm_index();
          }
          uint32_t off = (_cur_obj != nullptr) ?
            (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_cur_obj)) : 0;
          log_warning(gc)("CORRUPT-OOP [%s]: field=" PTR_FORMAT " raw=0x%lx NOT IN HEAP"
                          " (src_obj=" PTR_FORMAT " klass=%s region=%u offset=%u)",
                          src_kind, p2i(p), (unsigned long)raw,
                          p2i((void*)_cur_obj), src_klass, src_region, off);
          // Decode as markWord to detect header-scribble (Codex H6 hypothesis)
          uintptr_t mw_lock = raw & 0x3;
          uintptr_t mw_age = (raw >> 3) & 0xF;
          uintptr_t mw_hash = (raw >> 8) & 0x7FFFFFF;
          if (mw_lock == 0x1 && raw > 0xFF) {
            log_warning(gc)("  ^^ LOOKS LIKE MARK WORD: lock=unlocked age=%u hash=0x%07x"
                            " upper=0x%lx — possible header copied into oop slot",
                            (unsigned)mw_age, (unsigned)mw_hash,
                            (unsigned long)(raw >> 35));
          }
        }
        return;
      }

      HeapRegion* hr = _g1h->heap_region_containing_or_null(target);
      const char* stale_reason = nullptr;

      G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
      if (dense_direct_ref &&
          rmm != nullptr &&
          rmm->is_dense_segment_remote_addr((uintptr_t)target)) {
        return;
      }

      if (hr == nullptr) {
        stale_reason = "NO-HR";
      } else if (hr->is_free()) {
        stale_reason = "FREE";
      } else if (hr->is_evict_guarded()) {
        stale_reason = "GUARDED";
      } else {
        Klass* k = target->klass_or_null();
        if (k == nullptr) {
          stale_reason = "NULL-KLASS";
        } else if (G1CollectedHeap::is_obj_filler(target)) {
          stale_reason = "FILLER";
        }
      }

      if (stale_reason != nullptr) {
        _stale++;
        if (_stale <= 50) {
          const char* src_kind = _is_root ? _root_kind : "HEAP";
          HeapRegion* src_hr = nullptr;
          const char* src_klass = "?";
          uint src_region = 9999;
          bool has_tagged_fields = false;
          if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
            src_hr = _g1h->heap_region_containing(_cur_obj);
            src_region = src_hr->hrm_index();
            Klass* sk = _cur_obj->klass_or_null();
            if (sk != nullptr) src_klass = sk->external_name();
            // Check if source obj has any tagged fields (indicates FCR-fetched)
            HeapWord* obj_start = (HeapWord*)_cur_obj;
            HeapWord* obj_end = obj_start + _cur_obj->size();
            for (HeapWord* w = obj_start + 2; w < obj_end; w++) {
              uintptr_t v = *(uintptr_t*)w;
              if (v & G1_OOP_MANAGED_BIT) { has_tagged_fields = true; break; }
            }
          }
          G1CardTable* ct = _g1h->card_table();
          G1CardTable::CardValue card_val = 0xff;
          if (!_is_root) {
            card_val = *ct->byte_for((HeapWord*)p);
          }
          log_warning(gc)("STALE-REF-SWEEP [%s]: field=" PTR_FORMAT " raw=0x%lx -> target="
                          PTR_FORMAT " in %s region %u (src_obj=" PTR_FORMAT " klass=%s region=%u"
                          " card=0x%02x fcr_tagged=%s src_type=%s)",
                          src_kind, p2i(p), (unsigned long)raw,
                          p2i((void*)target),
                          stale_reason,
                          hr == nullptr ? 9999 : hr->hrm_index(),
                          p2i((void*)_cur_obj), src_klass, src_region,
                          (unsigned)card_val,
                          has_tagged_fields ? "yes" : "no",
                          src_hr != nullptr ? src_hr->get_short_type_str() : "?");

          // Dump all oop-width slots on the same card as this stale ref
          if (_cur_obj != nullptr && _g1h->is_in(_cur_obj)) {
            HeapWord* card_start = ct->addr_for(ct->byte_for((HeapWord*)p));
            HeapWord* card_end = card_start + G1CardTable::card_size_in_words();
            HeapWord* obj_start = (HeapWord*)_cur_obj;
            HeapWord* obj_end = obj_start + _cur_obj->size();
            // Clamp to object bounds (skip header: mark + klass = 2 words)
            HeapWord* scan_start = MAX2(card_start, obj_start + 2);
            HeapWord* scan_end = MIN2(card_end, obj_end);
            int n_null = 0, n_tagged = 0, n_live = 0, n_stale_card = 0;
            for (HeapWord* w = scan_start; w < scan_end; w++) {
              uintptr_t v = *(uintptr_t*)w;
              if (v == 0) { n_null++; continue; }
              if ((v & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
                  (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) { n_tagged++; continue; }
              oop slot_target;
              if ((v >> 63) != 0) { slot_target = cast_to_oop(v & G1_OOP_ADDR_MASK); }
              else { slot_target = cast_to_oop(v); }
              if (!_g1h->is_in(slot_target)) { continue; }
              HeapRegion* slot_hr = _g1h->heap_region_containing(slot_target);
              bool slot_stale = slot_hr == nullptr || slot_hr->is_free() ||
                                slot_hr->is_evict_guarded();
              if (!slot_stale) {
                Klass* slot_k = slot_target->klass_or_null();
                slot_stale = slot_k == nullptr ||
                             G1CollectedHeap::is_obj_filler(slot_target);
              }
              if (slot_stale) {
                n_stale_card++;
              } else {
                n_live++;
              }
            }
            log_warning(gc)("STALE-REF-SWEEP card dump: card=[" PTR_FORMAT "," PTR_FORMAT
                            ") obj=[" PTR_FORMAT "," PTR_FORMAT
                            ") scan=[" PTR_FORMAT "," PTR_FORMAT
                            "): %d null, %d tagged, %d live, %d stale (of %d slots)",
                            p2i(card_start), p2i(card_end),
                            p2i(obj_start), p2i(obj_end),
                            p2i(scan_start), p2i(scan_end),
                            n_null, n_tagged, n_live, n_stale_card,
                            (int)(scan_end - scan_start));
          }
        }
      }
    }

    void scan_raw_payload(oop obj) {
      if (obj == nullptr || obj->is_typeArray()) return;

      size_t obj_words = obj->size();
      size_t header_words = obj->is_objArray() ?
        (size_t)arrayOopDesc::header_size(T_OBJECT) :
        (size_t)oopDesc::header_size();
      if (obj_words <= header_words) return;

      HeapWord* obj_start = cast_from_oop<HeapWord*>(obj);
      HeapWord* obj_end = obj_start + obj_words;
      for (HeapWord* slot = obj_start + header_words; slot < obj_end; slot++) {
        uintptr_t raw = *(uintptr_t*)slot;
        uintptr_t addr = 0;
        if (!decode_raw_value(raw, &addr)) continue;
        void* target_addr = (void*)addr;
        if (!_g1h->is_in_reserved(target_addr)) continue;

        HeapRegion* target_hr = _g1h->heap_region_containing_or_null(target_addr);
        if (target_hr == nullptr) continue;
        if (!target_hr->is_free() && !target_hr->is_evict_guarded()) continue;

        _raw_guarded++;
        if (_raw_guarded <= 80) {
          HeapRegion* src_hr = _g1h->heap_region_containing_or_null(obj);
          const char* src_klass = "?";
          Klass* sk = obj->klass_or_null();
          if (sk != nullptr) src_klass = sk->external_name();

          G1CardTable* ct = _g1h->card_table();
          G1CardTable::CardValue card_val = *ct->byte_for(slot);
          uint32_t off = (uint32_t)((uintptr_t)slot - cast_from_oop<uintptr_t>(obj));
          log_warning(gc)("PROTECTED-RAW-REF [HEAP-PAYLOAD]: slot=" PTR_FORMAT
                          " raw=0x%lx decoded=" PTR_FORMAT
                          " -> %s region %u (src_obj=" PTR_FORMAT
                          " klass=%s region=%u src_type=%s offset=%u card=0x%02x)",
                          p2i(slot), (unsigned long)raw, p2i(target_addr),
                          target_hr->is_evict_guarded() ? "GUARDED" : "FREE",
                          target_hr->hrm_index(),
                          p2i((void*)obj), src_klass,
                          src_hr != nullptr ? src_hr->hrm_index() : 9999,
                          src_hr != nullptr ? src_hr->get_short_type_str() : "?",
                          off, (unsigned)card_val);
        }
      }
    }

    virtual void do_oop(narrowOop* p) {}
    int stale() const { return _stale; }
    int raw_guarded() const { return _raw_guarded; }
  };

  Ticks start = Ticks::now();
  StaleRefSweepClosure cl(_g1h);
  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();
  int regions_scanned = 0;

  class StaleObjectClosure {
    StaleRefSweepClosure* _cl;
  public:
    StaleObjectClosure(StaleRefSweepClosure* cl) : _cl(cl) {}
    void do_object(oop obj) {
      _cl->set_cur_obj(obj);
      obj->oop_iterate(_cl);
      _cl->scan_raw_payload(obj);
    }
  };

  for (uint i = 0; i < _g1h->max_reserved_regions(); i++) {
    HeapRegion* hr = _g1h->region_at_or_null(i);
    if (hr == nullptr) continue;
    if (hr->is_empty() || hr->is_free()) continue;
    if (hr->is_continues_humongous()) continue;
    if (hr->is_evict_guarded()) continue;
    StaleObjectClosure obj_cl(&cl);
    remote_eviction_scan_region_objects(hr, bitmap, "STALE-REF-SWEEP", &obj_cl);
    regions_scanned++;
  }

  int heap_stale = cl.stale();
  int heap_raw_guarded = cl.raw_guarded();

  cl.set_root_mode("ROOT-Threads");
  Threads::oops_do(&cl, nullptr);
  cl.set_root_mode("ROOT-JNI");
  JNIHandles::oops_do(&cl);
  cl.set_root_mode("ROOT-OopStorageStrong");
  OopStorageSet::strong_oops_do(&cl);
  {
    cl.set_root_mode("ROOT-CLDG");
    CLDToOopClosure cld_cl(&cl, ClassLoaderData::_claim_none);
    ClassLoaderDataGraph::cld_do(&cld_cl);
  }
  {
    cl.set_root_mode("ROOT-CodeCache");
    CodeBlobToOopClosure code_cl(&cl, false);
    CodeCache::blobs_do(&code_cl);
  }
  cl.set_root_mode("ROOT-Weak");
  _g1h->ref_processor_cm()->weak_oops_do(&cl);

  int root_stale = cl.stale() - heap_stale;
  int raw_guarded = cl.raw_guarded();
  double elapsed_ms = (Ticks::now() - start).seconds() * 1000.0;

  if (cl.stale() > 0 || raw_guarded > 0) {
    log_warning(gc)("STALE-REF-SWEEP: %d stale oop refs found (%d heap, %d root), "
                    "%d protected raw payload words (%d heap) in %.1fms "
                    "(%d regions scanned)",
                    cl.stale(), heap_stale, root_stale,
                    raw_guarded, heap_raw_guarded, elapsed_ms, regions_scanned);
  } else {
    log_info(gc)("STALE-REF-SWEEP: clean (0 stale refs, 0 protected raw payload words) "
                 "in %.1fms (%d regions scanned)",
                 elapsed_ms, regions_scanned);
  }
  return cl.stale() + raw_guarded;
}

bool G1RemoteMemoryManager::validate_local_handle_addr(RemoteHandle* h,
                                                       const char* context,
                                                       int* invalid_count,
                                                       int log_limit) {
  if (h == nullptr) return false;

  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
  if (state != REMOTE_HANDLE_LOCAL) return false;

  uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
  HeapRegion* hr = nullptr;
  const char* reason = nullptr;

  if (addr == 0) {
    reason = "NULL";
  } else if (!_g1h->is_in_reserved((void*)addr)) {
    reason = "NOT IN HEAP";
  } else {
    hr = _g1h->heap_region_containing_or_null((void*)addr);
    if (hr == nullptr) {
      reason = "NO REGION";
    } else if (hr->is_evict_guarded()) {
      reason = "GUARDED";
    } else if (hr->is_free()) {
      reason = "FREE";
    } else if (hr->is_empty()) {
      reason = "EMPTY";
    } else if (hr->is_continues_humongous()) {
      reason = "CONT-HUMONGOUS";
    } else if ((HeapWord*)addr < hr->bottom() || (HeapWord*)addr >= hr->top()) {
      reason = "OUTSIDE-TOP";
    } else if (!_g1h->is_in((void*)addr)) {
      reason = "NOT IN LIVE HEAP";
    } else {
      oop obj = cast_to_oop((HeapWord*)addr);
      Klass* k = obj->klass_or_null_acquire();
      if (!remote_eviction_valid_klass(k)) {
        reason = "BAD-KLASS";
      } else if (G1CollectedHeap::is_obj_filler(obj)) {
        reason = "FILLER";
      } else {
        Klass* size_k = obj->klass_or_null_acquire();
        if (size_k != k) {
          reason = "KLASS-CHANGED";
        } else {
          size_t word_size = obj->size_given_klass(size_k);
          if (word_size < (size_t)MinObjAlignment ||
              !is_object_aligned(word_size) ||
              word_size > (size_t)(hr->top() - (HeapWord*)addr) ||
              word_size > (size_t)(hr->end() - (HeapWord*)addr)) {
            reason = "BAD-SIZE";
          }
        }
      }
    }
  }

  if (reason == nullptr) {
    return true;
  }

  int ordinal = 1;
  if (invalid_count != nullptr) {
    ordinal = ++(*invalid_count);
  }
  if (log_limit < 0 || ordinal <= log_limit) {
    log_warning(gc)("%s: stale LOCAL handle=" PTR_FORMAT " addr=" PTR_FORMAT
                    " %s region=%u — marking DEAD (dormant=%d rc=%u)",
                    context == nullptr ? "STALE-LOCAL-HANDLE" : context,
                    p2i(h), p2i((void*)addr), reason,
                    hr == nullptr ? 9999 : hr->hrm_index(),
                    h->is_dormant() ? 1 : 0, h->remote_refcount());
  }
  mark_handle_dead(h);
  return false;
}

bool G1RemoteMemoryManager::validate_anchor_addr(RemoteHandle* h) {
  return validate_local_handle_addr(h, "STALE-ANCHOR");
}

int G1RemoteMemoryManager::count_local_handles_in_region(HeapRegion* hr, int log_limit) {
  if (hr == nullptr) return 0;

  uintptr_t bottom = (uintptr_t)hr->bottom();
  uintptr_t end = (uintptr_t)hr->end();
  class CountLocalHandleClosure {
    uintptr_t _bottom;
    uintptr_t _end;
    uint _region_idx;
    int _log_limit;
    int _count;

  public:
    CountLocalHandleClosure(uintptr_t bottom, uintptr_t end, uint region_idx, int log_limit)
      : _bottom(bottom), _end(end), _region_idx(region_idx),
        _log_limit(log_limit), _count(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) {
        uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
        if (addr >= _bottom && addr < _end) {
          _count++;
          if (_count <= _log_limit) {
            log_warning(gc)("LOCAL handle blocks eviction free: region=%u handle=" PTR_FORMAT
                            " local=" PTR_FORMAT
                            " listed=%d dormant=%d rc=%u",
                            _region_idx, p2i(h), addr,
                            h->_local_listed ? 1 : 0,
                            h->is_dormant() ? 1 : 0, h->remote_refcount());
          }
        }
      }
    }

    int count() const { return _count; }
  };

  CountLocalHandleClosure cl(bottom, end, hr->hrm_index(), log_limit);
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      cl.do_handle(e->_handle);
    }
  }
  return cl.count();
}

int G1RemoteMemoryManager::count_local_handles_in_regions(
    const bool* eviction_candidates,
    const bool* region_complete,
    const int* region_count,
    uint num_regions,
    int* blockers_by_region,
    int log_limit) {
  if (eviction_candidates == nullptr || region_complete == nullptr ||
      region_count == nullptr || blockers_by_region == nullptr || num_regions == 0) {
    return 0;
  }

  memset(blockers_by_region, 0, num_regions * sizeof(int));

  class CountLocalHandlesInRegionsClosure {
    G1RemoteMemoryManager* _rmm;
    const bool* _eviction_candidates;
    const bool* _region_complete;
    const int* _region_count;
    uint _num_regions;
    int* _blockers_by_region;
    int _log_limit;
    int _total_blockers;

  public:
    CountLocalHandlesInRegionsClosure(G1RemoteMemoryManager* rmm,
                                      const bool* eviction_candidates,
                                      const bool* region_complete,
                                      const int* region_count,
                                      uint num_regions,
                                      int* blockers_by_region,
                                      int log_limit)
      : _rmm(rmm), _eviction_candidates(eviction_candidates),
        _region_complete(region_complete), _region_count(region_count),
        _num_regions(num_regions), _blockers_by_region(blockers_by_region),
        _log_limit(log_limit), _total_blockers(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      if ((sa & REMOTE_HANDLE_STATE_MASK) != REMOTE_HANDLE_LOCAL) {
        return;
      }

      uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
      if (addr == 0 || !_rmm->_g1h->is_in_reserved((void*)addr)) {
        return;
      }

      HeapRegion* hr = _rmm->_g1h->heap_region_containing_or_null((void*)addr);
      if (hr == nullptr) {
        return;
      }

      uint ridx = hr->hrm_index();
      if (ridx >= _num_regions || !_eviction_candidates[ridx] ||
          !_region_complete[ridx] || _region_count[ridx] <= 0) {
        return;
      }

      int blockers = ++_blockers_by_region[ridx];
      _total_blockers++;
      if (blockers <= _log_limit) {
        log_warning(gc)("E3 local-handle guard: region=%u blocker handle="
                        PTR_FORMAT " local=" PTR_FORMAT
                        " listed=%d dormant=%d rc=%u",
                        ridx, p2i(h), addr,
                        h->_local_listed ? 1 : 0,
                        h->is_dormant() ? 1 : 0, h->remote_refcount());
      }
    }

    int total_blockers() const { return _total_blockers; }
  };

  CountLocalHandlesInRegionsClosure cl(this, eviction_candidates, region_complete,
                                       region_count, num_regions, blockers_by_region,
                                       log_limit);
  _handle_allocator.handles_do(&cl);
  return cl.total_blockers();
}

Klass* G1RemoteMemoryManager::fetch_remote_object(RemoteHandle* h, void* dest) {
  assert(h != nullptr, "Handle must not be null");

  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
  assert(state == REMOTE_HANDLE_REMOTE || state == REMOTE_HANDLE_FETCHING,
         "Handle must be REMOTE or FETCHING");

  if (!h->_remote_location.is_object_slot()) {
    log_warning(gc)("Remote fetch rejected non-object-slot handle=" PTR_FORMAT
                    " location_kind=%u",
                    p2i(h), h->_remote_location.kind_acquire());
    return nullptr;
  }

  size_t slot_id = h->remote_object_slot_id(sa);

  // Fetch object bytes via backend (SIM/TCP/RDMA)
  size_t word_size = 0;
  Klass* klass = _backend->fetch(slot_id, dest, &word_size);

  if (klass != nullptr) {
    size_t expected_ws = h->eviction_word_size();
    if (word_size != expected_ws) {
      log_warning(gc)("Remote fetch size MISMATCH: slot=" SIZE_FORMAT " expected=" SIZE_FORMAT "w got=" SIZE_FORMAT "w — aborting fetch to prevent type confusion",
                       slot_id, expected_ws, word_size);
      return nullptr;
    }
    log_trace(gc)("Remote fetch: slot=" SIZE_FORMAT " -> dest=" PTR_FORMAT " klass=%s size=" SIZE_FORMAT "w",
                  slot_id, p2i(dest), klass->external_name(), word_size);
  } else {
    log_warning(gc)("Remote fetch FAILED: slot=" SIZE_FORMAT, slot_id);
  }

  return klass;
}

// ============================================================
// Post-Fetch Field Patching
// ============================================================
// After fetching remote bytes into FCR, patch oop fields using the
// sidecar edge table. The fetched bytes contain oop values from eviction
// time — targets may have moved or died since then. The edge table maps
// each oop field offset to the target's Handle, which tracks the
// current address.
//
// Must be called BEFORE set_local_release() — the fetched object must
// not be visible to other threads until all fields are patched.

void G1RemoteMemoryManager::patch_fetched_fields(RemoteHandle* source_handle, HeapWord* dest) {
  ObjectEdgeTable* et = take_edge_table(source_handle);
  if (et == nullptr) {
    // No edge table — object had no oop fields at eviction time.
    // Or edge table was already cleaned up. Nothing to patch.
    return;
  }

  uintptr_t base = (uintptr_t)dest;
  int patched = 0;
  int stale_targets = 0;
  int stale_remapped = 0;
  bool cm_active = concurrent_marking_active();

  size_t obj_byte_size = et->_eviction_word_size * HeapWordSize;

  for (uint32_t i = 0; i < et->_entry_count; i++) {
    EdgeEntry& edge = et->_entries[i];
    guarantee(edge._field_offset >= 16,
              "Edge table offset %u would corrupt object header", edge._field_offset);
    guarantee(edge._field_offset + sizeof(uintptr_t) <= obj_byte_size,
              "Edge table offset %u + %zu overflows object of %zu bytes",
              edge._field_offset, sizeof(uintptr_t), obj_byte_size);
    uintptr_t* field_addr = (uintptr_t*)(base + edge._field_offset);
    RemoteHandle* target = edge._target_handle;
    if (target == nullptr) {
      *field_addr = 0;
      patched++;
      continue;
    }

    uintptr_t sa = target->load_state_and_addr_acquire();
    uintptr_t target_state = sa & REMOTE_HANDLE_STATE_MASK;

    if (target_state == REMOTE_HANDLE_DEAD) {
      *field_addr = 0;
      patched++;
      if (cm_active) { defer_refcount_decrement(target); } else { target->decrement_remote_refcount(); }
    } else if (target_state == REMOTE_HANDLE_LOCAL) {
      uintptr_t target_addr = sa & REMOTE_HANDLE_ADDR_MASK;
      if (validate_local_handle_addr(target, "FETCH-PATCH",
                                     &stale_targets, 16)) {
        *field_addr = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)target;
        patched++;
      } else {
        RemoteHandle* alias = handle_for_stale_eviction_addr(target_addr);
        if (alias != nullptr && alias != target) {
          uintptr_t alias_state =
              alias->load_state_and_addr_acquire() & REMOTE_HANDLE_STATE_MASK;
          if (alias_state != REMOTE_HANDLE_DEAD) {
            alias->increment_remote_refcount();
            if (cm_active) {
              defer_refcount_decrement(target);
            } else {
              target->decrement_remote_refcount();
            }
            *field_addr = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)alias;
            patched++;
            stale_remapped++;
            continue;
          }
        }
        *field_addr = 0;
        patched++;
        if (cm_active) { defer_refcount_decrement(target); } else { target->decrement_remote_refcount(); }
      }
    } else {
      // LOCAL, REMOTE, or FETCHING — write shared_oop(handle).
      // The load barrier resolves through the handle on every access,
      // so the field stays correct even if the target moves during GC.
      // Writing clean oops here would require card dirtying + RSet updates
      // to keep the reference current — shared_oop avoids that fragility.
      *field_addr = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)target;
      patched++;
    }
  }

  if (stale_targets > 16) {
    log_warning(gc)("FETCH-PATCH: marked %d stale LOCAL target handles DEAD "
                    "(logged first 16)", stale_targets);
  }
  if (stale_remapped > 0) {
    log_info(gc)("FETCH-PATCH: remapped %d stale LOCAL target handles via "
                 "eviction aliases", stale_remapped);
  }

  // Enqueue dirty cards covering the fetched object into the G1 dirty card
  // queue. dirty_MemRegion alone only sets card bytes — G1 concurrent
  // refinement and STW merging only process cards from the dirty card queue.
  // Without enqueuing, cross-region refs from this FCR object are never
  // added to remembered sets, so GC won't find or update them.
  if (patched > 0) {
    G1CardTable* ct = _g1h->card_table();
    G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
    Thread* thr = Thread::current();
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thr);
    CardTable::CardValue* first = ct->byte_for(dest);
    CardTable::CardValue* last = ct->byte_for(dest + et->_eviction_word_size - 1);
    for (CardTable::CardValue* card = first; card <= last; card++) {
      if (*card != G1CardTable::g1_young_card_val()) {
        *card = G1CardTable::dirty_card_val();
        qset.enqueue(queue, card);
      }
    }
  }

  log_debug(gc)("Fetch patch: handle=" PTR_FORMAT " dest=" PTR_FORMAT " patched=%d/%u fields",
                p2i(source_handle), p2i(dest), patched, et->_entry_count);

  // No longer needed after fetch.  The table was unlinked before patching so
  // concurrent fetchers of other handles can safely traverse the bucket chain.
  ObjectEdgeTable::free(et);
}

// ============================================================
// Full GC Handle Update
// ============================================================
// After Full GC phase 3 (adjust pointers), forwarding addresses are
// installed in mark words. Walk the Handle table and update each
// LOCAL Handle to point to the forwarded address. Must be called
// before phase 4 (compaction) moves the bytes.

void G1RemoteMemoryManager::update_handles_for_full_gc() {
  int updated = 0;
  int stale_skipped = 0;

  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    HandleEntry* e = _table[idx];
    while (e != nullptr) {
      HandleEntry* next = e->_next;
      RemoteHandle* h = e->_handle;

      if (h != nullptr) {
        uintptr_t sa = h->load_state_and_addr_acquire();
        if ((sa & REMOTE_HANDLE_STATE_MASK) != REMOTE_HANDLE_LOCAL) {
          e = next;
          continue;
        }

        // Full GC runs after remote eviction may leave LOCAL table entries
        // pointing into evict-guarded or otherwise stale heap ranges.  Do not
        // read the mark word until the address is known safe.  This pass must
        // not mark handles DEAD: tagged heap fields may still recover through
        // duplicate/remote aliases during normal barrier resolution.
        uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
        HeapRegion* hr = nullptr;
        bool safe_to_read = false;
        if (addr != 0 && _g1h->is_in_reserved((void*)addr)) {
          hr = _g1h->heap_region_containing_or_null((void*)addr);
          if (hr != nullptr &&
              !hr->is_evict_guarded() &&
              !hr->is_free() &&
              !hr->is_empty() &&
              !hr->is_continues_humongous() &&
              (HeapWord*)addr >= hr->bottom() &&
              (HeapWord*)addr < hr->top() &&
              _g1h->is_in((void*)addr)) {
            oop candidate = cast_to_oop(addr);
            Klass* k = candidate->klass_or_null_acquire();
            if (remote_eviction_valid_klass(k) &&
                !G1CollectedHeap::is_obj_filler(candidate)) {
              size_t word_size = candidate->size_given_klass(k);
              safe_to_read = word_size >= (size_t)MinObjAlignment &&
                             is_object_aligned(word_size) &&
                             word_size <= (size_t)(hr->top() - (HeapWord*)addr) &&
                             word_size <= (size_t)(hr->end() - (HeapWord*)addr);
            }
          }
        }
        if (!safe_to_read) {
          stale_skipped++;
          e = next;
          continue;
        }

        oop obj = cast_to_oop(addr);
        if (obj->is_forwarded()) {
          oop new_obj = obj->forwardee();
          uintptr_t new_addr = cast_from_oop<uintptr_t>(new_obj);

          // Update Handle to new address
          h->set_local(cast_from_oop<void*>(new_obj));

          // Rekey table entry: unlink from old bucket, insert in new
          // (we can't modify while iterating, so update in-place)
          e->_obj_addr = new_addr;
          updated++;
        }
      }
      e = next;
    }
  }

  // Rekey: some entries may now be in the wrong hash bucket.
  // Rebuild the table from the entries (simple for prototype).
  if (updated > 0) {
    // Collect all entries
    HandleEntry* all_entries = nullptr;
    for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
      HandleEntry* e = _table[idx];
      while (e != nullptr) {
        HandleEntry* next = e->_next;
        e->_next = all_entries;
        all_entries = e;
        e = next;
      }
      _table[idx] = nullptr;
    }
    // Re-insert all entries with new keys
    HandleEntry* e = all_entries;
    while (e != nullptr) {
      HandleEntry* next = e->_next;
      size_t new_idx = hash_obj(e->_obj_addr);
      e->_next = _table[new_idx];
      _table[new_idx] = e;
      e = next;
    }
  }
  table_unlock();

  if (updated > 0 || stale_skipped > 0) {
    log_info(gc)("Full GC handle update: %d handles rekeyed, %d stale handles skipped",
                 updated, stale_skipped);
  }
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

static inline size_t remote_root_hash(uintptr_t id, size_t mask) {
  uint64_t h = (uint64_t)id;
  h ^= h >> 33;
  h *= UINT64_C(0xff51afd7ed558ccd);
  h ^= h >> 33;
  h *= UINT64_C(0xc4ceb9fe1a85ec53);
  h ^= h >> 33;
  return (size_t)h & mask;
}

static inline bool insert_remote_root_id(uintptr_t* dedup_set, size_t set_mask,
                                         uintptr_t id) {
  size_t slot = remote_root_hash(id, set_mask);
  while (dedup_set[slot] != 0 && dedup_set[slot] != id) {
    slot = (slot + 1) & set_mask;
  }
  if (dedup_set[slot] == id) {
    return false;
  }
  dedup_set[slot] = id;
  return true;
}

size_t G1RemoteMemoryManager::collect_dead_remote_objects() {
  if (G1RemoteCollectionInterval == 0) {
    // The current trace-and-report path is conservative diagnostic machinery:
    // the JVM does not safely clear all dead shared-oops yet, so remote slots
    // are reported but not reclaimed. Keep it opt-in to avoid per-GC full
    // handle scans and remote graph traces on performance runs.
    log_debug(gc)("collect_dead: SKIP disabled by G1RemoteCollectionInterval=0");
    return 0;
  }

  size_t handles_allocated = _handle_allocator.total_handles_allocated();
  if (_remote_collection_has_trace && G1RemoteCollectionInterval > 1) {
    size_t handle_delta =
        (handles_allocated >= _remote_collection_last_handles_allocated) ?
        (handles_allocated - _remote_collection_last_handles_allocated) : 0;
    bool interval_due =
        (_remote_collection_skipped + 1) >= G1RemoteCollectionInterval;
    bool growth_due =
        G1RemoteCollectionHandleDelta > 0 &&
        handle_delta >= G1RemoteCollectionHandleDelta;

    if (!interval_due && !growth_due) {
      _remote_collection_skipped++;
      log_info(gc)("collect_dead: SKIP throttled (skipped=%u/%u, "
                   "handles=%zu last=%zu delta=%zu threshold=%zu, "
                   "tagged_entries=%d, retained_cross_roots=%d)",
                   _remote_collection_skipped, G1RemoteCollectionInterval,
                   handles_allocated, _remote_collection_last_handles_allocated,
                   handle_delta, G1RemoteCollectionHandleDelta,
                   _tagged_field_count, _cross_roots_count);
      return 0;
    }
  }

  Ticks root_build_start = Ticks::now();

  // Build deduplicated root set from three sources:
  //   1. CM roots (from concurrent marking — already in _remote_roots)
  //   2. Phase C tagged field handles (shared_oops in heap)
  //   3. REMOTE handles with remote_refcount > 0 (edge-table references)
  //
  // Use a power-of-2 hash set for O(1) dedup. Size from allocated handle
  // count instead of pre-scanning the handle table; the table scan below also
  // computes total_remote.
  size_t total_remote = 0;

  // Hash set for dedup: open addressing with linear probing
  size_t expected_entries = (size_t)_remote_roots_count +
                            (size_t)_tagged_field_count +
                            _handle_allocator.total_handles_allocated();
  size_t set_capacity = 1;
  while (set_capacity < expected_entries * 2 + 64) {
    set_capacity <<= 1;
  }
  uintptr_t* dedup_set = NEW_C_HEAP_ARRAY(uintptr_t, set_capacity, mtGC);
  memset(dedup_set, 0, set_capacity * sizeof(uintptr_t));
  size_t set_mask = set_capacity - 1;

  // Collect unique roots into _remote_roots (dynamically grown)
  int cm_count = MIN2(_cm_remote_roots_count, _remote_roots_count);
  _remote_roots_count = cm_count;
  size_t max_root_capacity = MIN2(expected_entries, (size_t)max_jint);
  ensure_remote_roots_capacity((int)max_root_capacity);

  // Insert existing CM roots into dedup set
  for (int i = 0; i < cm_count; i++) {
    insert_remote_root_id(dedup_set, set_mask, _remote_roots[i]);
  }

  // Source 2: Phase C tagged field handles
  Ticks tagged_start = Ticks::now();
  int phase_c_added = 0;
  for (int i = 0; i < _tagged_field_count; i++) {
    RemoteHandle* h = _tagged_fields[i]._handle;
    if (h != nullptr && h->is_remote()) {
      uintptr_t id = (uintptr_t)h;
      if (insert_remote_root_id(dedup_set, set_mask, id)) {
        add_remote_root(id);
        phase_c_added++;
      }
    }
  }
  double tagged_ms = (Ticks::now() - tagged_start).seconds() * 1000.0;

  // Source 3: REMOTE handles with remote_refcount > 0
  Ticks refcount_start = Ticks::now();
  int refcount_added = 0;
  table_lock();
  for (size_t idx = 0; idx < TABLE_SIZE; idx++) {
    for (HandleEntry* e = _table[idx]; e != nullptr; e = e->_next) {
      RemoteHandle* h = e->_handle;
      if (h != nullptr && h->is_remote()) {
        total_remote++;
        if (h->remote_refcount() > 0) {
          uintptr_t id = (uintptr_t)h;
          if (insert_remote_root_id(dedup_set, set_mask, id)) {
            add_remote_root(id);
            refcount_added++;
          }
        }
      }
    }
  }
  table_unlock();
  double refcount_ms = (Ticks::now() - refcount_start).seconds() * 1000.0;
  double root_build_ms = (Ticks::now() - root_build_start).seconds() * 1000.0;
  FREE_C_HEAP_ARRAY(uintptr_t, dedup_set);

  log_info(gc)("collect_dead: root-build %.1fms (tagged %.1fms, refcount %.1fms, "
               "dedup_cap=%zu, handles_allocated=%zu, tagged_entries=%d)",
               root_build_ms, tagged_ms, refcount_ms, set_capacity,
               handles_allocated, _tagged_field_count);

  log_info(gc)("collect_dead: roots: %d CM + %d tagged-fields + %d refcount = %d unique "
               "(%zu remote handles)",
               cm_count, phase_c_added, refcount_added, _remote_roots_count, total_remote);

  if (_remote_roots_count == 0) {
    log_info(gc)("collect_dead: SKIP (no roots, %zu remote handles retained)", total_remote);
    return 0;
  }

  log_info(gc)("collect_dead: report_remote_roots_v2 (%d roots, %zu remote handles)",
               _remote_roots_count, total_remote);
  _backend->report_remote_roots_v2(_remote_roots, _remote_roots_count);
  log_info(gc)("collect_dead: report_remote_roots_v2 DONE");

  // trace_and_report — get dead handles + cross-boundary edges
  uintptr_t* dead_ids = nullptr;
  size_t num_dead = 0;
  size_t bytes_freed = 0;
  uintptr_t* cross_src = nullptr;
  uintptr_t* cross_tgt = nullptr;
  size_t num_cross = 0;

  log_info(gc)("collect_dead: trace_and_report START");
  _backend->trace_and_report(&dead_ids, &num_dead, &bytes_freed,
                             &cross_src, &cross_tgt, &num_cross);
  log_info(gc)("collect_dead: trace_and_report DONE (dead=%zu freed=%zu cross=%zu)",
               num_dead, bytes_freed, num_cross);
  _remote_collection_has_trace = true;
  _remote_collection_skipped = 0;
  _remote_collection_last_handles_allocated = handles_allocated;

  // Step 2c: Populate cross-boundary roots.
  // Cross-edges: live REMOTE handle → LOCAL handle.
  // The LOCAL targets must be rooted during GC to prevent collection.
  _cross_roots_count = 0;
  if (num_cross > 0) {
    table_lock();
    for (size_t i = 0; i < num_cross && _cross_roots_count < MAX_CROSS_ROOTS; i++) {
      uintptr_t local_handle_id = cross_tgt[i];
      // Find the RemoteHandle by handle_id (address of Handle)
      RemoteHandle* h = (RemoteHandle*)local_handle_id;
      if (h != nullptr && h->is_local()) {
        _cross_roots[_cross_roots_count++] = h;
      }
    }
    table_unlock();
    log_info(gc)("Cross-boundary roots: %d LOCAL handles kept alive by live REMOTE objects",
                 _cross_roots_count);
  }

  if (cross_src) os::free(cross_src);
  if (cross_tgt) os::free(cross_tgt);

  // Step 3: Log dead handles but do NOT free them yet.
  // Freeing handles while shared_oops in the heap still reference them causes
  // SIGSEGV: mutators/GC closures dereference stale tagged oops to freed memory.
  // _tagged_fields doesn't capture all references (stack oops, moved objects).
  // Safe collection requires a full-heap scan to clear all shared_oops first.
  // TODO: implement full-heap dead-handle sweep before freeing handles.
  if (dead_ids) os::free(dead_ids);

  // Keep only persistent CM roots. Phase C/refcount roots are recomputed for
  // each collection; retaining them makes the root set grow every young GC.
  _remote_roots_count = cm_count;

  size_t retained = total_remote;
  if (num_dead > 0 || retained > 0) {
    log_info(gc)("Remote collection: %zu dead identified (NOT freed — unsafe), "
                 "%zu live, %zu total remote, %d cross-boundary roots",
                 num_dead, retained - num_dead, retained, _cross_roots_count);
  }
  return 0;
}

int G1RemoteMemoryManager::fixup_tagged_field_handles() {
  int updated = 0;
  int removed = 0;
  int direct_updated = 0;
  int direct_retained = 0;
  int direct_converted = 0;
  int direct_nulled = 0;
  int write_idx = 0;
  G1RemoteRollbackDirtyCards dirty_cards(_g1h);
  RemoteHandleAllocBuffer hab;

  for (int i = 0; i < _tagged_field_count; i++) {
    TaggedFieldEntry entry = _tagged_fields[i];
    oop* field_addr = _tagged_fields[i]._field_addr;
    RemoteHandle* h = _tagged_fields[i]._handle;

    if (_tagged_fields[i].is_direct()) {
      if (field_addr == nullptr || !_g1h->is_in_reserved((void*)field_addr)) {
        removed++;
        continue;
      }

      HeapRegion* field_hr =
          _g1h->heap_region_containing_or_null((HeapWord*)field_addr);
      if (field_hr != nullptr &&
          (field_hr->is_free() || field_hr->is_evict_guarded())) {
        removed++;
        continue;
      }

      uintptr_t raw = *(uintptr_t*)field_addr;
      bool direct = (raw & G1_OOP_MANAGED_BIT) != 0 &&
                    (raw & G1_OOP_INDIRECT_BIT) == 0;
      if (!direct) {
        removed++;
        continue;
      }
      if (entry._tagged_raw != 0 && raw != entry._tagged_raw) {
        entry._tagged_raw = raw;
      }

      uintptr_t addr = raw & G1_OOP_ADDR_MASK;
      if (!g1_remote_oop_is_aligned(addr) ||
          !_g1h->is_in_reserved((void*)addr)) {
        *(uintptr_t*)field_addr = 0;
        dirty_cards.dirty_field(field_addr);
        direct_nulled++;
        removed++;
        continue;
      }

      RemoteHandle* alias = nullptr;
      DenseDirectTargetState target_state =
          classify_dense_direct_target(this, _g1h, addr, &alias, nullptr);
      if (target_state == DenseDirectTargetRemote) {
        *(uintptr_t*)field_addr = 0;
        dirty_cards.dirty_field(field_addr);
        direct_nulled++;
        removed++;
        continue;
      }

      if (target_state == DenseDirectTargetAlias && alias != nullptr) {
        *(uintptr_t*)field_addr =
            G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)alias;
        dirty_cards.dirty_field(field_addr);
        entry._handle = alias;
        entry._tagged_raw = 0;
        entry._kind = TaggedFieldHandle;
        _tagged_fields[write_idx++] = entry;
        direct_converted++;
        continue;
      }
      if (target_state != DenseDirectTargetLocal) {
        *(uintptr_t*)field_addr = 0;
        dirty_cards.dirty_field(field_addr);
        direct_nulled++;
        removed++;
        continue;
      }

      oop target_oop = cast_to_oop((HeapWord*)addr);
      markWord m = target_oop->mark();
      if (m.is_marked()) {
        target_oop = cast_to_oop(m.decode_pointer());
        direct_updated++;
      }

      RemoteHandle* direct_target = handle_for(target_oop);
      if (direct_target == nullptr) {
        direct_target = ensure_handle_for(target_oop, &hab);
      }
      if (direct_target == nullptr) {
        *(uintptr_t*)field_addr = 0;
        dirty_cards.dirty_field(field_addr);
        direct_nulled++;
        removed++;
        continue;
      }
      *(uintptr_t*)field_addr =
          G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)direct_target;
      dirty_cards.dirty_field(field_addr);
      entry._handle = direct_target;
      entry._tagged_raw = 0;
      entry._kind = TaggedFieldHandle;

      _tagged_fields[write_idx++] = entry;
      direct_converted++;
      continue;
    }

    if (field_addr == nullptr || h == nullptr) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
      }
      removed++;
      continue;
    }

    if (!_g1h->is_in_reserved((void*)field_addr)) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
      }
      removed++;
      continue;
    }

    // field_addr may be in an evicted (mprotected) region — skip without reading
    HeapRegion* field_hr = _g1h->heap_region_containing((HeapWord*)field_addr);
    if (field_hr != nullptr && (field_hr->is_free() || field_hr->is_evict_guarded())) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
      }
      removed++;
      continue;
    }

    uintptr_t raw = *(uintptr_t*)field_addr;

    // Stale entry: field no longer tagged or points to a different Handle
    if ((raw & G1_OOP_INDIRECT_BIT) == 0 ||
        (RemoteHandle*)(raw & G1_OOP_ADDR_MASK) != h) {
      if (entry.is_dense_handle()) {
        release_dense_tagged_handle_ref(this, entry);
      }
      removed++;
      continue;
    }

    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    // Handle must be LOCAL for fixup (REMOTE/FETCHING/DEAD don't need it)
    if (state != REMOTE_HANDLE_LOCAL) {
      _tagged_fields[write_idx++] = _tagged_fields[i];
      continue;
    }

    HeapWord* target = (HeapWord*)(sa & REMOTE_HANDLE_ADDR_MASK);
    oop target_oop = cast_to_oop(target);

    // Check if target has been forwarded (mark word contains forwarding ptr)
    if (_g1h->is_in(target_oop)) {
      HeapRegion* target_hr = _g1h->heap_region_containing(target);
      if (target_hr != nullptr && (target_hr->is_free() || target_hr->is_evict_guarded())) {
        if (entry.is_dense_handle()) {
          release_dense_tagged_handle_ref(this, entry);
        }
        removed++;
        continue;
      }
      markWord m = target_oop->mark();
      if (m.is_marked()) {
        oop forwardee = cast_to_oop(m.decode_pointer());
        update_handle_for_evacuation(h, target_oop, forwardee);
        updated++;
      }
    }

    _tagged_fields[write_idx++] = _tagged_fields[i];
  }

  _tagged_field_count = write_idx;
  dirty_cards.flush();

  if (updated > 0 || removed > 0 || direct_updated > 0 ||
      direct_converted > 0 || direct_nulled > 0 ||
      dirty_cards.dirtied() > 0) {
    log_info(gc)("Tagged field fixup: %d handles updated, "
                 "%d direct refs updated, %d direct refs retained, "
                 "%d direct refs converted, %d direct refs nulled, "
                 "%d stale entries removed, %d entries remaining, "
                 "dirtied %d cards",
                 updated, direct_updated, direct_retained,
                 direct_converted, direct_nulled, removed,
                 _tagged_field_count, dirty_cards.dirtied());
  }
  return updated;
}

int G1RemoteMemoryManager::fixup_all_local_handles() {
  Ticks start = Ticks::now();

  class FixupLocalHandleClosure {
    G1RemoteMemoryManager* _rmm;
    int _updated;
    size_t _scanned;
    size_t _local_seen;
    size_t _stale_region;
    int _stale_killed;

  public:
    FixupLocalHandleClosure(G1RemoteMemoryManager* rmm)
      : _rmm(rmm), _updated(0), _scanned(0), _local_seen(0),
        _stale_region(0), _stale_killed(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_DEAD && h->remote_refcount() == 0) return;

      _scanned++;
      if (state != REMOTE_HANDLE_LOCAL) return;

      _local_seen++;
      uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
      if (!_rmm->validate_local_handle_addr(h, "HANDLE-FIXUP",
                                            &_stale_killed, 16)) {
        _stale_region++;
        return;
      }

      oop target = cast_to_oop(addr);
      markWord m = target->mark();
      if (m.is_marked()) {
        oop forwardee = cast_to_oop(m.decode_pointer());
        _rmm->update_handle_for_evacuation(h, target, forwardee);
        _updated++;
      }
    }

    int updated() const { return _updated; }
    size_t scanned() const { return _scanned; }
    size_t local_seen() const { return _local_seen; }
    size_t stale_region() const { return _stale_region; }
    int stale_killed() const { return _stale_killed; }
  };

  log_info(gc)("Handle table fixup START: local_handles=%zu allocated_handles=%zu "
               "tagged_entries=%d",
               _local_handle_count, _handle_allocator.total_handles_allocated(),
               _tagged_field_count);

  FixupLocalHandleClosure cl(this);
  size_t local_count_start = _local_handle_count;
  RemoteHandle* cur = _local_handles_head;
  while (cur != nullptr) {
    RemoteHandle* next = cur->_local_next;
    cl.do_handle(cur);
    cur = next;
  }

  double elapsed_ms = (Ticks::now() - start).seconds() * 1000.0;
  log_info(gc)("Handle table fixup DONE: %.1fms scanned=%zu local=%zu updated=%d "
               "stale_region=%zu stale_killed=%d local_handles=%zu allocated_handles=%zu",
               elapsed_ms, cl.scanned(), cl.local_seen(), cl.updated(),
               cl.stale_region(), cl.stale_killed(), _local_handle_count,
               _handle_allocator.total_handles_allocated());
  if (cl.stale_killed() > 16) {
    log_warning(gc)("Handle table fixup marked %d stale LOCAL handles DEAD "
                    "(logged first 16)", cl.stale_killed());
  }
  if (elapsed_ms > 1000.0) {
    log_warning(gc)("Handle table fixup took %.1fms for %zu entries "
                    "(local_handles=%zu)",
                    elapsed_ms, cl.scanned(), _local_handle_count);
  } else if (cl.updated() > 0) {
    log_info(gc)("Handle table fixup: %d LOCAL handles updated for forwarded objects",
                 cl.updated());
  }
  if (cl.scanned() != local_count_start && cl.stale_killed() == 0) {
    log_warning(gc)("Handle table fixup local-list count mismatch: scanned=%zu "
                    "start_local_handles=%zu current_local_handles=%zu",
                    cl.scanned(), local_count_start, _local_handle_count);
  }
  return cl.updated();
}

int G1RemoteMemoryManager::fixup_local_handles_in_regions(const bool* region_set,
                                                          uint num_regions) {
  if (region_set == nullptr || num_regions == 0 ||
      _local_handle_region_heads == nullptr ||
      _local_handle_region_counts == nullptr) {
    return fixup_all_local_handles();
  }

  Ticks start = Ticks::now();

  class FixupScopedLocalHandleClosure {
    G1RemoteMemoryManager* _rmm;
    int _updated;
    size_t _scanned;
    size_t _local_seen;
    size_t _stale_region;
    int _stale_killed;

  public:
    FixupScopedLocalHandleClosure(G1RemoteMemoryManager* rmm)
      : _rmm(rmm), _updated(0), _scanned(0), _local_seen(0),
        _stale_region(0), _stale_killed(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_DEAD && h->remote_refcount() == 0) return;

      _scanned++;
      if (state != REMOTE_HANDLE_LOCAL) return;

      _local_seen++;
      uintptr_t addr = sa & REMOTE_HANDLE_ADDR_MASK;
      if (!_rmm->validate_local_handle_addr(h, "HANDLE-FIXUP-CSET",
                                            &_stale_killed, 16)) {
        _stale_region++;
        return;
      }

      oop target = cast_to_oop(addr);
      markWord m = target->mark();
      if (m.is_marked()) {
        oop forwardee = cast_to_oop(m.decode_pointer());
        _rmm->update_handle_for_evacuation(h, target, forwardee);
        _updated++;
      }
    }

    int updated() const { return _updated; }
    size_t scanned() const { return _scanned; }
    size_t local_seen() const { return _local_seen; }
    size_t stale_region() const { return _stale_region; }
    int stale_killed() const { return _stale_killed; }
  };

  uint scoped_regions = 0;
  size_t expected_handles = 0;
  for (uint idx = 0; idx < num_regions && idx < _local_handle_region_capacity; idx++) {
    if (!region_set[idx]) {
      continue;
    }
    scoped_regions++;
    expected_handles += Atomic::load(&_local_handle_region_counts[idx]);
  }

  log_info(gc)("Handle table fixup START: scoped_regions=%u scoped_handles=%zu "
               "local_handles=%zu allocated_handles=%zu tagged_entries=%d",
               scoped_regions, expected_handles, _local_handle_count,
               _handle_allocator.total_handles_allocated(), _tagged_field_count);

  FixupScopedLocalHandleClosure cl(this);
  for (uint idx = 0; idx < num_regions && idx < _local_handle_region_capacity; idx++) {
    if (!region_set[idx]) {
      continue;
    }
    RemoteHandle* cur = _local_handle_region_heads[idx];
    while (cur != nullptr) {
      RemoteHandle* next = cur->_region_next;
      cl.do_handle(cur);
      cur = next;
    }
  }

  double elapsed_ms = (Ticks::now() - start).seconds() * 1000.0;
  log_info(gc)("Handle table fixup DONE: %.1fms scoped_regions=%u expected=%zu "
               "scanned=%zu local=%zu updated=%d stale_region=%zu stale_killed=%d "
               "local_handles=%zu allocated_handles=%zu",
               elapsed_ms, scoped_regions, expected_handles, cl.scanned(),
               cl.local_seen(), cl.updated(), cl.stale_region(),
               cl.stale_killed(), _local_handle_count,
               _handle_allocator.total_handles_allocated());
  if (cl.stale_killed() > 16) {
    log_warning(gc)("Scoped handle table fixup marked %d stale LOCAL handles DEAD "
                    "(logged first 16)", cl.stale_killed());
  }
  if (elapsed_ms > 1000.0) {
    log_warning(gc)("Scoped handle table fixup took %.1fms for %zu entries "
                    "(local_handles=%zu)",
                    elapsed_ms, cl.scanned(), _local_handle_count);
  } else if (cl.updated() > 0) {
    log_info(gc)("Scoped handle table fixup: %d LOCAL handles updated for forwarded objects",
                 cl.updated());
  }
  return cl.updated();
}

int G1RemoteMemoryManager::purge_stale_local_handles(const char* phase, int log_limit) {
  Ticks start = Ticks::now();

  class PurgeStaleLocalHandleClosure {
    G1RemoteMemoryManager* _rmm;
    const char* _phase;
    int _log_limit;
    size_t _scanned;
    size_t _local_seen;
    int _stale_killed;

  public:
    PurgeStaleLocalHandleClosure(G1RemoteMemoryManager* rmm,
                                 const char* phase,
                                 int log_limit)
      : _rmm(rmm), _phase(phase), _log_limit(log_limit),
        _scanned(0), _local_seen(0), _stale_killed(0) {}

    void do_handle(RemoteHandle* h) {
      if (h == nullptr) return;

      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_DEAD && h->remote_refcount() == 0) return;

      _scanned++;
      if (state != REMOTE_HANDLE_LOCAL) return;

      _local_seen++;
      _rmm->validate_local_handle_addr(h,
                                       _phase == nullptr ? "STALE-HANDLE-SWEEP" : _phase,
                                       &_stale_killed,
                                       _log_limit);
    }

    size_t scanned() const { return _scanned; }
    size_t local_seen() const { return _local_seen; }
    int stale_killed() const { return _stale_killed; }
  };

  PurgeStaleLocalHandleClosure cl(this, phase, log_limit);
  _handle_allocator.handles_do(&cl);

  if (cl.stale_killed() > 0) {
    double elapsed_ms = (Ticks::now() - start).seconds() * 1000.0;
    log_warning(gc)("%s: marked %d stale LOCAL handles DEAD in %.1fms "
                    "(scanned=%zu local=%zu allocated_handles=%zu)",
                    phase == nullptr ? "STALE-HANDLE-SWEEP" : phase,
                    cl.stale_killed(), elapsed_ms, cl.scanned(), cl.local_seen(),
                    _handle_allocator.total_handles_allocated());
  }
  return cl.stale_killed();
}

static bool is_valid_region_object(G1CollectedHeap* g1h, oop obj, HeapRegion** region_out = nullptr) {
  if (obj == nullptr || !g1h->is_in(obj)) return false;
  HeapRegion* hr = g1h->heap_region_containing(obj);
  if (hr == nullptr || hr->is_free() || hr->is_empty() ||
      hr->is_evict_guarded() || hr->is_continues_humongous()) {
    return false;
  }

  HeapWord* obj_addr = cast_from_oop<HeapWord*>(obj);
  if (obj_addr < hr->bottom() || obj_addr >= hr->top()) return false;

  // Avoid G1 block-start validation here. Stale-ref fixup may inspect refs
  // around recently fetched/evicted dense regions; BOT walking can parse
  // interior payload as an object header before rejecting the address.
  Klass* k = obj->klass_or_null_acquire();
  if (!remote_eviction_valid_klass(k)) return false;

  Klass* size_k = obj->klass_or_null_acquire();
  if (size_k != k) return false;

  size_t sz = obj->size_given_klass(size_k);
  if (sz < (size_t)MinObjAlignment) return false;
  if (!is_object_aligned(sz)) return false;
  if (sz > (size_t)(hr->top() - obj_addr)) return false;
  if (sz > (size_t)(hr->end() - obj_addr)) return false;

  if (region_out != nullptr) {
    *region_out = hr;
  }
  return true;
}

// Closure that fixes refs to cset regions by writing forwardees. If remset/card
// coverage missed a direct tagged old-field reference, the target may not have
// been forwarded; rescue it into an FCR region before the cset is freed.
class CSetRefFixupClosure : public BasicOopIterateClosure {
  G1CollectedHeap* _g1h;
  G1RemoteMemoryManager* _rmm;
  bool _allow_rescue;
  int _fixed;
  int _rescued;
  int _skipped;
  int _invalid;
  int _invalid_nulled;
  int _rescue_failed;
  int _direct_converted;
  bool _src_is_fcr;     // Set per object via set_src_is_fcr()
  RemoteHandleAllocBuffer _hab;

  bool should_log_rescue() const {
    return _rescued < 16 || ((_rescued & (_rescued - 1)) == 0);
  }

  void dirty_cards_for_range(HeapWord* start, size_t word_size) {
    if (start == nullptr || word_size == 0) return;
    if (!_g1h->is_in_reserved(start)) return;

    G1CardTable* ct = _g1h->card_table();
    G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
    G1DirtyCardQueue tmp_queue(&dcqs);

    CardTable::CardValue* first = ct->byte_for(start);
    CardTable::CardValue* last = ct->byte_for(start + word_size - 1);
    for (CardTable::CardValue* card = first; card <= last; card++) {
      if (*card != G1CardTable::g1_young_card_val()) {
        *card = G1CardTable::dirty_card_val();
        dcqs.enqueue(tmp_queue, card);
      }
    }
    dcqs.flush_queue(tmp_queue);
  }

  void dirty_card_for_field(oop* p) {
    if (p == nullptr) return;
    dirty_cards_for_range((HeapWord*)p, 1);
  }

public:
  CSetRefFixupClosure(G1CollectedHeap* g1h, G1RemoteMemoryManager* rmm, bool allow_rescue)
    : _g1h(g1h), _rmm(rmm), _allow_rescue(allow_rescue),
      _fixed(0), _rescued(0), _skipped(0), _invalid(0), _invalid_nulled(0),
      _rescue_failed(0), _direct_converted(0), _src_is_fcr(false), _hab() {}

  void set_src_is_fcr(bool v) { _src_is_fcr = v; }

  void store_forwardee(oop* p, uintptr_t tag_bits, oop fwd) {
    if (tag_bits != 0) {
      RemoteHandle* h = remote_handle_managed_local_oop(_g1h, fwd)
          ? _rmm->ensure_handle_for(fwd, &_hab) : nullptr;
      if (h != nullptr) {
        *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
        _direct_converted++;
      } else {
        RawAccess<IS_NOT_NULL>::oop_store(p, fwd);
      }
    } else {
      RawAccess<IS_NOT_NULL>::oop_store(p, fwd);
    }
    dirty_card_for_field(p);
  }

  bool rescue_unforwarded(oop* p, uintptr_t tag_bits, oop target) {
    if (!_allow_rescue) return false;

    if (!is_valid_region_object(_g1h, target)) {
      _invalid++;
      return false;
    }

    const size_t word_size = target->size();
    if (word_size == 0 || word_size > HeapRegion::GrainWords) {
      _rescue_failed++;
      log_warning(gc)("Old/cset rescue SKIP: invalid target size target="
                      PTR_FORMAT " size=" SIZE_FORMAT " failures=%d",
                      p2i(target), word_size, _rescue_failed);
      return false;
    }

    const bool log_rescue = should_log_rescue();
    if (log_rescue) {
      log_debug(gc)("Old/cset rescue START: target=" PTR_FORMAT " size=" SIZE_FORMAT
                    " src_fcr=%s field=" PTR_FORMAT,
                    p2i(target), word_size, _src_is_fcr ? "true" : "false", p2i(p));
    }
    HeapWord* dst = _rmm->allocate_in_fcr(word_size);
    if (dst == nullptr) {
      _rescue_failed++;
      if (log_rescue) {
        log_warning(gc)("Old/cset rescue FAILED: target=" PTR_FORMAT
                        " size=" SIZE_FORMAT " failures=%d",
                        p2i(target), word_size, _rescue_failed);
      }
      return false;
    }

    Copy::aligned_disjoint_words(cast_from_oop<HeapWord*>(target), dst, word_size);
    oop rescued = cast_to_oop(dst);
    target->forward_to(rescued);
    _rmm->update_handle_for_evacuation(target, rescued);
    store_forwardee(p, tag_bits, rescued);
    _rescued++;
    if (log_rescue) {
      log_debug(gc)("Old/cset rescue DONE: target=" PTR_FORMAT " rescued=" PTR_FORMAT
                    " size=" SIZE_FORMAT " count=%d",
                    p2i(target), p2i(rescued), word_size, _rescued);
    }

    // The rescued object was missed by normal evacuation, so scan its fields
    // immediately; otherwise its internal cset references would remain stale.
    rescued->oop_iterate(this);
    // This path writes old/FCR heap fields outside normal G1 evacuation
    // barriers. Enqueue the source and rescued object cards so later GCs see
    // old-to-FCR/old refs through remembered-set processing.
    dirty_card_for_field(p);
    dirty_cards_for_range(dst, word_size);
    return true;
  }

  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;

    // Shared tagged oops point at a RemoteHandle, not directly at an object.
    // The handle table is fixed elsewhere. Direct/unique tagged oops still
    // contain an object address and must be forwarded just like clean oops.
    if ((raw & G1_OOP_INDIRECT_BIT) != 0) return;

    uintptr_t tag_bits = raw & G1_OOP_TAG_MASK;
    uintptr_t addr = (tag_bits != 0) ? (raw & G1_OOP_ADDR_MASK) : raw;
    if (!_g1h->is_in((void*)addr)) return;
    oop target = cast_to_oop(addr);
    if (!is_valid_region_object(_g1h, target)) {
      if (tag_bits != 0 && _rmm->dense_segments_enabled() &&
          _rmm->is_dense_segment_remote_addr(addr)) {
        return;
      }
      if (_g1h->is_in((void*)p)) {
        *(uintptr_t*)p = 0;
        dirty_card_for_field(p);
        _rmm->record_fcr_fixup_null();
        _invalid_nulled++;
      }
      _invalid++;
      return;
    }
    const G1HeapRegionAttr attr = _g1h->region_attr(target);
    if (!attr.is_in_cset()) return;
    markWord mw = target->mark();
    if (mw.is_marked()) {
      oop fwd = cast_to_oop(mw.decode_pointer());
      store_forwardee(p, tag_bits, fwd);
      _fixed++;
    } else {
      // Do not null Java fields here. This runs after cleanup_1, which has
      // restored preserved marks for evacuation-failed objects, so a live
      // in-place object can be unmarked even though references to it are valid.
      // Nulling such refs corrupts application objects (for example
      // java.lang.Thread.holder) and leads to VM crashes shortly after GC.
      if (rescue_unforwarded(p, tag_bits, target)) {
        return;
      }
      _skipped++;
    }
  }
  virtual void do_oop(narrowOop* p) {}
  int fixed() const { return _fixed; }
  int rescued() const { return _rescued; }
  int skipped() const { return _skipped; }
  int invalid() const { return _invalid; }
  int invalid_nulled() const { return _invalid_nulled; }
  int rescue_failed() const { return _rescue_failed; }
  int direct_converted() const { return _direct_converted; }
  int updated() const { return _fixed + _rescued + _invalid_nulled; }
};

static int old_cset_fixup_dirty_cards(G1CollectedHeap* g1h, HeapRegion* hr) {
  if (g1h == nullptr || hr == nullptr || hr->is_empty() || hr->is_free() ||
      hr->is_continues_humongous() || hr->bottom() >= hr->top()) {
    return 0;
  }

  G1CardTable* ct = g1h->card_table();
  CardTable::CardValue* card = ct->byte_for(hr->bottom());
  CardTable::CardValue* end_card = ct->byte_for(hr->top() - 1) + 1;
  int dirty = 0;
  while (card < end_card) {
    if (*card == G1CardTable::dirty_card_val()) {
      dirty++;
    }
    card++;
  }
  return dirty;
}

int G1RemoteMemoryManager::fixup_stale_refs_in_old_regions(bool evacuation_failed) {
  Ticks start = Ticks::now();
  log_debug(gc)("Old/cset fixup START (allow_rescue=%s)",
                evacuation_failed ? "false" : "true");

  CSetRefFixupClosure cl(_g1h, this, !evacuation_failed);
  const G1CMBitMap* bitmap = _g1h->concurrent_mark()->mark_bitmap();
  const uint num_regions = _g1h->max_reserved_regions();

  static uint old_cset_fixup_cycle = 0;
  const bool missing_source_map =
      old_cset_source_hint_count() == 0 &&
      (local_handle_count() > 0 || _dense_segment_evict_success > 0);
  const bool full_heap_fixup =
      !G1RemoteUseFastPhaseC ||
      G1RemoteFastPhaseCVerifyInterval == 0 ||
      G1RemoteFastPhaseCVerifyInterval == 1 ||
      missing_source_map ||
      (G1RemoteFastPhaseCVerifyInterval > 1 &&
       (old_cset_fixup_cycle % G1RemoteFastPhaseCVerifyInterval) == 0);

  class CSetFixupObjectClosure {
    CSetRefFixupClosure* _cl;
  public:
    CSetFixupObjectClosure(CSetRefFixupClosure* cl) : _cl(cl) {}
    void do_object(oop obj) {
      obj->oop_iterate(_cl);
    }
  };

  int regions_scanned = 0;
  int objects_scanned = 0;
  int selected_regions = 0;
  int fcr_regions = 0;
  int dirty_regions = 0;
  int hint_regions = 0;
  int phase_c_hint_regions = 0;
  int dirty_cards = 0;
  log_debug(gc)("Old/cset fixup heap scan START: heap_regions=%u", num_regions);
  for (uint i = 0; i < num_regions; i++) {
    HeapRegion* hr = _g1h->region_at_or_null(i);
    if (hr == nullptr) continue;
    if (hr->is_empty() || hr->is_free()) continue;
    if (hr->is_continues_humongous()) continue;
    if (!hr->is_old() && !hr->is_starts_humongous()) continue;

    bool selected = full_heap_fixup;
    bool selected_by_fcr = hr->is_fetch_cache();
    bool selected_by_hint = is_old_cset_source_hint(i);
    bool selected_by_phase_c_hint = is_fast_phase_c_source_hint(i);
    int region_dirty_cards = 0;
    if (!full_heap_fixup) {
      if (selected_by_fcr) {
        selected = true;
        fcr_regions++;
      } else if (selected_by_hint) {
        selected = true;
        hint_regions++;
      } else if (selected_by_phase_c_hint) {
        selected = true;
        phase_c_hint_regions++;
      } else {
        region_dirty_cards = old_cset_fixup_dirty_cards(_g1h, hr);
        if (region_dirty_cards > 0) {
          selected = true;
          dirty_regions++;
          dirty_cards += region_dirty_cards;
        }
      }
    }
    if (!selected) {
      continue;
    }

    selected_regions++;
    cl.set_src_is_fcr(hr->is_fetch_cache());
    CSetFixupObjectClosure obj_cl(&cl);
    Ticks region_start = Ticks::now();
    int before = cl.updated();
    log_trace(gc)("Old/cset fixup region START: index=%u type=%s bottom=" PTR_FORMAT
                  " top=" PTR_FORMAT " src_fcr=%s",
                  hr->hrm_index(), hr->get_short_type_str(), p2i(hr->bottom()),
                  p2i(hr->top()), hr->is_fetch_cache() ? "true" : "false");
    int scanned = remote_eviction_scan_region_objects(hr, bitmap, "Old/cset fixup", &obj_cl);
    double region_ms = (Ticks::now() - region_start).seconds() * 1000.0;
    regions_scanned++;
    objects_scanned += scanned;
    if (cl.updated() != before) {
      remember_old_cset_source_hint(i, "old-cset-repair");
    }
    if (region_ms > 100.0) {
      log_debug(gc)("Old/cset fixup region %u type=%s scanned=%d in %.1fms",
                    hr->hrm_index(), hr->get_short_type_str(), scanned, region_ms);
    }
  }
  if (!full_heap_fixup) {
    log_info(gc)("Old/cset fixup targeted sources: selected=%d fcr=%d dirty=%d "
                 "hints=%d phase_c_hints=%d dirty_cards=%d hint_total=%u "
                 "mode=targeted",
                 selected_regions, fcr_regions, dirty_regions, hint_regions,
                 phase_c_hint_regions, dirty_cards, old_cset_source_hint_count());
  } else if (missing_source_map) {
    log_info(gc)("Old/cset fixup full: missing source map with local_handles="
                 SIZE_FORMAT " dense_evicts=" UINT64_FORMAT,
                 local_handle_count(), (uint64_t)_dense_segment_evict_success);
  }
  log_debug(gc)("Old/cset fixup heap scan DONE: scanned %d regions, %d objects",
                regions_scanned, objects_scanned);

  class CSetFixupCodeBlobClosure : public CodeBlobClosure {
    G1CollectedHeap* _g1h;
    CSetRefFixupClosure* _cl;
    int _nmethods_updated;
  public:
    CSetFixupCodeBlobClosure(G1CollectedHeap* g1h, CSetRefFixupClosure* cl)
      : _g1h(g1h), _cl(cl), _nmethods_updated(0) {}

    void do_code_blob(CodeBlob* cb) override {
      nmethod* nm = cb->as_nmethod_or_null();
      if (nm == nullptr) return;

      int before = _cl->updated();
      nm->oops_do(_cl);
      if (_cl->updated() != before) {
        nm->fix_oop_relocations();
        _g1h->register_nmethod(nm);
        _nmethods_updated++;
      }
    }

    int nmethods_updated() const { return _nmethods_updated; }
  };

  CSetFixupCodeBlobClosure code_cl(_g1h, &cl);
  Ticks code_start = Ticks::now();
  log_debug(gc)("Old/cset fixup code scan START");
  CodeCache::blobs_do(&code_cl);
  double code_ms = (Ticks::now() - code_start).seconds() * 1000.0;
  log_debug(gc)("Old/cset fixup code scan DONE: %.1fms, %d nmethods updated",
                code_ms, code_cl.nmethods_updated());

  double elapsed_ms = (Ticks::now() - start).seconds() * 1000.0;
  log_debug(gc)("Old/cset fixup DONE: scanned %d regions, %d objects, code %.1fms, "
                "%d fixed, %d rescued, %d skipped, %d invalid, %d rescue-failed, "
                "%d direct-converted, %d nmethods updated in %.1fms",
                regions_scanned, objects_scanned, code_ms,
                cl.fixed(), cl.rescued(), cl.skipped(), cl.invalid(), cl.rescue_failed(),
                cl.direct_converted(), code_cl.nmethods_updated(), elapsed_ms);

  if (cl.fixed() > 0 || cl.rescued() > 0 || cl.skipped() > 0 ||
      cl.invalid() > 0 || cl.rescue_failed() > 0 ||
      cl.direct_converted() > 0 ||
      code_cl.nmethods_updated() > 0) {
    log_warning(gc)("Old/humongous-region stale-ref fixup: %d fixed (forwardee), "
                    "%d rescued, %d skipped (unforwarded/in-place), %d invalid "
                    "(%d nulled), "
                    "%d rescue-failed, %d direct-converted, %d nmethods updated; "
                    "FCR evac writes: %llu, previous FCR nulls: %llu",
                    cl.fixed(), cl.rescued(), cl.skipped(), cl.invalid(),
                    cl.invalid_nulled(), cl.rescue_failed(),
                    cl.direct_converted(), code_cl.nmethods_updated(),
                    (unsigned long long)fcr_evac_writes(),
                    (unsigned long long)fcr_fixup_nulls());
  }
  old_cset_fixup_cycle++;
  return cl.fixed() + cl.rescued() + cl.skipped() + cl.invalid() + cl.rescue_failed();
}

HeapWord* G1RemoteMemoryManager::allocate_in_fcr(size_t word_size) {
  // Fast path: try CAS bump pointer on existing FCR region (lock-free).
  HeapRegion* fcr = _current_fcr;
  if (fcr != nullptr && fcr->is_fetch_cache() && !fcr->is_free() &&
      !fcr->is_evict_guarded()) {
    size_t actual = 0;
    HeapWord* result = fcr->par_allocate(word_size, word_size, &actual);
    if (result != nullptr) {
      fcr->update_bot_for_obj(result, word_size);
      return result;
    }
  }

  // Current FCR full or doesn't exist. Allocate a new FCR region.
  // This path is reachable from resolve_tagged_oop_no_safepoint(), a leaf
  // runtime call that cannot block indefinitely while a VM handshake or
  // safepoint is pending.  If either coordination lock is contended, report a
  // retryable allocation failure to the fetch state machine instead of waiting.
  const uint FCRLockSpinLimit = 256;
  bool locked = false;
  for (uint spins = 0; spins < FCRLockSpinLimit; spins++) {
    if (try_fcr_lock()) {
      locked = true;
      break;
    }
    SpinPause();
  }
  if (!locked) {
    return nullptr;
  }

  if (_current_fcr != fcr) {
    fcr = _current_fcr;
    fcr_unlock();
    if (fcr != nullptr && fcr->is_fetch_cache() && !fcr->is_free() &&
        !fcr->is_evict_guarded()) {
      size_t actual = 0;
      HeapWord* result = fcr->par_allocate(word_size, word_size, &actual);
      if (result != nullptr) {
        fcr->update_bot_for_obj(result, word_size);
        return result;
      }
    }
    return nullptr;
  }

  HeapRegion* new_fcr = nullptr;
  if (SafepointSynchronize::is_at_safepoint()) {
    new_fcr = allocate_new_fcr_region();
  } else {
    if (!Heap_lock->try_lock()) {
      fcr_unlock();
      return nullptr;
    }
    new_fcr = allocate_new_fcr_region();
    Heap_lock->unlock();
  }
  if (new_fcr != nullptr) {
    _current_fcr = new_fcr;
    fcr_unlock();
    size_t actual = 0;
    HeapWord* result = new_fcr->par_allocate(word_size, word_size, &actual);
    if (result != nullptr) {
      new_fcr->update_bot_for_obj(result, word_size);
    }
    return result;
  }

  fcr_unlock();
  return nullptr;
}

// ============================================================
// Remote Executor Client — TCP communication
// NOTE: Executor client code is in g1RemoteBackendTcp.cpp.
// G1RemoteMemoryManager dispatches to _backend (SimLocal, TCP, or RDMA).

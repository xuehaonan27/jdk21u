/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 *
 * G1RemoteBackend — abstract interface for remote memory storage backends.
 *
 * The G1RemoteMemoryManager dispatches eviction, fetch, and collection
 * operations to the selected backend. Backends differ in transport/storage:
 *
 *   SimLocalBackend    — in-process memcpy (no network, for development)
 *   TCPExecutorBackend — TCP to remote_executor process (for testing)
 *   RDMAExecutorBackend — RDMA to remote_executor process (for production)
 *   (future) SSDBackend — NVMe/SPDK to local SSD (tiered memory)
 *
 * Selected at runtime via -XX:RemoteMemoryBackend=sim|tcp|rdma
 */

#ifndef SHARE_GC_G1_G1REMOTEBACKEND_HPP
#define SHARE_GC_G1_G1REMOTEBACKEND_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class Klass;

class G1RemoteBackend : public CHeapObj<mtGC> {
public:
  virtual ~G1RemoteBackend() {}

  // Backend name (for logging)
  virtual const char* name() const = 0;

  // Initialize connection/storage. Returns true on success.
  virtual bool initialize() = 0;

  // Evict: store object bytes remotely. Returns slot_id, or (size_t)-1 on failure.
  virtual size_t evict(const void* obj_bytes, size_t word_size,
                       Klass* klass, size_t hint_slot_id) = 0;

  // Fetch: retrieve object bytes from remote slot_id into dest.
  // Returns the Klass pointer, or nullptr on failure.
  // out_word_size: filled with the object's word size.
  virtual Klass* fetch(size_t slot_id, void* dest, size_t* out_word_size) = 0;

  // Report root set: these slot_ids are alive (referenced from local heap).
  // Called before collect_dead().
  virtual void report_roots(const size_t* root_slot_ids, size_t num_roots) = 0;

  // Collect dead objects: identify unreachable remote objects and free them.
  // Returns dead slot_ids in out_dead_ids (caller frees), sets out_num_dead.
  // Dead objects' storage is freed WITHOUT sending bytes back.
  virtual void collect_dead(size_t** out_dead_ids, size_t* out_num_dead,
                            size_t* out_bytes_freed) = 0;

  // Discard a specific slot (explicit free, e.g., after local object death).
  virtual void discard_slot(size_t slot_id) = 0;

  // Query slot metadata (word size) without fetching.
  virtual size_t slot_word_size(size_t slot_id) const = 0;

  // Statistics
  virtual size_t total_evicted() const = 0;
  virtual size_t total_fetched() const = 0;

  // Shutdown
  virtual void shutdown() = 0;
};

#endif // SHARE_GC_G1_G1REMOTEBACKEND_HPP

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

  // Allocate a slot_id for pre-assigned batch eviction.
  virtual size_t allocate_slot_id() { return (size_t)-1; }

  // Statistics
  virtual size_t total_evicted() const = 0;
  virtual size_t total_fetched() const = 0;

  // Shutdown
  virtual void shutdown() = 0;

  // ============================================================
  // V2 Protocol — handle-id-based operations (optional override)
  // ============================================================
  // Default implementations are no-ops. TCPExecutorBackend/RDMA override
  // to send V2 commands to the executor for edge-table tracing.

  struct EdgeInfo { uint32_t field_offset; uintptr_t target_handle_id; };

  // Evict with edge table: sends object bytes + sidecar edges + handle_id
  virtual size_t evict_with_edges(const void* obj_bytes, size_t word_size,
                                  Klass* klass, uintptr_t handle_id,
                                  const EdgeInfo* edges, uint32_t num_edges,
                                  size_t hint_slot_id) {
    // Default: fall back to V1 evict (edge table stays JVM-local)
    return evict(obj_bytes, word_size, klass, hint_slot_id);
  }

  // Batch evict: send multiple objects in a single message.
  // msg_buf is a pre-built CMD_BATCH_EVICT message (header + packed entries).
  // Returns number of objects successfully stored, or -1 on failure.
  virtual int batch_evict(const void* msg_buf, size_t msg_len) {
    return -1; // Not supported by default
  }

  // Notify executor that these handle_ids are now LOCAL (fetched back)
  virtual void localize_batch(const uintptr_t* handle_ids, size_t count) {
    // Default: no-op (SIM backend doesn't track handle state)
  }

  // Report remote roots as handle_ids (P12 concurrent marking data)
  virtual void report_remote_roots_v2(const uintptr_t* handle_ids, size_t count) {
    // Default: no-op
  }

  // Upsert handle directory entries on executor
  virtual void directory_upsert(const uintptr_t* handle_ids, const uint32_t* states,
                                const size_t* slot_ids, size_t count) {
    // Default: no-op
  }
};

#endif // SHARE_GC_G1_G1REMOTEBACKEND_HPP

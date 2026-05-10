/*
 * RDMAExecutorBackend — connects to remote_executor process via RDMA.
 * Uses ibverbs for one-sided RDMA READ/WRITE (data plane) and
 * RDMA SEND/RECV (control plane).
 *
 * Selected by: -XX:RemoteMemoryBackend=rdma
 * Requires: compile with -DREMOTE_EXECUTOR_USE_RDMA, link with -libverbs
 * Configure: -XX:RemoteExecutorHost=<ip> -XX:RemoteExecutorPort=<port>
 *
 * Control channel: RDMA SEND/RECV (reliable connected QP)
 * Data plane: RDMA WRITE for eviction, RDMA READ for fetch
 *
 * The remote executor pre-registers its memory arena with ibv_reg_mr().
 * The JVM client receives the remote rkey+base_addr during handshake.
 * Eviction: ibv_post_send(RDMA_WRITE, local_buf → remote_slot)
 * Fetch: ibv_post_send(RDMA_READ, remote_slot → local_buf)
 */

#ifndef SHARE_GC_G1_G1REMOTEBACKENDRDMA_HPP
#define SHARE_GC_G1_G1REMOTEBACKENDRDMA_HPP

#include "gc/g1/g1RemoteBackend.hpp"
#include "runtime/atomic.hpp"

#ifdef REMOTE_EXECUTOR_USE_RDMA

#include <infiniband/verbs.h>

class RDMAExecutorBackend : public G1RemoteBackend {
  static const size_t RemoteExecutorMaxSlots = 16 * 1024 * 1024;

  // RDMA resources
  struct ibv_context*    _ctx;
  struct ibv_pd*         _pd;
  struct ibv_cq*         _send_cq;
  struct ibv_cq*         _recv_cq;
  struct ibv_qp*         _qp;
  struct ibv_mr*         _local_mr;     // registered local memory for RDMA

  // Remote memory info (received during handshake)
  uint64_t _remote_base_addr;
  uint32_t _remote_rkey;
  size_t   _remote_arena_size;

  // Control channel (TCP for initial QP setup, then RDMA SEND/RECV)
  int _tcp_fd;  // TCP socket for initial handshake + QP metadata exchange

  // State
  bool     _connected;
  uint64_t _seq_id;
  size_t   _next_slot;
  size_t   _total_evicted;
  size_t   _total_fetched;
  uintptr_t* _pending_localize_ids;
  size_t   _pending_localize_count;
  size_t   _pending_localize_capacity;
  volatile int _io_lock;

  void io_lock()   { while (Atomic::cmpxchg(&_io_lock, 0, 1) != 0) { /* spin */ } }
  void io_unlock() { Atomic::release_store(&_io_lock, 0); }

  // RDMA helpers
  bool setup_rdma_resources();
  bool exchange_qp_info();   // TCP handshake to exchange QP metadata
  bool transition_qp_to_rts();

  // RDMA one-sided operations
  bool rdma_write(uint64_t remote_offset, const void* local_buf, size_t len);
  bool rdma_read(uint64_t remote_offset, void* local_buf, size_t len);

  // RDMA SEND/RECV for control messages
  bool rdma_send_msg(const void* data, size_t len);
  bool rdma_recv_msg(void* buf, size_t max_len, size_t* actual_len);
  // Split post/wait — post BEFORE the matching send to avoid RNR race.
  bool rdma_post_recv();
  bool rdma_wait_recv(void* buf, size_t max_len, size_t* actual_len);

  bool ensure_localize_buffer_locked();
  bool send_localize_batch_locked(const uintptr_t* handle_ids, size_t count);
  bool flush_localize_batch_locked();

public:
  RDMAExecutorBackend();
  ~RDMAExecutorBackend();

  const char* name() const override { return "rdma-executor"; }
  bool initialize() override;

  size_t evict(const void* obj_bytes, size_t word_size,
               Klass* klass, size_t hint_slot_id) override;
  Klass* fetch(size_t slot_id, void* dest, size_t* out_word_size) override;
  void report_roots(const size_t* root_slot_ids, size_t num_roots) override;
  void collect_dead(size_t** out_dead_ids, size_t* out_num_dead,
                    size_t* out_bytes_freed) override;
  void discard_slot(size_t slot_id) override;
  size_t slot_word_size(size_t slot_id) const override;

  size_t allocate_slot_id() override {
    if (_next_slot >= RemoteExecutorMaxSlots) return (size_t)-1;
    return _next_slot++;
  }
  size_t total_evicted() const override { return _total_evicted; }
  size_t total_fetched() const override { return _total_fetched; }
  void shutdown() override;

  int batch_evict(const void* msg_buf, size_t msg_len) override;
  bool supports_batch_evict() const override;
  size_t max_batch_evict_message_size() const override;
  bool supports_staged_homogeneous_batch_evict() const override;
  size_t max_staged_batch_data_size() const override;
  int batch_evict_staged_homogeneous(const void* msg_buf, size_t msg_len,
                                     const void* data_buf, size_t data_len,
                                     uint64_t remote_data_offset) override;
  bool supports_batch_fetch() const override;
  size_t fetch_batch_around(uintptr_t handle_id, size_t slot_id,
                            uint max_objects, uint slot_window,
                            size_t max_response_bytes,
                            FetchBatchClosure* cl) override;
  bool supports_exact_batch_fetch() const override;
  size_t fetch_batch_exact(const uintptr_t* handle_ids,
                           const size_t* slot_ids,
                           size_t count,
                           size_t max_response_bytes,
                           FetchBatchClosure* cl) override;
  bool supports_segments() const override;
  bool evict_segment(uint64_t segment_id, uintptr_t vaddr_base,
                     const void* bytes, size_t byte_size,
                     uint32_t flags) override;
  bool fetch_segment(uint64_t segment_id, uintptr_t* out_vaddr_base,
                     void* dest, size_t byte_capacity,
                     size_t* out_byte_size, uint32_t* out_flags) override;
  bool fetch_segment_part(uint64_t segment_id, size_t segment_offset,
                          void* dest, size_t byte_size,
                          uintptr_t* out_vaddr_base,
                          uint32_t* out_flags) override;
  void discard_segment(uint64_t segment_id) override;
  void trace_and_report(uintptr_t** out_dead_ids, size_t* out_num_dead,
                        size_t* out_bytes_freed,
                        uintptr_t** out_cross_src, uintptr_t** out_cross_tgt,
                        size_t* out_num_cross) override;

  size_t evict_with_edges(const void* obj_bytes, size_t word_size,
                          Klass* klass, uintptr_t handle_id,
                          const EdgeInfo* edges, uint32_t num_edges,
                          size_t hint_slot_id) override;
  void localize_batch(const uintptr_t* handle_ids, size_t count) override;
  void report_remote_roots_v2(const uintptr_t* handle_ids, size_t count) override;
  void directory_upsert(const uintptr_t* handle_ids, const uint32_t* states,
                        const size_t* slot_ids, size_t count) override;
};

#else // !REMOTE_EXECUTOR_USE_RDMA

// Stub when RDMA not available
class RDMAExecutorBackend : public G1RemoteBackend {
public:
  const char* name() const override { return "rdma-executor (unavailable)"; }
  bool initialize() override {
    // RDMA not compiled in
    return false;
  }
  size_t evict(const void*, size_t, Klass*, size_t) override { return (size_t)-1; }
  Klass* fetch(size_t, void*, size_t*) override { return nullptr; }
  void report_roots(const size_t*, size_t) override {}
  void collect_dead(size_t** out_dead, size_t* out_n, size_t* out_b) override {
    *out_dead = nullptr; *out_n = 0; *out_b = 0;
  }
  void discard_slot(size_t) override {}
  size_t slot_word_size(size_t) const override { return 0; }
  size_t total_evicted() const override { return 0; }
  size_t total_fetched() const override { return 0; }
  void shutdown() override {}
};

#endif // REMOTE_EXECUTOR_USE_RDMA

#endif // SHARE_GC_G1_G1REMOTEBACKENDRDMA_HPP

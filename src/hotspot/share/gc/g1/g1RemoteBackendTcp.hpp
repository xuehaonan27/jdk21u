/*
 * TCPExecutorBackend — connects to remote_executor process via TCP.
 * Simulates RDMA SEND/RECV semantics over TCP sockets.
 * For testing with a real separate process on localhost or remote machine.
 *
 * Selected by: -XX:RemoteMemoryBackend=tcp
 * Configure:   -XX:RemoteExecutorHost=<ip> -XX:RemoteExecutorPort=<port>
 */

#ifndef SHARE_GC_G1_G1REMOTEBACKENDTCP_HPP
#define SHARE_GC_G1_G1REMOTEBACKENDTCP_HPP

#include "gc/g1/g1RemoteBackend.hpp"
#include "runtime/atomic.hpp"

class TCPExecutorBackend : public G1RemoteBackend {
  static const size_t RemoteExecutorMaxSlots = 16 * 1024 * 1024;

  int      _fd;              // TCP socket
  bool     _connected;
  uint64_t _seq_id;
  size_t   _next_slot;
  size_t   _total_evicted;
  size_t   _total_fetched;
  volatile int _io_lock;

  void io_lock()   { while (Atomic::cmpxchg(&_io_lock, 0, 1) != 0) { /* spin */ } }
  void io_unlock() { Atomic::release_store(&_io_lock, 0); }

  bool send_all(const void* data, size_t len);
  bool recv_all(void* buf, size_t len);
  bool send_msg(const void* data, size_t len);
  bool recv_msg(void* buf, size_t max_len, size_t* actual_len);
  bool send_hello();

public:
  TCPExecutorBackend();
  ~TCPExecutorBackend();

  const char* name() const override { return "tcp-executor"; }
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

  bool supports_segments() const override;
  bool evict_segment(uint64_t segment_id, uintptr_t vaddr_base,
                     const void* bytes, size_t byte_size,
                     uint32_t flags) override;
  bool fetch_segment(uint64_t segment_id, uintptr_t* out_vaddr_base,
                     void* dest, size_t byte_capacity,
                     size_t* out_byte_size, uint32_t* out_flags) override;
  void discard_segment(uint64_t segment_id) override;

  // V2 Protocol overrides
  size_t evict_with_edges(const void* obj_bytes, size_t word_size,
                          Klass* klass, uintptr_t handle_id,
                          const EdgeInfo* edges, uint32_t num_edges,
                          size_t hint_slot_id) override;
  void localize_batch(const uintptr_t* handle_ids, size_t count) override;
  void report_remote_roots_v2(const uintptr_t* handle_ids, size_t count) override;
  void directory_upsert(const uintptr_t* handle_ids, const uint32_t* states,
                        const size_t* slot_ids, size_t count) override;
  void trace_and_report(uintptr_t** out_dead_ids, size_t* out_num_dead,
                        size_t* out_bytes_freed,
                        uintptr_t** out_cross_src, uintptr_t** out_cross_tgt,
                        size_t* out_num_cross) override;
};

#endif // SHARE_GC_G1_G1REMOTEBACKENDTCP_HPP

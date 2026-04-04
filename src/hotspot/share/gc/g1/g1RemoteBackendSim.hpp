/*
 * SimLocalBackend — in-process simulated remote memory.
 * No network, no separate process. Object bytes stored in local malloc buffers.
 * For development and testing without remote infrastructure.
 *
 * Selected by: -XX:RemoteMemoryBackend=sim (default)
 */

#ifndef SHARE_GC_G1_G1REMOTEBACKENDSIM_HPP
#define SHARE_GC_G1_G1REMOTEBACKENDSIM_HPP

#include "gc/g1/g1RemoteBackend.hpp"

class SimLocalBackend : public G1RemoteBackend {
  struct Slot {
    void*   data;
    size_t  word_size;
    Klass*  klass;
    bool    in_use;
    bool    marked;  // for collection: marked alive?
  };

  static const size_t MAX_SLOTS = 1024;
  Slot    _slots[MAX_SLOTS];
  size_t  _next_slot;
  size_t  _total_evicted;
  size_t  _total_fetched;

  // Root set for collection
  size_t* _root_ids;
  size_t  _num_roots;

public:
  SimLocalBackend();
  ~SimLocalBackend();

  const char* name() const override { return "sim-local"; }
  bool initialize() override;

  size_t evict(const void* obj_bytes, size_t word_size,
               Klass* klass, size_t hint_slot_id) override;

  Klass* fetch(size_t slot_id, void* dest, size_t* out_word_size) override;

  void report_roots(const size_t* root_slot_ids, size_t num_roots) override;

  void collect_dead(size_t** out_dead_ids, size_t* out_num_dead,
                    size_t* out_bytes_freed) override;

  void discard_slot(size_t slot_id) override;

  size_t slot_word_size(size_t slot_id) const override;

  size_t total_evicted() const override { return _total_evicted; }
  size_t total_fetched() const override { return _total_fetched; }

  void shutdown() override;
};

#endif // SHARE_GC_G1_G1REMOTEBACKENDSIM_HPP

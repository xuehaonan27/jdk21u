#include "gc/g1/g1DrainLocalQueue.hpp"

#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ConcurrentMark.hpp"
#include "jni.h"
#include "logging/log.hpp"

void DrainLocalQueues::perform() {
  log_info(gc, heap)("Called DrainLocalQueues");
  G1CollectedHeap *g1h = G1CollectedHeap::heap();
  if (g1h == nullptr)
    return;

  G1ConcurrentMark *cm = g1h->concurrent_mark();
  if (cm == nullptr)
    return;

  uint num_active = cm->active_tasks();
  for (uint i = 0; i < num_active; ++i) {
    G1CMTask *task = cm->task(i);
    if (task != nullptr) {
      // Synchronization?
      task->drain_local_queue(true);
    }
  }
}

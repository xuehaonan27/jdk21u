/*
 * Copyright (c) 2021, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "classfile/vmClasses.hpp"
#include "classfile/classLoaderDataGraph.inline.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "compiler/oopMap.hpp"
#include "gc/g1/g1Allocator.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1CardSetMemory.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1ConcurrentMark.hpp"
#include "gc/g1/g1GCPhaseTimes.hpp"
#include "gc/g1/g1YoungGCEvacFailureInjector.hpp"
#include "gc/g1/g1EvacInfo.hpp"
#include "gc/g1/g1HRPrinter.hpp"
#include "gc/g1/g1MonitoringSupport.hpp"
#include "gc/g1/g1ParScanThreadState.inline.hpp"
#include "gc/g1/g1Analytics.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RemoteBackend.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/g1/g1RedirtyCardsQueue.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1RootProcessor.hpp"
#include "gc/g1/g1Trace.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionSet.hpp"
#include "gc/g1/g1YoungCollector.hpp"
#include "gc/g1/g1YoungGCPostEvacuateTasks.hpp"
#include "gc/g1/g1YoungGCPreEvacuateTasks.hpp"
#include "gc/g1/g1_globals.hpp"
#include "gc/shared/concurrentGCBreakpoints.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/preservedMarks.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/weakProcessor.inline.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/workerThread.hpp"
#include "jfr/jfrEvents.hpp"
#include "memory/resourceArea.hpp"
#include "gc/shared/oopStorage.inline.hpp"
#include "gc/shared/oopStorageSet.inline.hpp"
#include "memory/universe.hpp"
#include "oops/klass.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/os.hpp"
#include "runtime/threads.hpp"
#include "utilities/ticks.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// GCTraceTime wrapper that constructs the message according to GC pause type and
// GC cause.
// The code relies on the fact that GCTraceTimeWrapper stores the string passed
// initially as a reference only, so that we can modify it as needed.
class G1YoungGCTraceTime {
  G1YoungCollector* _collector;

  G1GCPauseType _pause_type;
  GCCause::Cause _pause_cause;

  static const uint MaxYoungGCNameLength = 128;
  char _young_gc_name_data[MaxYoungGCNameLength];

  GCTraceTime(Info, gc) _tt;

  const char* update_young_gc_name() {
    snprintf(_young_gc_name_data,
             MaxYoungGCNameLength,
             "Pause Young (%s) (%s)%s",
             G1GCPauseTypeHelper::to_string(_pause_type),
             GCCause::to_string(_pause_cause),
             _collector->evacuation_failed() ? " (Evacuation Failure)" : "");
    return _young_gc_name_data;
  }

public:
  G1YoungGCTraceTime(G1YoungCollector* collector, GCCause::Cause cause) :
    _collector(collector),
    // Take snapshot of current pause type at start as it may be modified during gc.
    // The strings for all Concurrent Start pauses are the same, so the parameter
    // does not matter here.
    _pause_type(_collector->collector_state()->young_gc_pause_type(false /* concurrent_operation_is_full_mark */)),
    _pause_cause(cause),
    // Fake a "no cause" and manually add the correct string in update_young_gc_name()
    // to make the string look more natural.
    _tt(update_young_gc_name(), nullptr, GCCause::_no_gc, true) {
  }

  ~G1YoungGCTraceTime() {
    update_young_gc_name();
  }
};

class G1YoungGCNotifyPauseMark : public StackObj {
  G1YoungCollector* _collector;

public:
  G1YoungGCNotifyPauseMark(G1YoungCollector* collector) : _collector(collector) {
    G1CollectedHeap::heap()->policy()->record_young_gc_pause_start();
  }

  ~G1YoungGCNotifyPauseMark() {
    G1CollectedHeap::heap()->policy()->record_young_gc_pause_end(_collector->evacuation_failed());
  }
};

class G1YoungGCJFRTracerMark : public G1JFRTracerMark {
  G1EvacInfo _evacuation_info;

  G1NewTracer* tracer() const { return (G1NewTracer*)_tracer; }

public:

  G1EvacInfo* evacuation_info() { return &_evacuation_info; }

  G1YoungGCJFRTracerMark(STWGCTimer* gc_timer_stw, G1NewTracer* gc_tracer_stw, GCCause::Cause cause) :
    G1JFRTracerMark(gc_timer_stw, gc_tracer_stw), _evacuation_info() { }

  void report_pause_type(G1GCPauseType type) {
    tracer()->report_young_gc_pause(type);
  }

  ~G1YoungGCJFRTracerMark() {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    tracer()->report_evacuation_info(&_evacuation_info);
    tracer()->report_tenuring_threshold(g1h->policy()->tenuring_threshold());
  }
};

class G1YoungGCVerifierMark : public StackObj {
  G1YoungCollector* _collector;
  G1HeapVerifier::G1VerifyType _type;

  static G1HeapVerifier::G1VerifyType young_collection_verify_type() {
    G1CollectorState* state = G1CollectedHeap::heap()->collector_state();
    if (state->in_concurrent_start_gc()) {
      return G1HeapVerifier::G1VerifyConcurrentStart;
    } else if (state->in_young_only_phase()) {
      return G1HeapVerifier::G1VerifyYoungNormal;
    } else {
      return G1HeapVerifier::G1VerifyMixed;
    }
  }

public:
  G1YoungGCVerifierMark(G1YoungCollector* collector) : _collector(collector), _type(young_collection_verify_type()) {
    G1CollectedHeap::heap()->verify_before_young_collection(_type);
  }

  ~G1YoungGCVerifierMark() {
    // Inject evacuation failure tag into type if needed.
    G1HeapVerifier::G1VerifyType type = _type;
    if (_collector->evacuation_failed()) {
      type = (G1HeapVerifier::G1VerifyType)(type | G1HeapVerifier::G1VerifyYoungEvacFail);
    }
    G1CollectedHeap::heap()->verify_after_young_collection(type);
  }
};

G1Allocator* G1YoungCollector::allocator() const {
  return _g1h->allocator();
}

G1CollectionSet* G1YoungCollector::collection_set() const {
  return _g1h->collection_set();
}

G1CollectorState* G1YoungCollector::collector_state() const {
  return _g1h->collector_state();
}

G1ConcurrentMark* G1YoungCollector::concurrent_mark() const {
  return _g1h->concurrent_mark();
}

STWGCTimer* G1YoungCollector::gc_timer_stw() const {
  return _g1h->gc_timer_stw();
}

G1NewTracer* G1YoungCollector::gc_tracer_stw() const {
  return _g1h->gc_tracer_stw();
}

G1Policy* G1YoungCollector::policy() const {
  return _g1h->policy();
}

G1GCPhaseTimes* G1YoungCollector::phase_times() const {
  return _g1h->phase_times();
}

G1HRPrinter* G1YoungCollector::hr_printer() const {
  return _g1h->hr_printer();
}

G1MonitoringSupport* G1YoungCollector::monitoring_support() const {
  return _g1h->monitoring_support();
}

G1RemSet* G1YoungCollector::rem_set() const {
  return _g1h->rem_set();
}

G1ScannerTasksQueueSet* G1YoungCollector::task_queues() const {
  return _g1h->task_queues();
}

G1SurvivorRegions* G1YoungCollector::survivor_regions() const {
  return _g1h->survivor();
}

ReferenceProcessor* G1YoungCollector::ref_processor_stw() const {
  return _g1h->ref_processor_stw();
}

WorkerThreads* G1YoungCollector::workers() const {
  return _g1h->workers();
}

G1YoungGCEvacFailureInjector* G1YoungCollector::evac_failure_injector() const {
  return _g1h->evac_failure_injector();
}


void G1YoungCollector::wait_for_root_region_scanning() {
  Ticks start = Ticks::now();
  // We have to wait until the CM threads finish scanning the
  // root regions as it's the only way to ensure that all the
  // objects on them have been correctly scanned before we start
  // moving them during the GC.
  bool waited = concurrent_mark()->wait_until_root_region_scan_finished();
  Tickspan wait_time;
  if (waited) {
    wait_time = (Ticks::now() - start);
  }
  phase_times()->record_root_region_scan_wait_time(wait_time.seconds() * MILLIUNITS);
}

class G1PrintCollectionSetClosure : public HeapRegionClosure {
private:
  G1HRPrinter* _hr_printer;
public:
  G1PrintCollectionSetClosure(G1HRPrinter* hr_printer) : HeapRegionClosure(), _hr_printer(hr_printer) { }

  virtual bool do_heap_region(HeapRegion* r) {
    _hr_printer->cset(r);
    return false;
  }
};

void G1YoungCollector::calculate_collection_set(G1EvacInfo* evacuation_info, double target_pause_time_ms) {
  // Forget the current allocation region (we might even choose it to be part
  // of the collection set!) before finalizing the collection set.
  allocator()->release_mutator_alloc_regions();

  collection_set()->finalize_initial_collection_set(target_pause_time_ms, survivor_regions());
  evacuation_info->set_collection_set_regions(collection_set()->region_length() +
                                              collection_set()->optional_region_length());

  concurrent_mark()->verify_no_collection_set_oops();

  if (hr_printer()->is_active()) {
    G1PrintCollectionSetClosure cl(hr_printer());
    collection_set()->iterate(&cl);
    collection_set()->iterate_optional(&cl);
  }
}

class G1PrepareEvacuationTask : public WorkerTask {
  class G1PrepareRegionsClosure : public HeapRegionClosure {
    G1CollectedHeap* _g1h;
    G1PrepareEvacuationTask* _parent_task;
    uint _worker_humongous_total;
    uint _worker_humongous_candidates;

    G1MonotonicArenaMemoryStats _card_set_stats;

    void sample_card_set_size(HeapRegion* hr) {
      // Sample card set sizes for young gen and humongous before GC: this makes
      // the policy to give back memory to the OS keep the most recent amount of
      // memory for these regions.
      if (hr->is_young() || hr->is_starts_humongous()) {
        _card_set_stats.add(hr->rem_set()->card_set_memory_stats());
      }
    }

    bool humongous_region_is_candidate(HeapRegion* region) const {
      assert(region->is_starts_humongous(), "Must start a humongous object");

      oop obj = cast_to_oop(region->bottom());

      // Dead objects cannot be eager reclaim candidates. Due to class
      // unloading it is unsafe to query their classes so we return early.
      if (_g1h->is_obj_dead(obj, region)) {
        return false;
      }

      // If we do not have a complete remembered set for the region, then we can
      // not be sure that we have all references to it.
      if (!region->rem_set()->is_complete()) {
        return false;
      }
      // Candidate selection must satisfy the following constraints
      // while concurrent marking is in progress:
      //
      // * In order to maintain SATB invariants, an object must not be
      // reclaimed if it was allocated before the start of marking and
      // has not had its references scanned.  Such an object must have
      // its references (including type metadata) scanned to ensure no
      // live objects are missed by the marking process.  Objects
      // allocated after the start of concurrent marking don't need to
      // be scanned.
      //
      // * An object must not be reclaimed if it is on the concurrent
      // mark stack.  Objects allocated after the start of concurrent
      // marking are never pushed on the mark stack.
      //
      // Nominating only objects allocated after the start of concurrent
      // marking is sufficient to meet both constraints.  This may miss
      // some objects that satisfy the constraints, but the marking data
      // structures don't support efficiently performing the needed
      // additional tests or scrubbing of the mark stack.
      //
      // However, we presently only nominate is_typeArray() objects.
      // A humongous object containing references induces remembered
      // set entries on other regions.  In order to reclaim such an
      // object, those remembered sets would need to be cleaned up.
      //
      // We also treat is_typeArray() objects specially, allowing them
      // to be reclaimed even if allocated before the start of
      // concurrent mark.  For this we rely on mark stack insertion to
      // exclude is_typeArray() objects, preventing reclaiming an object
      // that is in the mark stack.  We also rely on the metadata for
      // such objects to be built-in and so ensured to be kept live.
      // Frequent allocation and drop of large binary blobs is an
      // important use case for eager reclaim, and this special handling
      // may reduce needed headroom.

      return obj->is_typeArray() &&
             _g1h->is_potential_eager_reclaim_candidate(region);
    }

  public:
    G1PrepareRegionsClosure(G1CollectedHeap* g1h, G1PrepareEvacuationTask* parent_task) :
      _g1h(g1h),
      _parent_task(parent_task),
      _worker_humongous_total(0),
      _worker_humongous_candidates(0) { }

    ~G1PrepareRegionsClosure() {
      _parent_task->add_humongous_candidates(_worker_humongous_candidates);
      _parent_task->add_humongous_total(_worker_humongous_total);
    }

    virtual bool do_heap_region(HeapRegion* hr) {
      // First prepare the region for scanning
      _g1h->rem_set()->prepare_region_for_scan(hr);

      sample_card_set_size(hr);

      // Now check if region is a humongous candidate
      if (!hr->is_starts_humongous()) {
        _g1h->register_region_with_region_attr(hr);
        return false;
      }

      uint index = hr->hrm_index();
      if (humongous_region_is_candidate(hr)) {
        _g1h->register_humongous_candidate_region_with_region_attr(index);
        _worker_humongous_candidates++;
        // We will later handle the remembered sets of these regions.
      } else {
        _g1h->register_region_with_region_attr(hr);
      }
      log_debug(gc, humongous)("Humongous region %u (object size %zu @ " PTR_FORMAT ") remset %zu code roots %zu marked %d reclaim candidate %d type array %d",
                               index,
                               cast_to_oop(hr->bottom())->size() * HeapWordSize,
                               p2i(hr->bottom()),
                               hr->rem_set()->occupied(),
                               hr->rem_set()->code_roots_list_length(),
                               _g1h->concurrent_mark()->mark_bitmap()->is_marked(hr->bottom()),
                               _g1h->is_humongous_reclaim_candidate(index),
                               cast_to_oop(hr->bottom())->is_typeArray()
                              );
      _worker_humongous_total++;

      return false;
    }

    G1MonotonicArenaMemoryStats card_set_stats() const {
      return _card_set_stats;
    }
  };

  G1CollectedHeap* _g1h;
  HeapRegionClaimer _claimer;
  volatile uint _humongous_total;
  volatile uint _humongous_candidates;

  G1MonotonicArenaMemoryStats _all_card_set_stats;

public:
  G1PrepareEvacuationTask(G1CollectedHeap* g1h) :
    WorkerTask("Prepare Evacuation"),
    _g1h(g1h),
    _claimer(_g1h->workers()->active_workers()),
    _humongous_total(0),
    _humongous_candidates(0) { }

  void work(uint worker_id) {
    G1PrepareRegionsClosure cl(_g1h, this);
    _g1h->heap_region_par_iterate_from_worker_offset(&cl, &_claimer, worker_id);

    MutexLocker x(G1RareEvent_lock, Mutex::_no_safepoint_check_flag);
    _all_card_set_stats.add(cl.card_set_stats());
  }

  void add_humongous_candidates(uint candidates) {
    Atomic::add(&_humongous_candidates, candidates);
  }

  void add_humongous_total(uint total) {
    Atomic::add(&_humongous_total, total);
  }

  uint humongous_candidates() {
    return _humongous_candidates;
  }

  uint humongous_total() {
    return _humongous_total;
  }

  const G1MonotonicArenaMemoryStats all_card_set_stats() const {
    return _all_card_set_stats;
  }
};

Tickspan G1YoungCollector::run_task_timed(WorkerTask* task) {
  Ticks start = Ticks::now();
  workers()->run_task(task);
  return Ticks::now() - start;
}

void G1YoungCollector::set_young_collection_default_active_worker_threads(){
  uint active_workers = WorkerPolicy::calc_active_workers(workers()->max_workers(),
                                                          workers()->active_workers(),
                                                          Threads::number_of_non_daemon_threads());
  active_workers = workers()->set_active_workers(active_workers);
  log_info(gc,task)("Using %u workers of %u for evacuation", active_workers, workers()->max_workers());
}

void G1YoungCollector::pre_evacuate_collection_set(G1EvacInfo* evacuation_info) {
  {
    Ticks start = Ticks::now();
    G1PreEvacuateCollectionSetBatchTask cl;
    G1CollectedHeap::heap()->run_batch_task(&cl);
    phase_times()->record_pre_evacuate_prepare_time_ms((Ticks::now() - start).seconds() * 1000.0);
  }

  // Needs log buffers flushed.
  calculate_collection_set(evacuation_info, policy()->max_pause_time_ms());

  // Please see comment in g1CollectedHeap.hpp and
  // G1CollectedHeap::ref_processing_init() to see how
  // reference processing currently works in G1.
  ref_processor_stw()->start_discovery(false /* always_clear */);

  _evac_failure_regions.pre_collection(_g1h->max_reserved_regions());

  _g1h->gc_prologue(false);

  // Initialize the GC alloc regions.
  allocator()->init_gc_alloc_regions(evacuation_info);

  {
    Ticks start = Ticks::now();
    rem_set()->prepare_for_scan_heap_roots();
    phase_times()->record_prepare_heap_roots_time_ms((Ticks::now() - start).seconds() * 1000.0);
  }

  {
    G1PrepareEvacuationTask g1_prep_task(_g1h);
    Tickspan task_time = run_task_timed(&g1_prep_task);

    _g1h->set_young_gen_card_set_stats(g1_prep_task.all_card_set_stats());
    _g1h->set_humongous_stats(g1_prep_task.humongous_total(), g1_prep_task.humongous_candidates());

    phase_times()->record_register_regions(task_time.seconds() * 1000.0);
  }

  assert(_g1h->verifier()->check_region_attr_table(), "Inconsistency in the region attributes table.");

#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif

  if (collector_state()->in_concurrent_start_gc()) {
    concurrent_mark()->pre_concurrent_start(_gc_cause);
  }

  evac_failure_injector()->arm_if_needed();
}

class G1ParEvacuateFollowersClosure : public VoidClosure {
  double _start_term;
  double _term_time;
  size_t _term_attempts;

  void start_term_time() { _term_attempts++; _start_term = os::elapsedTime(); }
  void end_term_time() { _term_time += (os::elapsedTime() - _start_term); }

  G1CollectedHeap*              _g1h;
  G1ParScanThreadState*         _par_scan_state;
  G1ScannerTasksQueueSet*       _queues;
  TaskTerminator*               _terminator;
  G1GCPhaseTimes::GCParPhases   _phase;

  G1ParScanThreadState*   par_scan_state() { return _par_scan_state; }
  G1ScannerTasksQueueSet* queues()         { return _queues; }
  TaskTerminator*         terminator()     { return _terminator; }

  inline bool offer_termination() {
    EventGCPhaseParallel event;
    G1ParScanThreadState* const pss = par_scan_state();
    start_term_time();
    const bool res = (terminator() == nullptr) ? true : terminator()->offer_termination();
    end_term_time();
    event.commit(GCId::current(), pss->worker_id(), G1GCPhaseTimes::phase_name(G1GCPhaseTimes::Termination));
    return res;
  }

public:
  G1ParEvacuateFollowersClosure(G1CollectedHeap* g1h,
                                G1ParScanThreadState* par_scan_state,
                                G1ScannerTasksQueueSet* queues,
                                TaskTerminator* terminator,
                                G1GCPhaseTimes::GCParPhases phase)
    : _start_term(0.0), _term_time(0.0), _term_attempts(0),
      _g1h(g1h), _par_scan_state(par_scan_state),
      _queues(queues), _terminator(terminator), _phase(phase) {}

  void do_void() {
    EventGCPhaseParallel event;
    G1ParScanThreadState* const pss = par_scan_state();
    pss->trim_queue();
    event.commit(GCId::current(), pss->worker_id(), G1GCPhaseTimes::phase_name(_phase));
    do {
      EventGCPhaseParallel event;
      pss->steal_and_trim_queue(queues());
      event.commit(GCId::current(), pss->worker_id(), G1GCPhaseTimes::phase_name(_phase));
    } while (!offer_termination());
  }

  double term_time() const { return _term_time; }
  size_t term_attempts() const { return _term_attempts; }
};

class G1EvacuateRegionsBaseTask : public WorkerTask {
protected:
  G1CollectedHeap* _g1h;
  G1ParScanThreadStateSet* _per_thread_states;

  G1ScannerTasksQueueSet* _task_queues;
  TaskTerminator _terminator;

  uint _num_workers;

  void evacuate_live_objects(G1ParScanThreadState* pss,
                             uint worker_id,
                             G1GCPhaseTimes::GCParPhases objcopy_phase,
                             G1GCPhaseTimes::GCParPhases termination_phase) {
    G1GCPhaseTimes* p = _g1h->phase_times();

    Ticks start = Ticks::now();
    G1ParEvacuateFollowersClosure cl(_g1h, pss, _task_queues, &_terminator, objcopy_phase);
    cl.do_void();

    assert(pss->queue_is_empty(), "should be empty");

    Tickspan evac_time = (Ticks::now() - start);
    p->record_or_add_time_secs(objcopy_phase, worker_id, evac_time.seconds() - cl.term_time());

    if (termination_phase == G1GCPhaseTimes::Termination) {
      p->record_time_secs(termination_phase, worker_id, cl.term_time());
      p->record_thread_work_item(termination_phase, worker_id, cl.term_attempts());
    } else {
      p->record_or_add_time_secs(termination_phase, worker_id, cl.term_time());
      p->record_or_add_thread_work_item(termination_phase, worker_id, cl.term_attempts());
    }
    assert(pss->trim_ticks().value() == 0,
           "Unexpected partial trimming during evacuation value " JLONG_FORMAT,
           pss->trim_ticks().value());
  }

  virtual void start_work(uint worker_id) { }

  virtual void end_work(uint worker_id) { }

  virtual void scan_roots(G1ParScanThreadState* pss, uint worker_id) = 0;

  virtual void evacuate_live_objects(G1ParScanThreadState* pss, uint worker_id) = 0;

public:
  G1EvacuateRegionsBaseTask(const char* name,
                            G1ParScanThreadStateSet* per_thread_states,
                            G1ScannerTasksQueueSet* task_queues,
                            uint num_workers) :
    WorkerTask(name),
    _g1h(G1CollectedHeap::heap()),
    _per_thread_states(per_thread_states),
    _task_queues(task_queues),
    _terminator(num_workers, _task_queues),
    _num_workers(num_workers)
  { }

  void work(uint worker_id) {
    start_work(worker_id);

    {
      ResourceMark rm;

      G1ParScanThreadState* pss = _per_thread_states->state_for_worker(worker_id);
      pss->set_ref_discoverer(_g1h->ref_processor_stw());

      scan_roots(pss, worker_id);
      evacuate_live_objects(pss, worker_id);
    }

    end_work(worker_id);
  }
};

class G1EvacuateRegionsTask : public G1EvacuateRegionsBaseTask {
  G1RootProcessor* _root_processor;
  bool _has_optional_evacuation_work;

  void scan_roots(G1ParScanThreadState* pss, uint worker_id) {
    _root_processor->evacuate_roots(pss, worker_id);
    _g1h->rem_set()->scan_heap_roots(pss, worker_id, G1GCPhaseTimes::ScanHR, G1GCPhaseTimes::ObjCopy, _has_optional_evacuation_work);
    _g1h->rem_set()->scan_collection_set_code_roots(pss, worker_id, G1GCPhaseTimes::CodeRoots, G1GCPhaseTimes::ObjCopy);

    // Worker 0 scans tagged field roots: ensures Handle targets in the
    // cset are evacuated even if no card/remset entry points to them.
    if (worker_id == 0) {
      G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
      if (rmm != nullptr && rmm->tagged_field_count() > 0) {
        int count = rmm->tagged_field_count();
        const G1RemoteMemoryManager::TaggedFieldEntry* entries = rmm->tagged_fields();
        int evacuated = 0;
        for (int i = 0; i < count; i++) {
          RemoteHandle* h = entries[i]._handle;
          uintptr_t sa = h->load_state_and_addr_acquire();
          uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
          if (state != REMOTE_HANDLE_LOCAL) continue;

          oop target = cast_to_oop(sa & REMOTE_HANDLE_ADDR_MASK);
          if (target == nullptr || !_g1h->is_in(target)) continue;

          const G1HeapRegionAttr region_attr = _g1h->region_attr(target);
          if (!region_attr.is_in_cset()) continue;

          markWord m = target->mark();
          oop forwardee;
          if (m.is_marked()) {
            forwardee = cast_to_oop(m.decode_pointer());
          } else {
            forwardee = pss->copy_to_survivor_space(region_attr, target, m);
          }
          if (forwardee != nullptr) {
            rmm->update_handle_for_evacuation(h, target, forwardee);
            evacuated++;
          }
        }
        if (evacuated > 0) {
          log_info(gc)("Tagged field root scan: evacuated %d Handle targets from cset", evacuated);
        }
      }
    }

    // There are no optional roots to scan right now.
#ifdef ASSERT
    class VerifyOptionalCollectionSetRootsEmptyClosure : public HeapRegionClosure {
      G1ParScanThreadState* _pss;

    public:
      VerifyOptionalCollectionSetRootsEmptyClosure(G1ParScanThreadState* pss) : _pss(pss) { }

      bool do_heap_region(HeapRegion* r) override {
        assert(!r->has_index_in_opt_cset(), "must be");
        return false;
      }
    } cl(pss);
    _g1h->collection_set_iterate_increment_from(&cl, worker_id);
#endif
  }

  void evacuate_live_objects(G1ParScanThreadState* pss, uint worker_id) {
    G1EvacuateRegionsBaseTask::evacuate_live_objects(pss, worker_id, G1GCPhaseTimes::ObjCopy, G1GCPhaseTimes::Termination);
  }

  void start_work(uint worker_id) {
    _g1h->phase_times()->record_time_secs(G1GCPhaseTimes::GCWorkerStart, worker_id, Ticks::now().seconds());
  }

  void end_work(uint worker_id) {
    _g1h->phase_times()->record_time_secs(G1GCPhaseTimes::GCWorkerEnd, worker_id, Ticks::now().seconds());
  }

public:
  G1EvacuateRegionsTask(G1CollectedHeap* g1h,
                        G1ParScanThreadStateSet* per_thread_states,
                        G1ScannerTasksQueueSet* task_queues,
                        G1RootProcessor* root_processor,
                        uint num_workers,
                        bool has_optional_evacuation_work) :
    G1EvacuateRegionsBaseTask("G1 Evacuate Regions", per_thread_states, task_queues, num_workers),
    _root_processor(root_processor),
    _has_optional_evacuation_work(has_optional_evacuation_work)
  { }
};

void G1YoungCollector::evacuate_initial_collection_set(G1ParScanThreadStateSet* per_thread_states,
                                                      bool has_optional_evacuation_work) {
  G1GCPhaseTimes* p = phase_times();

  {
    log_trace(gc)("DIAG: merge_heap_roots START");
    Ticks start = Ticks::now();
    rem_set()->merge_heap_roots(true /* initial_evacuation */);
    double merge_ms = (Ticks::now() - start).seconds() * 1000.0;
    p->record_merge_heap_roots_time(merge_ms);
    log_trace(gc)("DIAG: merge_heap_roots DONE (%.1fms)", merge_ms);
  }

  Tickspan task_time;
  const uint num_workers = workers()->active_workers();

  Ticks start_processing = Ticks::now();
  {
    G1RootProcessor root_processor(_g1h, num_workers);
    G1EvacuateRegionsTask g1_par_task(_g1h,
                                      per_thread_states,
                                      task_queues(),
                                      &root_processor,
                                      num_workers,
                                      has_optional_evacuation_work);
    log_trace(gc)("DIAG: G1EvacuateRegionsTask START (%u workers)", num_workers);
    task_time = run_task_timed(&g1_par_task);
    log_trace(gc)("DIAG: G1EvacuateRegionsTask DONE (%.1fms)", task_time.seconds() * 1000.0);
    // Closing the inner scope will execute the destructor for the
    // G1RootProcessor object. By subtracting the WorkerThreads task from the total
    // time of this scope, we get the "NMethod List Cleanup" time. This list is
    // constructed during "STW two-phase nmethod root processing", see more in
    // nmethod.hpp
  }
  Tickspan total_processing = Ticks::now() - start_processing;

  p->record_initial_evac_time(task_time.seconds() * 1000.0);
  p->record_or_add_nmethod_list_cleanup_time((total_processing - task_time).seconds() * 1000.0);

  rem_set()->complete_evac_phase(has_optional_evacuation_work);
}

class G1EvacuateOptionalRegionsTask : public G1EvacuateRegionsBaseTask {

  void scan_roots(G1ParScanThreadState* pss, uint worker_id) {
    _g1h->rem_set()->scan_heap_roots(pss, worker_id, G1GCPhaseTimes::OptScanHR, G1GCPhaseTimes::OptObjCopy, true /* remember_already_scanned_cards */);
    _g1h->rem_set()->scan_collection_set_code_roots(pss, worker_id, G1GCPhaseTimes::OptCodeRoots, G1GCPhaseTimes::OptObjCopy);
    _g1h->rem_set()->scan_collection_set_optional_roots(pss, worker_id, G1GCPhaseTimes::OptScanHR, G1GCPhaseTimes::ObjCopy);
  }

  void evacuate_live_objects(G1ParScanThreadState* pss, uint worker_id) {
    G1EvacuateRegionsBaseTask::evacuate_live_objects(pss, worker_id, G1GCPhaseTimes::OptObjCopy, G1GCPhaseTimes::OptTermination);
  }

public:
  G1EvacuateOptionalRegionsTask(G1ParScanThreadStateSet* per_thread_states,
                                G1ScannerTasksQueueSet* queues,
                                uint num_workers) :
    G1EvacuateRegionsBaseTask("G1 Evacuate Optional Regions", per_thread_states, queues, num_workers) {
  }
};

void G1YoungCollector::evacuate_next_optional_regions(G1ParScanThreadStateSet* per_thread_states) {
  // To access the protected constructor/destructor
  class G1MarkScope : public MarkScope { };

  Tickspan task_time;

  Ticks start_processing = Ticks::now();
  {
    G1MarkScope code_mark_scope;
    G1EvacuateOptionalRegionsTask task(per_thread_states, task_queues(), workers()->active_workers());
    task_time = run_task_timed(&task);
    // See comment in evacuate_initial_collection_set() for the reason of the scope.
  }
  Tickspan total_processing = Ticks::now() - start_processing;

  G1GCPhaseTimes* p = phase_times();
  p->record_or_add_nmethod_list_cleanup_time((total_processing - task_time).seconds() * 1000.0);
}

void G1YoungCollector::evacuate_optional_collection_set(G1ParScanThreadStateSet* per_thread_states) {
  const double collection_start_time_ms = phase_times()->cur_collection_start_sec() * 1000.0;

  while (!evacuation_failed() && collection_set()->optional_region_length() > 0) {

    double time_used_ms = os::elapsedTime() * 1000.0 - collection_start_time_ms;
    double time_left_ms = MaxGCPauseMillis - time_used_ms;

    if (time_left_ms < 0 ||
        !collection_set()->finalize_optional_for_evacuation(time_left_ms * policy()->optional_evacuation_fraction())) {
      log_trace(gc, ergo, cset)("Skipping evacuation of %u optional regions, no more regions can be evacuated in %.3fms",
                                collection_set()->optional_region_length(), time_left_ms);
      break;
    }

    {
      Ticks start = Ticks::now();
      rem_set()->merge_heap_roots(false /* initial_evacuation */);
      phase_times()->record_or_add_optional_merge_heap_roots_time((Ticks::now() - start).seconds() * 1000.0);
    }

    {
      Ticks start = Ticks::now();
      evacuate_next_optional_regions(per_thread_states);
      phase_times()->record_or_add_optional_evac_time((Ticks::now() - start).seconds() * 1000.0);
    }

    rem_set()->complete_evac_phase(true /* has_more_than_one_evacuation_phase */);
  }

  collection_set()->abandon_optional_collection_set(per_thread_states);
}

// Non Copying Keep Alive closure
class G1KeepAliveClosure: public OopClosure {
  G1CollectedHeap*_g1h;
public:
  G1KeepAliveClosure(G1CollectedHeap* g1h) :_g1h(g1h) {}
  void do_oop(narrowOop* p) { guarantee(false, "Not needed"); }
  void do_oop(oop* p) {
    // Use g1_resolved_load instead of raw *p to handle tagged oops
    oop obj = g1_resolved_load(p);
    assert(obj != nullptr, "the caller should have filtered out null values");

    const G1HeapRegionAttr region_attr =_g1h->region_attr(obj);
    if (!region_attr.is_in_cset_or_humongous_candidate()) {
      return;
    }
    if (region_attr.is_in_cset()) {
      assert(obj->is_forwarded(), "invariant" );
      *p = obj->forwardee();
    } else {
      assert(!obj->is_forwarded(), "invariant" );
      assert(region_attr.is_humongous_candidate(),
             "Only allowed G1HeapRegionAttr state is IsHumongous, but is %d", region_attr.type());
     _g1h->set_humongous_is_live(obj);
    }
  }
};

// Copying Keep Alive closure - can be called from both
// serial and parallel code as long as different worker
// threads utilize different G1ParScanThreadState instances
// and different queues.
class G1CopyingKeepAliveClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  G1ParScanThreadState*    _par_scan_state;

public:
  G1CopyingKeepAliveClosure(G1CollectedHeap* g1h,
                            G1ParScanThreadState* pss):
    _g1h(g1h),
    _par_scan_state(pss)
  {}

  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(      oop* p) { do_oop_work(p); }

  template <class T> void do_oop_work(T* p) {
    oop obj = g1_resolved_load(p);

    if (_g1h->is_in_cset_or_humongous_candidate(obj)) {
      // If the referent object has been forwarded (either copied
      // to a new location or to itself in the event of an
      // evacuation failure) then we need to update the reference
      // field and, if both reference and referent are in the G1
      // heap, update the RSet for the referent.
      //
      // If the referent has not been forwarded then we have to keep
      // it alive by policy. Therefore we have copy the referent.
      //
      // When the queue is drained (after each phase of reference processing)
      // the object and it's followers will be copied, the reference field set
      // to point to the new location, and the RSet updated.
      _par_scan_state->push_on_queue(ScannerTask(p));
    }
  }
};

class G1STWRefProcProxyTask : public RefProcProxyTask {
  G1CollectedHeap& _g1h;
  G1ParScanThreadStateSet& _pss;
  TaskTerminator _terminator;
  G1ScannerTasksQueueSet& _task_queues;

  // Special closure for enqueuing discovered fields: during enqueue the card table
  // may not be in shape to properly handle normal barrier calls (e.g. card marks
  // in regions that failed evacuation, scribbling of various values by card table
  // scan code). Additionally the regular barrier enqueues into the "global"
  // DCQS, but during GC we need these to-be-refined entries in the GC local queue
  // so that after clearing the card table, the redirty cards phase will properly
  // mark all dirty cards to be picked up by refinement.
  class G1EnqueueDiscoveredFieldClosure : public EnqueueDiscoveredFieldClosure {
    G1CollectedHeap* _g1h;
    G1ParScanThreadState* _pss;

  public:
    G1EnqueueDiscoveredFieldClosure(G1CollectedHeap* g1h, G1ParScanThreadState* pss) : _g1h(g1h), _pss(pss) { }

    void enqueue(HeapWord* discovered_field_addr, oop value) override {
      assert(_g1h->is_in(discovered_field_addr), PTR_FORMAT " is not in heap ", p2i(discovered_field_addr));
      // Store the value first, whatever it is.
      RawAccess<>::oop_store(discovered_field_addr, value);
      if (value == nullptr) {
        return;
      }
      _pss->write_ref_field_post(discovered_field_addr, value);
    }
  };

public:
  G1STWRefProcProxyTask(uint max_workers, G1CollectedHeap& g1h, G1ParScanThreadStateSet& pss, G1ScannerTasksQueueSet& task_queues)
    : RefProcProxyTask("G1STWRefProcProxyTask", max_workers),
      _g1h(g1h),
      _pss(pss),
      _terminator(max_workers, &task_queues),
      _task_queues(task_queues) {}

  void work(uint worker_id) override {
    assert(worker_id < _max_workers, "sanity");
    uint index = (_tm == RefProcThreadModel::Single) ? 0 : worker_id;

    G1ParScanThreadState* pss = _pss.state_for_worker(index);
    pss->set_ref_discoverer(nullptr);

    G1STWIsAliveClosure is_alive(&_g1h);
    G1CopyingKeepAliveClosure keep_alive(&_g1h, pss);
    G1EnqueueDiscoveredFieldClosure enqueue(&_g1h, pss);
    G1ParEvacuateFollowersClosure complete_gc(&_g1h, pss, &_task_queues, _tm == RefProcThreadModel::Single ? nullptr : &_terminator, G1GCPhaseTimes::ObjCopy);
    _rp_task->rp_work(worker_id, &is_alive, &keep_alive, &enqueue, &complete_gc);

    // We have completed copying any necessary live referent objects.
    assert(pss->queue_is_empty(), "both queue and overflow should be empty");
  }

  void prepare_run_task_hook() override {
    _terminator.reset_for_reuse(_queue_count);
  }
};

void G1YoungCollector::process_discovered_references(G1ParScanThreadStateSet* per_thread_states) {
  Ticks start = Ticks::now();

  ReferenceProcessor* rp = ref_processor_stw();
  assert(rp->discovery_enabled(), "should have been enabled");

  uint no_of_gc_workers = workers()->active_workers();
  rp->set_active_mt_degree(no_of_gc_workers);

  G1STWRefProcProxyTask task(rp->max_num_queues(), *_g1h, *per_thread_states, *task_queues());
  ReferenceProcessorPhaseTimes& pt = *phase_times()->ref_phase_times();
  ReferenceProcessorStats stats = rp->process_discovered_references(task, pt);

  gc_tracer_stw()->report_gc_reference_stats(stats);

  _g1h->make_pending_list_reachable();

  phase_times()->record_ref_proc_time((Ticks::now() - start).seconds() * MILLIUNITS);
}

void G1YoungCollector::post_evacuate_cleanup_1(G1ParScanThreadStateSet* per_thread_states) {
  Ticks start = Ticks::now();
  {
    G1PostEvacuateCollectionSetCleanupTask1 cl(per_thread_states, &_evac_failure_regions);
    _g1h->run_batch_task(&cl);
  }
  phase_times()->record_post_evacuate_cleanup_task_1_time((Ticks::now() - start).seconds() * 1000.0);
}

void G1YoungCollector::post_evacuate_cleanup_2(G1ParScanThreadStateSet* per_thread_states,
                                               G1EvacInfo* evacuation_info) {
  Ticks start = Ticks::now();
  {
    G1PostEvacuateCollectionSetCleanupTask2 cl(per_thread_states, evacuation_info, &_evac_failure_regions);
    _g1h->run_batch_task(&cl);
  }
  phase_times()->record_post_evacuate_cleanup_task_2_time((Ticks::now() - start).seconds() * 1000.0);
}

struct RootPinEntry {
  oop*      root;
  uintptr_t obj_addr;
  bool      update_root;
};
static int rpe_cmp(const void* a, const void* b) {
  uintptr_t aa = ((const RootPinEntry*)a)->obj_addr;
  uintptr_t bb = ((const RootPinEntry*)b)->obj_addr;
  return (aa < bb) ? -1 : (aa > bb) ? 1 : 0;
}

static bool remote_eviction_is_block_start(G1CollectedHeap* g1h,
                                           uintptr_t obj_addr,
                                           HeapRegion** region_out,
                                           const char** reason_out) {
  if (region_out != nullptr) {
    *region_out = nullptr;
  }
  if (reason_out != nullptr) {
    *reason_out = nullptr;
  }

  if (obj_addr == 0) {
    if (reason_out != nullptr) *reason_out = "NULL";
    return false;
  }
  if (!is_aligned((address)obj_addr, HeapWordSize)) {
    if (reason_out != nullptr) *reason_out = "UNALIGNED";
    return false;
  }
  if (!g1h->is_in_reserved((void*)obj_addr)) {
    if (reason_out != nullptr) *reason_out = "NOT-IN-HEAP";
    return false;
  }

  HeapRegion* hr = g1h->heap_region_containing_or_null((void*)obj_addr);
  if (hr == nullptr) {
    if (reason_out != nullptr) *reason_out = "NO-REGION";
    return false;
  }
  if (region_out != nullptr) {
    *region_out = hr;
  }
  if (hr->is_free()) {
    if (reason_out != nullptr) *reason_out = "FREE";
    return false;
  }
  if (hr->is_empty()) {
    if (reason_out != nullptr) *reason_out = "EMPTY";
    return false;
  }
  if (hr->is_continues_humongous()) {
    if (reason_out != nullptr) *reason_out = "CONT-HUMONGOUS";
    return false;
  }

  HeapWord* addr = (HeapWord*)obj_addr;
  if (addr < hr->bottom() || addr >= hr->top()) {
    if (reason_out != nullptr) *reason_out = "OUTSIDE-TOP";
    return false;
  }
  // Do not call HeapRegion::block_start() for remote-eviction root
  // validation. The G1 block-start path may parse from a speculative/interior
  // address and dereference payload bits as a Klass before it can report that
  // the address is not a block boundary. Root scanners are supposed to supply
  // real oop starts; validate the header directly and reject implausible roots.
  oop obj = cast_to_oop(addr);
  Klass* k = obj->klass_or_null_acquire();
  if (k == nullptr) {
    if (reason_out != nullptr) *reason_out = "NULL-KLASS";
    return false;
  }
  if (!Klass::is_valid(k)) {
    if (reason_out != nullptr) *reason_out = "BAD-KLASS";
    return false;
  }
  return true;
}

static bool remote_eviction_is_filler_klass(Klass* k) {
  return k == Universe::fillerArrayKlassObj() ||
         k == vmClasses::FillerObject_klass();
}

static bool remote_eviction_is_relocatable_object(G1CollectedHeap* g1h,
                                                  uintptr_t obj_addr,
                                                  HeapRegion** region_out,
                                                  size_t* word_size_out,
                                                  const char** reason_out) {
  HeapRegion* hr = nullptr;
  if (!remote_eviction_is_block_start(g1h, obj_addr, &hr, reason_out)) {
    if (region_out != nullptr) *region_out = hr;
    return false;
  }

  oop obj = cast_to_oop(obj_addr);
  Klass* k = obj->klass_or_null_acquire();
  if (k == nullptr) {
    if (reason_out != nullptr) *reason_out = "NULL-KLASS";
    if (region_out != nullptr) *region_out = hr;
    return false;
  }
  if (!Klass::is_valid(k)) {
    if (reason_out != nullptr) *reason_out = "BAD-KLASS";
    if (region_out != nullptr) *region_out = hr;
    return false;
  }
  if (remote_eviction_is_filler_klass(k)) {
    if (reason_out != nullptr) *reason_out = "FILLER";
    if (region_out != nullptr) *region_out = hr;
    return false;
  }

  Klass* size_k = obj->klass_or_null_acquire();
  if (size_k == nullptr) {
    if (reason_out != nullptr) *reason_out = "NULL-KLASS-SIZE";
    if (region_out != nullptr) *region_out = hr;
    return false;
  }
  if (size_k != k) {
    if (reason_out != nullptr) *reason_out = "KLASS-CHANGED";
    if (region_out != nullptr) *region_out = hr;
    return false;
  }

  size_t word_size = obj->size_given_klass(size_k);
  if (word_size == 0 || (HeapWord*)obj_addr + word_size > hr->top()) {
    if (reason_out != nullptr) *reason_out = "BAD-SIZE";
    if (region_out != nullptr) *region_out = hr;
    return false;
  }

  if (region_out != nullptr) *region_out = hr;
  if (word_size_out != nullptr) *word_size_out = word_size;
  return true;
}

static void dirty_root_catch_region(G1CollectedHeap* g1h, HeapRegion* root_catch, HeapWord* top) {
  if (root_catch == nullptr || top == root_catch->bottom()) {
    return;
  }

  root_catch->set_top(top);

  G1CardTable* ct = g1h->card_table();
  CardTable::CardValue* start_card = ct->byte_for(root_catch->bottom());
  CardTable::CardValue* end_card   = ct->byte_for(top - 1) + 1;
  memset(start_card, CardTable::dirty_card_val(), end_card - start_card);

  // Raw card table dirtying alone is invisible to G1's remset processing.
  G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
  G1DirtyCardQueue tmp_queue(&dcqs);
  for (CardTable::CardValue* card = start_card; card < end_card; card++) {
    dcqs.enqueue(tmp_queue, card);
  }
  dcqs.flush_queue(tmp_queue);
}

static int guard_eviction_candidates_with_raw_stack(G1CollectedHeap* g1h,
                                                    bool* eviction_candidates,
                                                    uint num_regions,
                                                    const char* log_prefix) {
  class RawStackGuardClosure : public ThreadClosure {
    G1CollectedHeap* _g1h;
    bool*            _eviction_candidates;
    uint             _num_regions;
    int              _regions_guarded;
    int              _stack_words_found;
    int              _threads_scanned;
    size_t           _words_scanned;
    int              _reports_left;
    const char*      _log_prefix;

    bool maybe_guard_candidate(uintptr_t raw, Thread* thread, uintptr_t* slot) {
      if (raw == 0) return false;

      // Shared-handle references are already safe: they do not retain a
      // raw local object address into the region being evicted.
      if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
          (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
        return false;
      }

      uintptr_t addr = raw;
      if ((raw & G1_OOP_TAG_MASK) != 0) {
        addr = raw & G1_OOP_ADDR_MASK;
      }
      if (!is_aligned((address)addr, HeapWordSize)) return false;
      if (!_g1h->is_in_reserved((void*)addr)) return false;

      HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)addr);
      if (hr == nullptr) return false;

      uint idx = hr->hrm_index();
      if (idx >= _num_regions) return false;

      if (_eviction_candidates[idx]) {
        _eviction_candidates[idx] = false;
        hr->clear_cold_destination();
        _regions_guarded++;
        _stack_words_found++;
        if (_reports_left > 0) {
          log_warning(gc)("%s: thread=" PTR_FORMAT " slot=" PTR_FORMAT
                          " raw=" PTR_FORMAT " guards candidate region %u",
                          _log_prefix, p2i(thread), p2i(slot), raw, idx);
          _reports_left--;
        }
        return true;
      }

      _stack_words_found++;
      return false;
    }

  public:
    RawStackGuardClosure(G1CollectedHeap* g1h, bool* candidates,
                         uint num_regions, const char* log_prefix)
      : _g1h(g1h), _eviction_candidates(candidates), _num_regions(num_regions),
        _regions_guarded(0), _stack_words_found(0), _threads_scanned(0),
        _words_scanned(0), _reports_left(32), _log_prefix(log_prefix) {}

    void do_thread(Thread* thread) override {
      JavaThread* jt = JavaThread::cast(thread);
      if (!jt->has_last_Java_frame()) return;

      address java_sp = (address)jt->last_Java_sp();
      address high = jt->stack_base();
      if (java_sp == nullptr || high == nullptr || java_sp >= high) return;

      // Compiled/interpreter execution can leave raw clean oop values in
      // stack slots that are not described as roots at the safepoint.  A
      // bounded SP window missed long-lived task locals in Spark, so scan
      // the full usable Java stack conservatively.  Stay above the low
      // stack guard pages.
      address stack_low = jt->stack_overflow_state()->stack_reserved_zone_base();
      address low = MIN2(java_sp, stack_low);

      uintptr_t* cur = (uintptr_t*)align_up(low, sizeof(uintptr_t));
      uintptr_t* end = (uintptr_t*)align_down(high, sizeof(uintptr_t));
      _threads_scanned++;
      while (cur < end) {
        maybe_guard_candidate(*cur, thread, cur);
        cur++;
        _words_scanned++;
      }
    }

    int regions_guarded() const { return _regions_guarded; }
    int stack_words_found() const { return _stack_words_found; }
    int threads_scanned() const { return _threads_scanned; }
    size_t words_scanned() const { return _words_scanned; }
  };

  RawStackGuardClosure raw_stack_cl(g1h, eviction_candidates, num_regions, log_prefix);
  Threads::java_threads_do(&raw_stack_cl);
  if (raw_stack_cl.regions_guarded() > 0) {
    log_info(gc)("%s: removed %d candidate regions with %d stack words "
                 "(scanned " SIZE_FORMAT " words in %d Java threads)",
                 log_prefix, raw_stack_cl.regions_guarded(),
                 raw_stack_cl.stack_words_found(),
                 raw_stack_cl.words_scanned(), raw_stack_cl.threads_scanned());
  } else {
    log_info(gc)("%s: scanned " SIZE_FORMAT
                 " words in %d Java threads, 0 candidate stack words",
                 log_prefix, raw_stack_cl.words_scanned(),
                 raw_stack_cl.threads_scanned());
  }

  return raw_stack_cl.regions_guarded();
}

// Closure that checks if a root oop targets a cold-destination region.
// Used by both Path 1 (root-pinning) and Path 2 (existing region pinning).
class ColdRegionPinClosure : public OopClosure {
  G1CollectedHeap* _g1h;
public:
  ColdRegionPinClosure(G1CollectedHeap* g1h) : _g1h(g1h) {}
  void do_oop(oop* p) {
    oop obj = *p;
    if (obj == nullptr) return;
    uintptr_t raw = cast_from_oop<uintptr_t>(obj);
    if ((raw & G1_OOP_TAG_MASK) != 0) {
      obj = cast_to_oop(raw & G1_OOP_ADDR_MASK);
    }
    if (!_g1h->is_in(obj)) return;
    HeapRegion* hr = _g1h->heap_region_containing(obj);
    if (hr != nullptr && hr->is_cold_destination() && !hr->is_root_pinned()) {
      hr->set_root_pinned();
      Klass* k = obj->klass_or_null();
      log_info(gc)("Root-pinned region %u (root " PTR_FORMAT " -> obj " PTR_FORMAT " klass=%s)",
                    hr->hrm_index(), p2i(p), p2i((void*)obj),
                    (k != nullptr ? k->external_name() : "<null klass>"));
    }
  }
  void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }
};

struct RegionColdnessSample {
  size_t sampled_words;
  size_t cold_words;
  size_t hot_words;
  size_t unknown_words;
  size_t object_count;
  size_t object_words;
  bool dense_small_objects;

  RegionColdnessSample() :
    sampled_words(0),
    cold_words(0),
    hot_words(0),
    unknown_words(0),
    object_count(0),
    object_words(0),
    dense_small_objects(false) {}
};

static bool region_is_cold_by_epoch(HeapRegion* hr,
                                    G1RemoteMemoryManager* rmm,
                                    bool guard_dense_small_objects,
                                    RegionColdnessSample* sample) {
  const uintptr_t cold_distance = 4;
  const uintptr_t gc_epoch = rmm->gc_epoch();
  const size_t min_avg_object_words = 512 / HeapWordSize;
  const size_t dense_probe_object_count = 4096;
  const size_t min_dense_object_count = MAX2((size_t)4096, HeapRegion::GrainBytes / 1024);
  size_t sampled_words = 0;
  size_t cold_words = 0;
  size_t hot_words = 0;
  size_t unknown_words = 0;
  size_t object_count = 0;
  size_t object_words = 0;
  size_t obj_index = 0;

  HeapWord* p = hr->bottom();
  HeapWord* top = hr->top();
  while (p < top) {
    oop obj = cast_to_oop(p);
    size_t word_size = 0;
    if (obj->is_forwarded()) {
      word_size = obj->size_given_klass(obj->klass());
    } else {
      word_size = obj->size();
    }
    if (word_size == 0) {
      break;
    }
    object_count++;
    object_words += word_size;

    if (object_count >= dense_probe_object_count &&
        object_words < object_count * min_avg_object_words) {
      sample->sampled_words = sampled_words;
      sample->cold_words = cold_words;
      sample->hot_words = hot_words;
      sample->unknown_words = unknown_words;
      sample->object_count = object_count;
      sample->object_words = object_words;
      sample->dense_small_objects = true;
      return false;
    }

    // Sample every 8th object to bound mark-word work while still scanning
    // object sizes correctly across the region.
    if ((obj_index++ & 0x7) == 0) {
      markWord mw = obj->mark();
      sampled_words += word_size;
      if (!mw.is_unlocked() || mw.remote_epoch() == 0) {
        unknown_words += word_size;
      } else if (mw.hotness_distance(gc_epoch) >= cold_distance) {
        cold_words += word_size;
      } else {
        hot_words += word_size;
      }
    }
    p += word_size;
  }

  sample->sampled_words = sampled_words;
  sample->cold_words = cold_words;
  sample->hot_words = hot_words;
  sample->unknown_words = unknown_words;
  sample->object_count = object_count;
  sample->object_words = object_words;

  if (object_count > 0) {
    // Object-granularity RDMA fetch makes densely packed tiny-object regions
    // extremely expensive to fault back in. Keep them local during proactive
    // T1 eviction; higher pressure tiers may still fall back to them.
    if (object_count >= min_dense_object_count &&
        object_words < object_count * min_avg_object_words) {
      sample->dense_small_objects = true;
      if (guard_dense_small_objects) {
        return false;
      }
    }
  }

  size_t known_words = cold_words + hot_words;
  if (sampled_words == 0 || known_words == 0) {
    return false;
  }

  // Require a minimum amount of stamped hotness signal, then accept regions
  // where the known sample is mostly cold and the total sample is not hot.
  bool enough_signal = known_words * 100 >= sampled_words * 10;
  bool mostly_cold = cold_words * 100 >= known_words * 75;
  bool not_hot = hot_words * 100 <= sampled_words * 25;
  return enough_signal && mostly_cold && not_hot;
}

#ifdef LINUX
static bool read_jlong_from_file(const char* path, jlong* value) {
  FILE* fp = fopen(path, "r");
  if (fp == nullptr) {
    return false;
  }

  char buf[64];
  bool ok = false;
  if (fgets(buf, sizeof(buf), fp) != nullptr) {
    errno = 0;
    char* end = nullptr;
    jlong parsed = strtoll(buf, &end, 10);
    if (errno == 0 && end != buf && parsed > 0) {
      *value = parsed;
      ok = true;
    }
  }
  fclose(fp);
  return ok;
}

static bool cgroup_controller_matches(const char* controllers, const char* name) {
  size_t name_len = strlen(name);
  const char* cursor = controllers;
  while (*cursor != '\0') {
    const char* comma = strchr(cursor, ',');
    size_t token_len = comma == nullptr ? strlen(cursor) : (size_t)(comma - cursor);
    if (token_len == name_len && strncmp(cursor, name, name_len) == 0) {
      return true;
    }
    if (comma == nullptr) {
      return false;
    }
    cursor = comma + 1;
  }
  return false;
}

static bool build_cgroup_file_path(char* out,
                                   size_t out_len,
                                   const char* root,
                                   const char* cgroup_path,
                                   const char* file_name) {
  if (cgroup_path == nullptr || cgroup_path[0] == '\0' ||
      strcmp(cgroup_path, "/") == 0) {
    return false;
  }

  int written;
  if (cgroup_path[0] == '/') {
    written = snprintf(out, out_len, "%s%s/%s", root, cgroup_path, file_name);
  } else {
    written = snprintf(out, out_len, "%s/%s/%s", root, cgroup_path, file_name);
  }
  return written > 0 && (size_t)written < out_len;
}

static bool init_remote_cgroup_memory_paths(char* usage_path,
                                            size_t usage_path_len,
                                            char* limit_path,
                                            size_t limit_path_len) {
  FILE* fp = fopen("/proc/self/cgroup", "r");
  if (fp == nullptr) {
    return false;
  }

  char line[1024];
  bool found = false;
  while (fgets(line, sizeof(line), fp) != nullptr && !found) {
    char* first_colon = strchr(line, ':');
    if (first_colon == nullptr) {
      continue;
    }
    char* second_colon = strchr(first_colon + 1, ':');
    if (second_colon == nullptr) {
      continue;
    }

    *first_colon = '\0';
    *second_colon = '\0';
    const char* hierarchy = line;
    const char* controllers = first_colon + 1;
    char* cgroup_path = second_colon + 1;
    cgroup_path[strcspn(cgroup_path, "\n")] = '\0';

    if (cgroup_controller_matches(controllers, "memory")) {
      found =
        build_cgroup_file_path(usage_path, usage_path_len,
                               "/sys/fs/cgroup/memory", cgroup_path,
                               "memory.usage_in_bytes") &&
        build_cgroup_file_path(limit_path, limit_path_len,
                               "/sys/fs/cgroup/memory", cgroup_path,
                               "memory.limit_in_bytes");
    } else if (strcmp(hierarchy, "0") == 0 && controllers[0] == '\0') {
      found =
        build_cgroup_file_path(usage_path, usage_path_len,
                               "/sys/fs/cgroup", cgroup_path,
                               "memory.current") &&
        build_cgroup_file_path(limit_path, limit_path_len,
                               "/sys/fs/cgroup", cgroup_path,
                               "memory.max");
    }
  }

  fclose(fp);
  return found;
}

static bool read_remote_cgroup_files(jlong* raw_usage, jlong* raw_limit) {
  static bool paths_initialized = false;
  static char usage_path[1024];
  static char limit_path[1024];

  if (!paths_initialized) {
    paths_initialized =
      init_remote_cgroup_memory_paths(usage_path, sizeof(usage_path),
                                      limit_path, sizeof(limit_path));
    if (!paths_initialized) {
      // The Spark executor is moved into the cgexec memory cgroup shortly after
      // launch. Retry later instead of caching a startup-time root cgroup miss.
      return false;
    }
  }

  return read_jlong_from_file(usage_path, raw_usage) &&
         read_jlong_from_file(limit_path, raw_limit);
}
#endif

static bool read_remote_cgroup_pressure(size_t local_capacity,
                                        size_t* usage,
                                        size_t* capacity) {
#ifdef LINUX
  if (!G1RemoteUseCgroupPressure) {
    return false;
  }

  jlong raw_usage = 0;
  jlong raw_limit = 0;
  if (!read_remote_cgroup_files(&raw_usage, &raw_limit)) {
    return false;
  }
  if (raw_usage <= 0 || raw_limit <= 0 || local_capacity == 0) {
    return false;
  }

  size_t pressure_capacity = MIN2(local_capacity, (size_t)raw_limit);
  if (pressure_capacity == 0) {
    return false;
  }

  *usage = (size_t)raw_usage;
  *capacity = pressure_capacity;
  return true;
#else
  return false;
#endif
}

static void flush_free_region_trim(G1CollectedHeap* g1h,
                                   uint& run_start_idx,
                                   char*& run_start,
                                   char*& run_end,
                                   uint& run_region_count,
                                   uint& trimmed_regions,
                                   uint& failed_regions,
                                   uint& trimmed_ranges,
                                   uint& failed_ranges,
                                   size_t& trimmed_bytes) {
  if (run_start == nullptr) {
    return;
  }

  size_t run_bytes = (size_t)(run_end - run_start);
  if (::madvise(run_start, run_bytes, MADV_DONTNEED) == 0) {
    for (uint i = 0; i < run_region_count; i++) {
      g1h->region_at(run_start_idx + i)->set_rss_trimmed_free();
    }
    trimmed_regions += run_region_count;
    trimmed_ranges++;
    trimmed_bytes += run_bytes;
  } else {
    failed_regions += run_region_count;
    failed_ranges++;
  }

  run_start_idx = 0;
  run_start = nullptr;
  run_end = nullptr;
  run_region_count = 0;
}

static void trim_free_region_rss(G1CollectedHeap* g1h) {
  Ticks trim_start = Ticks::now();
  uint trimmed_regions = 0;
  uint failed_regions = 0;
  uint trimmed_ranges = 0;
  uint failed_ranges = 0;
  size_t trimmed_bytes = 0;
  uint num_regions = g1h->num_regions();
  uint run_start_idx = 0;
  char* run_start = nullptr;
  char* run_end = nullptr;
  uint run_region_count = 0;

  for (uint i = 0; i < num_regions; i++) {
    HeapRegion* hr = g1h->region_at(i);
    if (hr == nullptr || !hr->is_free() || hr->is_evict_guarded() ||
        hr->is_rss_trimmed_free()) {
      flush_free_region_trim(g1h, run_start_idx, run_start, run_end, run_region_count,
                             trimmed_regions, failed_regions,
                             trimmed_ranges, failed_ranges,
                             trimmed_bytes);
      continue;
    }

    char* bottom = (char*)hr->bottom();
    char* end = (char*)hr->end();
    if (run_start == nullptr) {
      run_start_idx = i;
      run_start = bottom;
      run_end = end;
      run_region_count = 1;
    } else if (bottom == run_end) {
      run_end = end;
      run_region_count++;
    } else {
      flush_free_region_trim(g1h, run_start_idx, run_start, run_end, run_region_count,
                             trimmed_regions, failed_regions,
                             trimmed_ranges, failed_ranges,
                             trimmed_bytes);
      run_start_idx = i;
      run_start = bottom;
      run_end = end;
      run_region_count = 1;
    }
  }
  flush_free_region_trim(g1h, run_start_idx, run_start, run_end, run_region_count,
                         trimmed_regions, failed_regions,
                         trimmed_ranges, failed_ranges,
                         trimmed_bytes);

  if (trimmed_regions > 0 || failed_regions > 0) {
    double trim_ms = (Ticks::now() - trim_start).seconds() * 1000.0;
    if (failed_regions > 0) {
      log_info(gc)("Remote RSS trim: madvised %u free regions (" SIZE_FORMAT
                   "MB, %u ranges) in %.1fms, failures=%u regions/%u ranges",
                   trimmed_regions, trimmed_bytes / M, trimmed_ranges, trim_ms,
                   failed_regions, failed_ranges);
    } else {
      log_info(gc)("Remote RSS trim: madvised %u free regions (" SIZE_FORMAT
                   "MB, %u ranges) in %.1fms",
                   trimmed_regions, trimmed_bytes / M, trimmed_ranges, trim_ms);
    }
  }
}

static bool has_remote_heap_activity(G1RemoteMemoryManager* rmm) {
  if (rmm == nullptr) {
    return false;
  }
  G1RemoteBackend* backend = rmm->backend();
  return rmm->tagged_field_count() > 0 ||
         rmm->remote_roots_count() > 0 ||
         (backend != nullptr &&
          (backend->total_evicted() > 0 || backend->total_fetched() > 0));
}

void G1YoungCollector::post_evacuate_collection_set(G1EvacInfo* evacuation_info,
                                                    G1ParScanThreadStateSet* per_thread_states) {
  G1GCPhaseTimes* p = phase_times();

  // Capture heap usage before cleanup for legacy eviction threshold mode.
  // LocalMemoryRatio mode uses post-cleanup live usage below: reclaimed young
  // pages are madvised before returning to the mutator and should not force
  // dense old-region eviction.
  const size_t pre_cleanup_heap_used = _g1h->used();

  // Process any discovered reference objects - we have
  // to do this _before_ we retire the GC alloc regions
  // as we may have to copy some 'reachable' referent
  // objects (and their reachable sub-graphs) that were
  // not copied during the pause.
  log_trace(gc)(">>>   process_discovered_references START");
  process_discovered_references(per_thread_states);
  log_trace(gc)(">>>   process_discovered_references DONE");

  // Fixup tagged field Handles: update any LOCAL Handle whose target was
  // forwarded during evacuation. The root scan in G1EvacuateRegionsTask
  // evacuates Handle targets in the cset; this pass catches any that were
  // forwarded by other closures (e.g., through remset scanning).
  {
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    if (rmm != nullptr) {
      const bool remote_activity = has_remote_heap_activity(rmm);
      const int tagged_count = rmm->tagged_field_count();
      Ticks fixup_start = Ticks::now();
      int tagged_updated = 0;
      int local_updated = 0;
      if (remote_activity || tagged_count > 0) {
        log_info(gc)("Remote handle post-evac fixup START (tagged_entries=%d)",
                     tagged_count);
        if (tagged_count > 0) {
          tagged_updated = rmm->fixup_tagged_field_handles();
        }
        local_updated = rmm->fixup_all_local_handles();
        double fixup_ms = (Ticks::now() - fixup_start).seconds() * 1000.0;
        log_info(gc)("Remote handle post-evac fixup DONE: %.1fms "
                     "(tagged_updated=%d local_updated=%d tagged_remaining=%d)",
                     fixup_ms, tagged_updated, local_updated,
                     rmm->tagged_field_count());
      } else {
        log_debug(gc)("Remote handle post-evac fixup SKIP: no remote heap activity");
      }
    }
  }

  G1STWIsAliveClosure is_alive(_g1h);
  G1KeepAliveClosure keep_alive(_g1h);

  log_trace(gc)(">>>   WeakProcessor START");
  WeakProcessor::weak_oops_do(workers(), &is_alive, &keep_alive, p->weak_phase_times());
  log_trace(gc)(">>>   WeakProcessor DONE");

  allocator()->release_gc_alloc_regions(evacuation_info);

  // Step 2: Root-pinning pass for cold-destination regions.
  // After alloc regions are released (cold_destination flag is set),
  // scan roots to detect if any cold region has root-referenced objects.
  // Root-pinned regions cannot be evicted (root refs bypass load barrier).
  if (G1RemoteEvictionThreshold > 0) {
    ColdRegionPinClosure pin_cl(_g1h);
    // Scan ALL root sources (not just threads+JNI).
    // Missing any root type leaves clean oops to filler → crash.
    Threads::oops_do(&pin_cl, nullptr);
    JNIHandles::oops_do(&pin_cl);
    OopStorageSet::strong_oops_do(&pin_cl);
    // Weak OopStorages: StringTable, ResolvedMethodTable, etc.
    // These hold direct oops (not tagged) that bypass the load barrier.
    for (auto id : EnumRange<OopStorageSet::WeakId>()) {
      OopStorageSet::storage(id)->oops_do(&pin_cl);
    }
    // ClassLoaderDataGraph — java.lang.Class mirrors and CLD oops
    {
      CLDToOopClosure cld_cl(&pin_cl, ClassLoaderData::_claim_none);
      ClassLoaderDataGraph::cld_do(&cld_cl);
    }
    // CodeCache — embedded oop constants in compiled methods
    {
      CodeBlobToOopClosure code_cl(&pin_cl, false);
      CodeCache::blobs_do(&code_cl);
    }
    // Concurrent mark ref processor — discovered references
    _g1h->ref_processor_cm()->weak_oops_do(&pin_cl);

    // Also check remote anchor roots + cross-boundary roots
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    if (rmm != nullptr) {
      rmm->oops_do_remote_anchors(&pin_cl);
      rmm->oops_do_remote_cross_roots(&pin_cl);
    }
  }

  log_trace(gc)(">>>   post_evacuate_cleanup_1 START");
  if (G1TagRefSites || G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0 || LocalMemoryRatio < 100) {
    log_debug(gc)("Remote post-evac cleanup1 START");
  }
  post_evacuate_cleanup_1(per_thread_states);
  if (G1TagRefSites || G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0 || LocalMemoryRatio < 100) {
    log_debug(gc)("Remote post-evac cleanup1 DONE");
  }
  log_trace(gc)(">>>   post_evacuate_cleanup_1 DONE");

  if (G1TagRefSites || G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0 || LocalMemoryRatio < 100) {
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    if (rmm != nullptr) {
      static uint old_cset_fixup_gc = 0;
      static uint old_cset_clean_streak = 0;
      static uint old_cset_last_old_regions = 0;

      uint old_regions = _g1h->old_regions_count();
      bool remote_activity = has_remote_heap_activity(rmm);
      bool warmup = old_cset_fixup_gc < 30;
      bool old_growth = old_regions >= old_cset_last_old_regions + 8;
      bool periodic = (old_cset_fixup_gc % 8) == 0;
      bool clean_probe = old_cset_clean_streak < 2;

      if (remote_activity && (warmup || old_growth || periodic || clean_probe)) {
        int repaired = rmm->fixup_stale_refs_in_old_regions(evacuation_failed());
        old_cset_clean_streak = (repaired == 0) ? old_cset_clean_streak + 1 : 0;
        old_cset_last_old_regions = old_regions;
      } else {
        log_debug(gc)("Old/cset fixup SKIP: no remote activity, clean_streak=%u, "
                      "old_regions=%u last_scan_old_regions=%u",
                      old_cset_clean_streak, old_regions, old_cset_last_old_regions);
      }
      old_cset_fixup_gc++;
    }
  }

  log_trace(gc)(">>>   post_evacuate_cleanup_2 START");
  if (G1TagRefSites || G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0 || LocalMemoryRatio < 100) {
    log_debug(gc)("Remote post-evac cleanup2 START");
  }
  post_evacuate_cleanup_2(per_thread_states, evacuation_info);
  if (G1TagRefSites || G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0 || LocalMemoryRatio < 100) {
    log_debug(gc)("Remote post-evac cleanup2 DONE");
  }
  log_trace(gc)(">>>   post_evacuate_cleanup_2 DONE");

  if (G1TagRefSites || G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0 || LocalMemoryRatio < 100) {
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    if (rmm != nullptr && has_remote_heap_activity(rmm)) {
      rmm->purge_stale_local_handles("POST-FREE-HANDLE-SWEEP", 16);
    }
  }

  _evac_failure_regions.post_collection();

  assert_used_and_recalculate_used_equal(_g1h);

  // Early stale-ref sweep: run at every GC for first 20 cycles to catch first occurrence
  {
    static int _gc_count_for_sweep = 0;
    G1RemoteMemoryManager* rmm_early = _g1h->remote_memory_manager();
    if (G1VerifyAfterEviction && rmm_early != nullptr &&
        has_remote_heap_activity(rmm_early) && _gc_count_for_sweep < 20) {
      int stale_early = rmm_early->verify_no_stale_refs_to_freed_regions();
      if (stale_early > 0) {
        log_warning(gc)("EARLY-SWEEP GC#%d: %d stale refs found!", _gc_count_for_sweep, stale_early);
      } else {
        log_debug(gc)("EARLY-SWEEP GC#%d: clean", _gc_count_for_sweep);
      }
      _gc_count_for_sweep++;
    }
  }

  // Remote collection: free dead remote objects without fetching.
  // "Garbage never crosses the network" — dispatches to the selected backend
  // (sim, TCP executor, or RDMA executor) via G1RemoteMemoryManager.
  {
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    if (rmm != nullptr && has_remote_heap_activity(rmm)) {
      log_trace(gc)(">>>   collect_dead_remote_objects START");
      rmm->collect_dead_remote_objects();
      log_trace(gc)(">>>   collect_dead_remote_objects DONE");
    }
  }

  // ============================================================
  // Remote Memory Eviction — full heap scan for barrier coverage
  // ============================================================
  // Restructured: collect ALL candidates first, then one full heap scan
  // to tag every reference to eviction targets. This catches refs that
  // remset-only tagging misses (dirty cards not refined, post-evacuation
  // card dirtying, cross-region refs from non-collected regions).
  if (G1RemoteEvictionThreshold > 0 || G1SimulateRemoteEviction || LocalMemoryRatio < 100) {
    guarantee(!UseCompressedOops,
              "Remote eviction requires -XX:-UseCompressedOops "
              "(tagged oops need 64-bit pointers)");
    // Skip eviction during concurrent start (initial mark) GC.
    // Old/survivor alloc regions retired during concurrent start are registered
    // as concurrent mark root regions. Quarantining them causes SIGSEGV when
    // the concurrent mark thread later scans the poisoned root region data.
    if (collector_state()->in_concurrent_start_gc()) {
      log_info(gc)("Skipping eviction during concurrent start GC (root region conflict)");
    } else {
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    RemoteHandleAllocBuffer hab;
    int total_evicted = 0;
    int regions_evicted = 0;
    int regions_pinned = 0;
    int regions_kept_alive = 0;
    uint freed_regions = 0;
    size_t total_freed_bytes = 0;
    FreeRegionList freed_list("Evicted Cold Regions");
    uint num_regions = _g1h->num_regions();

    // ---- Phase A: Collect eviction candidates ----
    bool* eviction_candidates = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
    memset(eviction_candidates, 0, num_regions * sizeof(bool));
    int path1_candidates = 0;
    int path2_candidates = 0;

    // Path 1: cold-destination regions from this evacuation (root-pinned already filtered)
    for (uint i = 0; i < num_regions; i++) {
      HeapRegion* hr = _g1h->region_at(i);
      if (!hr->is_cold_destination()) continue;
      if (hr->is_free() || hr->is_empty()) {
        hr->clear_cold_destination();
        hr->clear_root_pinned();
        continue;
      }
      if (hr->is_fetch_cache()) continue;
      if (hr->is_root_pinned()) {
        hr->clear_cold_destination();
        hr->clear_root_pinned();
        regions_pinned++;
        log_debug(gc)("Cold region %u pinned by roots — kept local", hr->hrm_index());
        continue;
      }
      eviction_candidates[i] = true;
      path1_candidates++;
    }

    // Path 2: tiered eviction based on local memory pressure
    //
    // When LocalMemoryRatio < 100, the real deadline is the process/cgroup
    // local capacity, not Xmx. Reserve headroom for native/RDMA/Spark memory
    // so heap growth does not reach the cgroup limit before eviction fires.
    // Two active tiers based on heap budget pressure (with allocation rate
    // lookahead). The old proactive T1 path did full object hotness/classify
    // work for Spark NB but selected zero regions because all candidates were
    // dense tiny-object regions. Keep object-granularity eviction dormant until
    // pressure is high enough to justify its cost.
    //   Tier 2 (>85%): aggressive - evict old regions to 70% target
    //   Tier 3 (>95%): emergency - evict old regions to 60% target
    //
    // Falls back to G1RemoteEvictionThreshold if LocalMemoryRatio == 100.
    {
      size_t heap_capacity = _g1h->max_capacity();
      size_t local_used = pre_cleanup_heap_used;
      int eviction_tier = 0;
      size_t evict_target_bytes = 0;
      size_t evict_batch_cap_bytes = 0;

      if (LocalMemoryRatio < 100 && LocalMemoryRatio > 0) {
        local_used = _g1h->used();
        size_t local_capacity = (heap_capacity * LocalMemoryRatio) / 100;
        // RDMA mode keeps large native side metadata (handles, edge tables,
        // tagged-field lists, staging buffers, Spark/JVM native state). In the
        // Spark lr=25 run, a 2G reserve still let the process reach the 5G
        // memcg limit while the local heap was just under 4G, so keep half of
        // the local capacity as non-Java-heap headroom.
        size_t native_reserve = MAX2(local_capacity / 2, (size_t)2 * G);
        native_reserve = MIN2(native_reserve, (local_capacity * 3) / 5);
        size_t heap_budget = local_capacity - native_reserve;
        size_t cgroup_usage = 0;
        size_t cgroup_capacity = 0;
        bool has_cgroup_pressure =
          read_remote_cgroup_pressure(local_capacity, &cgroup_usage, &cgroup_capacity);

        // Allocation rate lookahead: predict bytes allocated before next GC.
        // predict_alloc_rate_ms() returns bytes/ms; multiply by 2000ms lookahead.
        double alloc_rate_ms = _g1h->policy()->analytics()->predict_alloc_rate_ms();
        size_t lookahead_alloc = (size_t)(alloc_rate_ms * 2000.0);
        size_t effective_used = local_used + lookahead_alloc;

        double heap_pressure = (double)effective_used / (double)heap_budget;
        double cgroup_pressure = has_cgroup_pressure ?
          (double)cgroup_usage / (double)cgroup_capacity : 0.0;
        double pressure = MAX2(heap_pressure, cgroup_pressure);
        // Do not evict at low pressure: object-granularity fetch is expensive,
        // and early eviction refetches hot Spark partitions while plenty of the
        // local budget is still unused. Keep headroom for the next young cycle,
        // but avoid pushing the heap down to an artificially low watermark.
        size_t target_low_percent = 0;
        uint tier2_percent = G1RemoteTier2Percent;
        uint tier3_percent = MAX2(G1RemoteTier3Percent, tier2_percent);

        if (pressure * 100.0 > (double)tier3_percent) {
          eviction_tier = 3;
          target_low_percent = 60;
        } else if (pressure * 100.0 > (double)tier2_percent) {
          eviction_tier = 2;
          target_low_percent = 70;
        }

        if (eviction_tier > 0) {
          size_t target_low = (heap_budget * target_low_percent) / 100;
          evict_target_bytes = (local_used > target_low) ? (local_used - target_low) : 0;
          if (has_cgroup_pressure) {
            size_t cgroup_target_low = (cgroup_capacity * target_low_percent) / 100;
            size_t cgroup_evict_target =
              (cgroup_usage > cgroup_target_low) ? (cgroup_usage - cgroup_target_low) : 0;
            evict_target_bytes = MAX2(evict_target_bytes, cgroup_evict_target);
          }
          if (eviction_tier == 2) {
            evict_batch_cap_bytes = HeapRegion::GrainBytes * 8;
            evict_target_bytes = MIN2(evict_target_bytes, evict_batch_cap_bytes);
          } else if (eviction_tier == 3) {
            evict_batch_cap_bytes = HeapRegion::GrainBytes * 16;
            evict_target_bytes = MIN2(evict_target_bytes, evict_batch_cap_bytes);
          }
        }

        if (eviction_tier > 0) {
          log_info(gc)("Tiered eviction T%d: local_used=" SIZE_FORMAT "MB / heap_budget=" SIZE_FORMAT
                       "MB (local_cap=" SIZE_FORMAT "MB reserve=" SIZE_FORMAT "MB, %.1f%%), "
                       "alloc_rate=%.1fKB/ms, lookahead=" SIZE_FORMAT "MB, heap_effective=%.1f%%, "
                       "cgroup=" SIZE_FORMAT "MB/" SIZE_FORMAT "MB %.1f%%, thresholds=T2:%u%%/T3:%u%%, "
                       "effective=%.1f%%, target=%zu%%, batch_cap=" SIZE_FORMAT
                       "MB, evict_target=" SIZE_FORMAT "MB, dense_last_resort=%s",
                       eviction_tier, local_used / M, heap_budget / M,
                       local_capacity / M, native_reserve / M, heap_pressure * 100.0,
                       alloc_rate_ms / 1024.0,
                       lookahead_alloc / M,
                       heap_pressure * 100.0,
                       cgroup_usage / M, cgroup_capacity / M, cgroup_pressure * 100.0,
                       tier2_percent, tier3_percent,
                       pressure * 100.0,
                       target_low_percent,
                       evict_batch_cap_bytes / M,
                       evict_target_bytes / M,
                       G1RemoteAllowDenseObjectEviction ? "on" : "off");
        }
      } else if (G1RemoteEvictionThreshold > 0) {
        // Legacy threshold mode: evict when total heap > threshold% of Xmx
        size_t threshold_bytes = (heap_capacity * G1RemoteEvictionThreshold) / 100;
        if (local_used > threshold_bytes) {
          eviction_tier = 2;
          evict_target_bytes = local_used - threshold_bytes;
          log_info(gc)("Legacy eviction: heap " SIZE_FORMAT "MB / " SIZE_FORMAT "MB "
                       "(threshold %u%% = " SIZE_FORMAT "MB), need " SIZE_FORMAT "MB",
                       local_used / M, heap_capacity / M,
                       G1RemoteEvictionThreshold, threshold_bytes / M,
                       evict_target_bytes / M);
        }
      }

      if (eviction_tier > 0 && evict_target_bytes > 0) {
        size_t path2_bytes = 0;
        size_t path2_cold_bytes = 0;
        size_t path2_fallback_bytes = 0;
        size_t sampled_words = 0;
        size_t cold_words = 0;
        size_t hot_words = 0;
        size_t unknown_words = 0;
        int path2_cold_candidates = 0;
        int path2_fallback_candidates = 0;
        int path2_regions_scanned = 0;
        int path2_regions_not_cold = 0;
        int path2_regions_dense_small = 0;
        int path2_regions_backoff_skipped = 0;
        size_t path2_dense_small_bytes = 0;
        size_t path2_dense_small_objects = 0;
        int path2_dense_last_resort_candidates = 0;
        size_t path2_dense_last_resort_bytes = 0;
        size_t path2_dense_last_resort_objects = 0;
        const bool allow_dense_object_granularity_eviction =
          G1RemoteAllowDenseObjectEviction;
        bool unlimited = false;
        G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
        bool* dense_deferred_candidates = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
        memset(dense_deferred_candidates, 0, num_regions * sizeof(bool));

        // Prefer old regions whose sampled objects have stale hotness epochs.
        // This makes T1 truly cold-region eviction instead of heap-index-order
        // old-region eviction. For non-emergency tiers, defer dense tiny-object
        // regions: they are cheap to classify as cold but very expensive to
        // evict and fault back object-by-object under Spark's scan pattern.
        if (rmm != nullptr) {
          for (uint i = 0; i < num_regions; i++) {
            if (!unlimited && path2_bytes >= evict_target_bytes) break;

            HeapRegion* hr = _g1h->region_at(i);
            if (!hr->is_old() || hr->is_humongous() || hr->is_empty()) continue;
            if (hr->is_cold_destination() || hr->is_fetch_cache() || hr->is_evict_guarded()) continue;
            if (eviction_candidates[i]) continue;
            if (rmm->is_region_in_eviction_backoff(i)) {
              path2_regions_backoff_skipped++;
              continue;
            }

            RegionColdnessSample region_sample;
            path2_regions_scanned++;
            if (!region_is_cold_by_epoch(hr, rmm, false, &region_sample)) {
              path2_regions_not_cold++;
              sampled_words += region_sample.sampled_words;
              cold_words += region_sample.cold_words;
              hot_words += region_sample.hot_words;
              unknown_words += region_sample.unknown_words;
              if (region_sample.dense_small_objects) {
                dense_deferred_candidates[i] = true;
                path2_regions_dense_small++;
                path2_dense_small_bytes += hr->used();
                path2_dense_small_objects += region_sample.object_count;
              }
              continue;
            }

            if (region_sample.dense_small_objects) {
              dense_deferred_candidates[i] = true;
              path2_regions_dense_small++;
              path2_dense_small_bytes += hr->used();
              path2_dense_small_objects += region_sample.object_count;
              sampled_words += region_sample.sampled_words;
              cold_words += region_sample.cold_words;
              hot_words += region_sample.hot_words;
              unknown_words += region_sample.unknown_words;
              continue;
            }

            hr->set_cold_destination();
            eviction_candidates[i] = true;
            path2_candidates++;
            path2_cold_candidates++;
            path2_bytes += hr->used();
            path2_cold_bytes += hr->used();
            sampled_words += region_sample.sampled_words;
            cold_words += region_sample.cold_words;
            hot_words += region_sample.hot_words;
            unknown_words += region_sample.unknown_words;
          }
        }

        if (path2_cold_candidates > 0 || path2_regions_scanned > 0) {
          size_t sampled_bytes = sampled_words * HeapWordSize;
          size_t cold_bytes = cold_words * HeapWordSize;
          size_t hot_bytes = hot_words * HeapWordSize;
          size_t unknown_bytes = unknown_words * HeapWordSize;
          log_info(gc)("Path 2 cold scan: selected=%d/%d regions (" SIZE_FORMAT "MB), "
                       "not_cold=%d, backoff=%d, dense_small=%d (" SIZE_FORMAT "MB, "
                       SIZE_FORMAT " objs), sample cold=" SIZE_FORMAT "MB hot="
                       SIZE_FORMAT "MB unknown=" SIZE_FORMAT "MB sampled="
                       SIZE_FORMAT "MB",
                       path2_cold_candidates, path2_regions_scanned,
                       path2_cold_bytes / M, path2_regions_not_cold,
                       path2_regions_backoff_skipped,
                       path2_regions_dense_small, path2_dense_small_bytes / M,
                       path2_dense_small_objects,
                       cold_bytes / M, hot_bytes / M, unknown_bytes / M,
                       sampled_bytes / M);
        }

        // Under higher local pressure, fall back to old regions after
        // exhausting cold candidates. Dense tiny-object regions are deliberately
        // excluded: object-granularity eviction of Spark's cached partitions
        // has poor reclaim/fetch economics and currently leaves stale raw
        // references. A region/page-granularity path should handle them.
        if (eviction_tier >= 2 && (unlimited || path2_bytes < evict_target_bytes)) {
          for (uint i = 0; i < num_regions; i++) {
            if (!unlimited && path2_bytes >= evict_target_bytes) break;

            HeapRegion* hr = _g1h->region_at(i);
            if (!hr->is_old() || hr->is_humongous() || hr->is_empty()) continue;
            if (hr->is_cold_destination() || hr->is_fetch_cache() || hr->is_evict_guarded()) continue;
            if (eviction_candidates[i]) continue;
            if (dense_deferred_candidates[i]) continue;
            if (rmm != nullptr && rmm->is_region_in_eviction_backoff(i)) {
              path2_regions_backoff_skipped++;
              continue;
            }

            if (rmm != nullptr) {
              RegionColdnessSample region_sample;
              (void)region_is_cold_by_epoch(hr, rmm, false, &region_sample);
              if (region_sample.dense_small_objects) {
                if (!dense_deferred_candidates[i]) {
                  dense_deferred_candidates[i] = true;
                  path2_regions_dense_small++;
                  path2_dense_small_bytes += hr->used();
                  path2_dense_small_objects += region_sample.object_count;
                }
                continue;
              }
            }

            hr->set_cold_destination();
            eviction_candidates[i] = true;
            path2_candidates++;
            path2_fallback_candidates++;
            path2_bytes += hr->used();
            path2_fallback_bytes += hr->used();
          }

          if (path2_fallback_candidates > 0) {
            log_info(gc)("Path 2 fallback selected %d old regions (" SIZE_FORMAT
                         "MB) for T%d pressure",
                         path2_fallback_candidates, path2_fallback_bytes / M,
                         eviction_tier);
          }
        }

        if (allow_dense_object_granularity_eviction &&
            eviction_tier >= 2 && path2_bytes < evict_target_bytes) {
          uint dense_region_cap = eviction_tier >= 3 ? G1RemoteDenseT3Regions : G1RemoteDenseT2Regions;
          size_t dense_last_resort_cap = HeapRegion::GrainBytes * (size_t)dense_region_cap;
          size_t remaining_target = evict_target_bytes - path2_bytes;
          dense_last_resort_cap = MIN2(dense_last_resort_cap, remaining_target);
          dense_last_resort_cap = MIN2(dense_last_resort_cap, evict_batch_cap_bytes);

          for (uint i = 0; i < num_regions && dense_last_resort_cap > 0; i++) {
            if (path2_dense_last_resort_bytes >= dense_last_resort_cap) break;
            if (!dense_deferred_candidates[i]) continue;

            HeapRegion* hr = _g1h->region_at(i);
            if (!hr->is_old() || hr->is_humongous() || hr->is_empty()) continue;
            if (hr->is_cold_destination() || hr->is_fetch_cache() || hr->is_evict_guarded()) continue;
            if (eviction_candidates[i]) continue;
            if (rmm != nullptr && rmm->is_region_in_eviction_backoff(i)) {
              path2_regions_backoff_skipped++;
              continue;
            }

            RegionColdnessSample region_sample;
            if (rmm != nullptr) {
              (void)region_is_cold_by_epoch(hr, rmm, false, &region_sample);
            }

            hr->set_cold_destination();
            eviction_candidates[i] = true;
            path2_candidates++;
            path2_fallback_candidates++;
            path2_bytes += hr->used();
            path2_fallback_bytes += hr->used();
            path2_dense_last_resort_candidates++;
            path2_dense_last_resort_bytes += hr->used();
            path2_dense_last_resort_objects += region_sample.object_count;
          }

          if (path2_dense_last_resort_candidates > 0) {
            log_info(gc)("Path 2 dense last-resort selected %d regions ("
                         SIZE_FORMAT "MB, " SIZE_FORMAT " objs) for T%d pressure",
                         path2_dense_last_resort_candidates,
                         path2_dense_last_resort_bytes / M,
                         path2_dense_last_resort_objects,
                         eviction_tier);
          }
        }

        if (path2_candidates > 0) {
          log_info(gc)("Path 2 selected %d regions (" SIZE_FORMAT "MB, cold="
                       SIZE_FORMAT "MB fallback=" SIZE_FORMAT "MB) for T%d eviction",
                       path2_candidates, path2_bytes / M, path2_cold_bytes / M,
                       path2_fallback_bytes / M, eviction_tier);
        }

        FREE_C_HEAP_ARRAY(bool, dense_deferred_candidates);
      }
    }

    int total_candidates = path1_candidates + path2_candidates;

    if (G1DeoptimizeBeforeEviction && total_candidates > 0) {
      Ticks deopt_start = Ticks::now();
      DeoptimizationScope deopt_scope;
      CodeCache::mark_all_nmethods_for_deoptimization(&deopt_scope);
      deopt_scope.deoptimize_marked();
      double deopt_ms = (Ticks::now() - deopt_start).seconds() * 1000.0;
      log_info(gc)("Remote eviction: deoptimized compiled Java frames in %.1fms "
                   "before processing %d candidate regions",
                   deopt_ms, total_candidates);
    }

    // ---- Phase C.9: Raw stack guard before root-catch relocation ----
    // Root-catch installs forwarding pointers in old candidate objects.
    // If a later raw-stack guard keeps that region local, those forwarding
    // stubs become mutator-visible. Guard stack-held regions before any
    // relocation so kept-local regions remain untouched.
    if (total_candidates > 0) {
      int raw_guarded = guard_eviction_candidates_with_raw_stack(
          _g1h, eviction_candidates, num_regions, "Pre-D raw stack guard");
      if (raw_guarded > 0) {
        total_candidates -= raw_guarded;
      }
    }

    // ---- Phase D: Root-catch relocation ----
    // Instead of pinning entire 16MB regions for a few root-referenced
    // objects (often just a Double or small array), relocate root-pinned
    // objects to a dedicated "root-catch" region. This allows eviction of
    // regions that previously had 1-2 root refs blocking them.
    if (total_candidates > 0) {
      int pin_capacity = 131072;
      int pin_grows = 0;
      RootPinEntry* pins = NEW_C_HEAP_ARRAY(RootPinEntry, pin_capacity, mtGC);
      int num_pins = 0;

      class EvictionThreadRootPinClosure : public OopClosure {
        G1CollectedHeap* _g1h;
        bool*            _eviction_candidates;
        uint             _num_regions;
        int              _regions_guarded;
        int              _roots_found;

      public:
        EvictionThreadRootPinClosure(G1CollectedHeap* g1h, bool* candidates,
                                     uint num_regions)
          : _g1h(g1h), _eviction_candidates(candidates), _num_regions(num_regions),
            _regions_guarded(0), _roots_found(0) {}

        void do_oop(oop* p) {
          uintptr_t raw = *(uintptr_t*)p;
          if (raw == 0) return;
          if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
              (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;

          uintptr_t addr = raw;
          if ((raw & G1_OOP_TAG_MASK) != 0) {
            addr = raw & G1_OOP_ADDR_MASK;
          }
          if (!is_aligned((address)addr, HeapWordSize)) return;
          if (!_g1h->is_in_reserved((void*)addr)) return;

          HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)addr);
          if (hr == nullptr) return;

          uint idx = hr->hrm_index();
          if (idx >= _num_regions) return;

          if (_eviction_candidates[idx]) {
            _eviction_candidates[idx] = false;
            hr->clear_cold_destination();
            _regions_guarded++;
            _roots_found++;
          } else {
            _roots_found++;
          }
        }
        void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }

        int regions_guarded() const { return _regions_guarded; }
        int roots_found() const { return _roots_found; }
      };

      // Thread oop-map slots can still contain stale or interior heap-looking
      // words in this late post-evacuation eviction pass. Do not parse object
      // headers or relocate them here; conservatively keep their regions local.
      EvictionThreadRootPinClosure thread_pin_cl(_g1h, eviction_candidates, num_regions);
      Threads::oops_do(&thread_pin_cl, nullptr);
      if (thread_pin_cl.regions_guarded() > 0) {
        total_candidates -= thread_pin_cl.regions_guarded();
        log_info(gc)("Root-catch: pinned %d candidate regions for %d thread roots",
                     thread_pin_cl.regions_guarded(), thread_pin_cl.roots_found());
      }

      class EvictionRootCollectClosure : public OopClosure {
        G1CollectedHeap* _g1h;
        const bool*      _eviction_candidates;
        uint             _num_regions;
        RootPinEntry**   _pins;
        int&             _num_pins;
        int&             _pin_capacity;
        int&             _pin_grows;

        void append(oop* root, uintptr_t obj_addr, bool update_root) {
          if (_num_pins >= _pin_capacity) {
            int new_capacity = _pin_capacity + MAX2(_pin_capacity / 2, 4096);
            RootPinEntry* new_pins = NEW_C_HEAP_ARRAY(RootPinEntry, new_capacity, mtGC);
            memcpy(new_pins, *_pins, _num_pins * sizeof(RootPinEntry));
            FREE_C_HEAP_ARRAY(RootPinEntry, *_pins);
            *_pins = new_pins;
            _pin_capacity = new_capacity;
            _pin_grows++;
          }
          (*_pins)[_num_pins] = {root, obj_addr, update_root};
          _num_pins++;
        }

        bool is_valid_root_object(oop obj, HeapRegion** region_out = nullptr) {
          size_t word_size = 0;
          const char* reason = nullptr;
          return remote_eviction_is_relocatable_object(
              _g1h, cast_from_oop<uintptr_t>(obj), region_out, &word_size, &reason);
        }

      public:
        EvictionRootCollectClosure(G1CollectedHeap* g1h, const bool* candidates,
                                   uint num_regions, RootPinEntry** pins,
                                   int& num_pins, int& pin_capacity, int& pin_grows)
          : _g1h(g1h), _eviction_candidates(candidates), _num_regions(num_regions),
            _pins(pins), _num_pins(num_pins), _pin_capacity(pin_capacity),
            _pin_grows(pin_grows) {}
        void do_oop(oop* p) {
          oop obj = *p;
          if (obj == nullptr) return;
          uintptr_t raw = cast_from_oop<uintptr_t>(obj);
          if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
              (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;
          if ((raw & G1_OOP_TAG_MASK) != 0) {
            obj = cast_to_oop(raw & G1_OOP_ADDR_MASK);
          }
          HeapRegion* hr = nullptr;
          if (!is_valid_root_object(obj, &hr)) return;
          uint idx = hr->hrm_index();
          if (idx < _num_regions && _eviction_candidates[idx]) {
            append(p, cast_from_oop<uintptr_t>(obj), true);
          }
        }
        void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }
      };

      EvictionRootCollectClosure collect_cl(_g1h, eviction_candidates, num_regions,
                                            &pins, num_pins, pin_capacity, pin_grows);
      JNIHandles::oops_do(&collect_cl);
      OopStorageSet::strong_oops_do(&collect_cl);
      for (auto id : EnumRange<OopStorageSet::WeakId>()) {
        OopStorageSet::storage(id)->oops_do(&collect_cl);
      }
      {
        CLDToOopClosure cld_cl(&collect_cl, ClassLoaderData::_claim_none);
        ClassLoaderDataGraph::cld_do(&cld_cl);
      }
      {
        CodeBlobToOopClosure code_cl(&collect_cl, false);
        CodeCache::blobs_do(&code_cl);
      }
      _g1h->ref_processor_cm()->weak_oops_do(&collect_cl);

      bool remote_anchor_overflow = false;
      int remote_anchor_seen = 0;
      uintptr_t* remote_anchor_addrs = nullptr;
      remote_anchor_seen = rmm->collect_remote_anchor_addrs_in_regions(
          eviction_candidates, num_regions, nullptr, 0, &remote_anchor_overflow);

      int remote_anchor_capacity = remote_anchor_seen;
      if (remote_anchor_capacity > 0) {
        remote_anchor_addrs = NEW_C_HEAP_ARRAY(uintptr_t, remote_anchor_capacity, mtGC);
      }
      remote_anchor_overflow = false;
      remote_anchor_seen = rmm->collect_remote_anchor_addrs_in_regions(
          eviction_candidates, num_regions, remote_anchor_addrs,
          remote_anchor_capacity, &remote_anchor_overflow);
      int remote_anchor_stored = MIN2(remote_anchor_seen, remote_anchor_capacity);
      for (int i = 0; i < remote_anchor_stored; i++) {
        if (num_pins >= pin_capacity) {
          int new_capacity = pin_capacity + MAX2(pin_capacity / 2, 4096);
          RootPinEntry* new_pins = NEW_C_HEAP_ARRAY(RootPinEntry, new_capacity, mtGC);
          memcpy(new_pins, pins, num_pins * sizeof(RootPinEntry));
          FREE_C_HEAP_ARRAY(RootPinEntry, pins);
          pins = new_pins;
          pin_capacity = new_capacity;
          pin_grows++;
        }
        pins[num_pins] = {nullptr, remote_anchor_addrs[i], false};
        num_pins++;
      }
      if (remote_anchor_addrs != nullptr) {
        FREE_C_HEAP_ARRAY(uintptr_t, remote_anchor_addrs);
      }
      if (remote_anchor_seen > remote_anchor_stored) {
        remote_anchor_overflow = true;
      }

      // Relocate collected root-pinned objects to a catch region
      if (remote_anchor_overflow) {
        int overflow_pinned = 0;
        for (uint i = 0; i < num_regions; i++) {
          if (eviction_candidates[i]) {
            HeapRegion* hr = _g1h->region_at(i);
            eviction_candidates[i] = false;
            hr->clear_cold_destination();
            regions_pinned++;
            overflow_pinned++;
          }
        }
        total_candidates -= overflow_pinned;
        log_warning(gc)("Root-catch overflow: pinned %d candidate regions "
                        "(pins=%d capacity=%d grows=%d remote-anchor=%d stored=%d)",
                        overflow_pinned, num_pins, pin_capacity, pin_grows,
                        remote_anchor_seen, remote_anchor_stored);
      } else if (num_pins > 0) {
        qsort(pins, num_pins, sizeof(RootPinEntry), rpe_cmp);

        HeapRegion* root_catch = nullptr;
        HeapWord* catch_top = nullptr;
        uint first_root_catch_idx = (uint)-1;
        int catch_regions = 0;
        int relocated = 0;
        int roots_updated = 0;
        int fallback_pinned = 0;
        size_t root_catch_words = 0;
        bool catch_alloc_failed = false;

        uintptr_t prev_addr = 0;
        HeapWord* prev_new = nullptr;

        for (int i = 0; i < num_pins; i++) {
          uintptr_t obj_addr = pins[i].obj_addr;
          oop* root_p = pins[i].root;
          bool update_root = pins[i].update_root && root_p != nullptr;

          if (obj_addr == prev_addr && prev_new != nullptr) {
            // Duplicate root to same object. Remote-anchor pins have no root
            // slot; their Handle was updated by the first relocation and any
            // duplicates are handled by the forwarding fixup below.
            if (update_root) {
              uintptr_t raw = cast_from_oop<uintptr_t>(*root_p);
              uintptr_t tags = raw & G1_OOP_TAG_MASK;
              *root_p = cast_to_oop(tags | ((uintptr_t)prev_new & G1_OOP_ADDR_MASK));
              roots_updated++;
            }
            continue;
          }

          HeapRegion* pin_hr = nullptr;
          const char* invalid_reason = nullptr;
          if (!remote_eviction_is_block_start(_g1h, obj_addr, &pin_hr, &invalid_reason)) {
            if (pin_hr != nullptr) {
              uint idx = pin_hr->hrm_index();
              if (idx < num_regions && eviction_candidates[idx]) {
                eviction_candidates[idx] = false;
                pin_hr->clear_cold_destination();
                regions_pinned++;
                total_candidates--;
                fallback_pinned++;
              }
            }
            if (fallback_pinned <= 32) {
              log_warning(gc)("Root-catch: skipped invalid pin addr=" PTR_FORMAT
                              " reason=%s region=%u",
                              obj_addr,
                              invalid_reason == nullptr ? "?" : invalid_reason,
                              pin_hr == nullptr ? 9999 : pin_hr->hrm_index());
            }
            prev_addr = obj_addr;
            prev_new = nullptr;
            continue;
          }

          oop obj = cast_to_oop(obj_addr);
          if (obj->is_forwarded()) {
            // Already relocated by an earlier entry; update root to forwardee.
            oop fwd = obj->forwardee();
            if (update_root) {
              uintptr_t raw = cast_from_oop<uintptr_t>(*root_p);
              uintptr_t tags = raw & G1_OOP_TAG_MASK;
              *root_p = cast_to_oop(tags | (cast_from_oop<uintptr_t>(fwd) & G1_OOP_ADDR_MASK));
              roots_updated++;
            }
            prev_addr = obj_addr;
            prev_new = cast_from_oop<HeapWord*>(fwd);
            continue;
          }

          size_t word_sz = 0;
          if (!remote_eviction_is_relocatable_object(_g1h, obj_addr, &pin_hr,
                                                     &word_sz, &invalid_reason)) {
            if (pin_hr != nullptr) {
              uint idx = pin_hr->hrm_index();
              if (idx < num_regions && eviction_candidates[idx]) {
                eviction_candidates[idx] = false;
                pin_hr->clear_cold_destination();
                regions_pinned++;
                total_candidates--;
                fallback_pinned++;
              }
            }
            if (fallback_pinned <= 32) {
              log_warning(gc)("Root-catch: pinned region for invalid object addr="
                              PTR_FORMAT " reason=%s region=%u",
                              obj_addr,
                              invalid_reason == nullptr ? "?" : invalid_reason,
                              pin_hr == nullptr ? 9999 : pin_hr->hrm_index());
            }
            prev_addr = obj_addr;
            prev_new = nullptr;
            continue;
          }
          if (root_catch == nullptr) {
            root_catch = _g1h->allocate_fcr_region();
            if (root_catch != nullptr) {
              catch_top = root_catch->bottom();
              if (first_root_catch_idx == (uint)-1) {
                first_root_catch_idx = root_catch->hrm_index();
              }
              catch_regions++;
            }
          } else if (catch_top + word_sz > root_catch->end() &&
                     catch_top != root_catch->bottom()) {
            dirty_root_catch_region(_g1h, root_catch, catch_top);
            root_catch = _g1h->allocate_fcr_region();
            if (root_catch != nullptr) {
              catch_top = root_catch->bottom();
              catch_regions++;
            }
          }

          if (root_catch == nullptr || catch_top + word_sz > root_catch->end()) {
            catch_alloc_failed = true;
            HeapRegion* hr = _g1h->heap_region_containing(obj);
            uint idx = hr->hrm_index();
            if (idx < num_regions && eviction_candidates[idx]) {
              eviction_candidates[idx] = false;
              regions_pinned++;
              total_candidates--;
              fallback_pinned++;
            }
            prev_addr = obj_addr;
            prev_new = nullptr;
            continue;
          }

          // Copy object to catch region.
          Copy::aligned_disjoint_words((HeapWord*)obj_addr, catch_top, word_sz);
          oop new_obj = cast_to_oop(catch_top);
          root_catch->update_bot_for_obj(catch_top, word_sz);

          // Install forwarding pointer in old location (Phase B/C/E check this).
          obj->forward_to(new_obj);

          // Update root (preserve tag bits).
          if (update_root) {
            uintptr_t raw = cast_from_oop<uintptr_t>(*root_p);
            uintptr_t tags = raw & G1_OOP_TAG_MASK;
            *root_p = cast_to_oop(tags | ((uintptr_t)catch_top & G1_OOP_ADDR_MASK));
            roots_updated++;
          }

          // Rekey handle table (old_addr -> new_addr).
          rmm->update_handle_for_evacuation(obj, new_obj);

          prev_addr = obj_addr;
          prev_new = catch_top;
          catch_top += word_sz;
          root_catch_words += word_sz;
          relocated++;
        }

        if (root_catch != nullptr) {
          if (catch_top == root_catch->bottom()) {
            FreeRegionList tmp("tmp");
            _g1h->free_region(root_catch, &tmp);
          } else {
            dirty_root_catch_region(_g1h, root_catch, catch_top);
          }
        }

        if (relocated > 0 || fallback_pinned > 0 || catch_alloc_failed) {
          uint log_first_idx = (first_root_catch_idx == (uint)-1) ? 0 : first_root_catch_idx;
          log_info(gc)("Root-catch relocation: %d objects (%zuKB) relocated to %d catch regions "
                       "(first=%u), %d roots updated, %d regions fallback-pinned, pins=%d "
                       "capacity=%d grows=%d remote-anchor=%d",
                       relocated, root_catch_words * HeapWordSize / K,
                       catch_regions, log_first_idx, roots_updated,
                       fallback_pinned, num_pins, pin_capacity, pin_grows,
                       remote_anchor_seen);
        }
        if (catch_alloc_failed) {
          log_warning(gc)("Root-catch: catch region allocation failed or object too large; "
                          "fallback-pinned %d regions", fallback_pinned);
        }

        // Root-catch installs forwarding pointers in the old candidate
        // regions. Update every LOCAL Handle that still points at one of
        // those forwarding stubs, including duplicate dormant anchors.
        rmm->fixup_all_local_handles();
      }

      // Clean up cold_destination/root_pinned flags from earlier scan
      for (uint i = 0; i < num_regions; i++) {
        HeapRegion* hr = _g1h->region_at(i);
        if (hr->is_root_pinned()) {
          hr->clear_cold_destination();
          hr->clear_root_pinned();
        }
      }
      FREE_C_HEAP_ARRAY(RootPinEntry, pins);
    }

    if (total_candidates > 0) {
      log_info(gc)("Eviction candidates: %d path1 + %d path2 = %d surviving (%d pinned early)",
                   path1_candidates, path2_candidates, total_candidates, regions_pinned);

      // ---- Phase B: Ensure handles for all objects in candidate regions ----
      {
        Ticks phase_b_start = Ticks::now();
        uint nworkers = _g1h->workers()->active_workers();

        class EnsureHandlesTask : public WorkerTask {
          G1RemoteMemoryManager* _rmm;
          G1CollectedHeap*       _g1h;
          const bool*            _eviction_candidates;
          uint                   _num_regions;
          HeapRegionClaimer      _claimer;
          volatile int           _total_handles;
        public:
          EnsureHandlesTask(G1RemoteMemoryManager* rmm, G1CollectedHeap* g1h,
                            const bool* candidates, uint num_regions, uint num_workers)
            : WorkerTask("G1 Ensure Handles"),
              _rmm(rmm), _g1h(g1h),
              _eviction_candidates(candidates), _num_regions(num_regions),
              _claimer(num_workers), _total_handles(0) {}

          void work(uint worker_id) {
            RemoteHandleAllocBuffer hab;
            G1RemoteMemoryManager::HandleEntryAllocBuffer eab;
            int count = 0;
            for (uint i = _claimer.offset_for_worker(worker_id); i < _num_regions; i++) {
              if (!_eviction_candidates[i]) continue;
              if (!_claimer.claim_region(i)) continue;
              HeapRegion* hr = _g1h->region_at(i);
              HeapWord* p = hr->bottom();
              HeapWord* region_end = hr->end();
              while (p < hr->top()) {
                if (p < hr->bottom() || p >= region_end) break;
                oop obj = cast_to_oop(p);
                if (obj->is_forwarded()) {
                  p += obj->size_given_klass(obj->klass());
                  continue;
                }
                if (obj->klass_or_null() == nullptr) break;
                size_t sz = obj->size();
                if (sz == 0 || sz > (size_t)(region_end - p)) break;
                _rmm->ensure_handle_for_parallel(obj, &hab, &eab);
                count++;
                p += sz;
              }
            }
            Atomic::add(&_total_handles, count);
          }
          int total_handles() const { return _total_handles; }
        };

        if (nworkers > 1) {
          EnsureHandlesTask task(rmm, _g1h, eviction_candidates, num_regions, nworkers);
          _g1h->workers()->run_task(&task, nworkers);
          double phase_b_ms = (Ticks::now() - phase_b_start).seconds() * 1000.0;
          log_info(gc)("Phase B ensure handles: %.1fms (%d handles, %u workers)",
                       phase_b_ms, task.total_handles(), nworkers);
        } else {
          int count = 0;
          for (uint i = 0; i < num_regions; i++) {
            if (!eviction_candidates[i]) continue;
            HeapRegion* hr = _g1h->region_at(i);
            HeapWord* p = hr->bottom();
            HeapWord* region_end = hr->end();
            while (p < hr->top()) {
              if (p < hr->bottom() || p >= region_end) break;
              oop obj = cast_to_oop(p);
              if (obj->is_forwarded()) {
                p += obj->size_given_klass(obj->klass());
                continue;
              }
              if (obj->klass_or_null() == nullptr) break;
              size_t sz = obj->size();
              if (sz == 0 || sz > (size_t)(region_end - p)) break;
              rmm->ensure_handle_for(obj, &hab);
              count++;
              p += sz;
            }
          }
          double phase_b_ms = (Ticks::now() - phase_b_start).seconds() * 1000.0;
          log_info(gc)("Phase B ensure handles: %.1fms (%d handles, 1 worker)", phase_b_ms, count);
        }
      }

      // ---- Phase C: Tag refs to eviction candidates ----
      // The default full scan is O(entire_heap) but conservative.  The fast
      // scanner is a diagnostic/capability path guarded by verification: it
      // scans candidates, young regions, newly evacuated ranges, roots, and
      // candidate remembered sets.  If verification finds a miss, eviction
      // aborts and tagged refs are restored.
      {
        Ticks phase_c_start = Ticks::now();
        uint nworkers = _g1h->workers()->active_workers();
        int tagged = G1RemoteUseFastPhaseC
          ? rmm->tag_refs_to_eviction_set_fast(eviction_candidates,
                                               num_regions,
                                               _pre_evac_tops,
                                               _g1h->workers(), nworkers)
          : rmm->tag_all_heap_refs_to_eviction_set(eviction_candidates,
                                                   num_regions,
                                                   _g1h->workers(), nworkers);
        double phase_c_ms = (Ticks::now() - phase_c_start).seconds() * 1000.0;
        log_info(gc)("Phase C %s scan: %.1fms (%u workers, %d tagged)",
                     G1RemoteUseFastPhaseC ? "fast" : "full",
                     phase_c_ms, nworkers, tagged);
      }

      // ---- Phase C.1: Safety-net scan of newly-evacuated areas ----
      // Objects evacuated during this GC land above _pre_evac_tops[i] in
      // destination regions.  The general Phase C scan covers them via
      // sequential iteration above parsable_bottom, but truncation on
      // unexpected heap gaps can silently skip objects.  This targeted
      // pass re-scans only the newly-evacuated portion of each region.
      if (_pre_evac_tops != nullptr) {
        Ticks phase_c1_start = Ticks::now();
        int c1_tagged = rmm->tag_evacuated_area_refs_to_eviction_set(
            eviction_candidates, num_regions, _pre_evac_tops);
        double phase_c1_ms = (Ticks::now() - phase_c1_start).seconds() * 1000.0;
        if (c1_tagged > 0) {
          log_warning(gc)("Phase C.1 safety-net: %.1fms, tagged %d refs missed by general scan",
                          phase_c1_ms, c1_tagged);
        } else {
          log_info(gc)("Phase C.1 safety-net: %.1fms, 0 missed refs", phase_c1_ms);
        }
      }

      // ---- Phase C.5: Verify no untagged refs remain ----
      if (G1RemoteVerifyEvictionRefs) {
        Ticks phase_c5_start = Ticks::now();
        int missed = rmm->verify_no_untagged_refs_to_eviction_set(eviction_candidates,
                                                                  num_regions,
                                                                  _pre_evac_tops);
        double phase_c5_ms = (Ticks::now() - phase_c5_start).seconds() * 1000.0;
        log_info(gc)("Phase C.5 verify: %.1fms (%d missed heap refs)", phase_c5_ms, missed);
        if (missed > 0) {
          log_warning(gc)("Eviction ABORTED: %d untagged HEAP refs found after tagging", missed);
          int backoff_regions = 0;
          for (uint i = 0; i < num_regions; i++) {
            if (eviction_candidates[i]) {
              if (G1RemoteEvictionAbortBackoffGCCycles > 0) {
                rmm->backoff_eviction_region(i, G1RemoteEvictionAbortBackoffGCCycles);
                backoff_regions++;
              }
              HeapRegion* hr = _g1h->region_at(i);
              hr->clear_cold_destination();
              eviction_candidates[i] = false;
            }
          }
          if (backoff_regions > 0) {
            log_warning(gc)("Eviction backoff: skipping %d aborted candidate regions for %u GC cycles",
                            backoff_regions, G1RemoteEvictionAbortBackoffGCCycles);
          }
          total_candidates = 0;
          rmm->untag_all_heap_refs();
        }
      }

      // ---- Pre-E Safety Guard: remove candidate regions with dangling roots ----
      // Root-catch should have relocated all root-referenced objects, but may
      // miss some (catch region full, roots added during Phase B/C scanning).
      // Any region with raw root oops MUST NOT be evicted — those roots bypass
      // the load barrier and would SIGSEGV on madvise'd pages.
      {
        class PreEvictionRootGuardClosure : public OopClosure {
          G1CollectedHeap* _g1h;
          bool*            _eviction_candidates;
          uint             _num_regions;
          int              _regions_guarded;
          int              _roots_found;
        public:
          PreEvictionRootGuardClosure(G1CollectedHeap* g1h, bool* candidates, uint num_regions)
            : _g1h(g1h), _eviction_candidates(candidates), _num_regions(num_regions),
              _regions_guarded(0), _roots_found(0) {}
          void do_oop(oop* p) {
            uintptr_t raw = *(uintptr_t*)p;
            if (raw == 0) return;
            if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
                (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;
            oop obj;
            if ((raw & G1_OOP_TAG_MASK) != 0) {
              obj = cast_to_oop(raw & G1_OOP_ADDR_MASK);
            } else {
              obj = (oop)raw;
            }
            if (!_g1h->is_in(obj)) return;
            HeapRegion* hr = _g1h->heap_region_containing(obj);
            uint idx = hr->hrm_index();
            if (idx < _num_regions && _eviction_candidates[idx]) {
              _eviction_candidates[idx] = false;
              hr->clear_cold_destination();
              _regions_guarded++;
              _roots_found++;
            } else if (idx < _num_regions) {
              _roots_found++;
            }
          }
          void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }
          int regions_guarded() const { return _regions_guarded; }
          int roots_found() const { return _roots_found; }
        };

        PreEvictionRootGuardClosure guard_cl(_g1h, eviction_candidates, num_regions);
        Threads::oops_do(&guard_cl, nullptr);
        JNIHandles::oops_do(&guard_cl);
        OopStorageSet::strong_oops_do(&guard_cl);
        {
          CLDToOopClosure cld_cl(&guard_cl, ClassLoaderData::_claim_none);
          ClassLoaderDataGraph::cld_do(&cld_cl);
        }
        {
          CodeBlobToOopClosure code_cl(&guard_cl, false);
          CodeCache::blobs_do(&code_cl);
        }

        if (guard_cl.regions_guarded() > 0) {
          total_candidates -= guard_cl.regions_guarded();
          log_info(gc)("Pre-E root guard: removed %d candidate regions with %d dangling roots",
                       guard_cl.regions_guarded(), guard_cl.roots_found());
        }

        class PreEvictionRawStackGuardClosure : public ThreadClosure {
          G1CollectedHeap* _g1h;
          bool*            _eviction_candidates;
          uint             _num_regions;
          int              _regions_guarded;
          int              _stack_words_found;
          int              _threads_scanned;
          size_t           _words_scanned;
          int              _reports_left;

          bool maybe_guard_candidate(uintptr_t raw, Thread* thread, uintptr_t* slot) {
            if (raw == 0) return false;

            // Shared-handle references are already safe: they do not retain a
            // raw local object address into the region being evicted.
            if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
                (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
              return false;
            }

            uintptr_t addr = raw;
            if ((raw & G1_OOP_TAG_MASK) != 0) {
              addr = raw & G1_OOP_ADDR_MASK;
            }
            if (!is_aligned((address)addr, HeapWordSize)) return false;
            if (!_g1h->is_in_reserved((void*)addr)) return false;

            HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)addr);
            if (hr == nullptr) return false;

            uint idx = hr->hrm_index();
            if (idx >= _num_regions) return false;

            if (_eviction_candidates[idx]) {
              _eviction_candidates[idx] = false;
              hr->clear_cold_destination();
              _regions_guarded++;
              _stack_words_found++;
              if (_reports_left > 0) {
                log_warning(gc)("Pre-E raw stack guard: thread=" PTR_FORMAT
                                " slot=" PTR_FORMAT " raw=" PTR_FORMAT
                                " guards candidate region %u",
                                p2i(thread), p2i(slot), raw, idx);
                _reports_left--;
              }
              return true;
            }

            _stack_words_found++;
            return false;
          }

        public:
          PreEvictionRawStackGuardClosure(G1CollectedHeap* g1h, bool* candidates, uint num_regions)
            : _g1h(g1h), _eviction_candidates(candidates), _num_regions(num_regions),
              _regions_guarded(0), _stack_words_found(0), _threads_scanned(0),
              _words_scanned(0), _reports_left(32) {}

          void do_thread(Thread* thread) override {
            JavaThread* jt = JavaThread::cast(thread);
            if (!jt->has_last_Java_frame()) return;

            address java_sp = (address)jt->last_Java_sp();
            address high = jt->stack_base();
            if (java_sp == nullptr || high == nullptr || java_sp >= high) return;

            // Compiled/interpreter execution can leave raw clean oop values in
            // stack slots that are not described as roots at the safepoint.  A
            // bounded SP window missed long-lived task locals in Spark, so scan
            // the full usable Java stack conservatively.  Stay above the low
            // stack guard pages.
            address stack_low = jt->stack_overflow_state()->stack_reserved_zone_base();
            address low = MIN2(java_sp, stack_low);

            uintptr_t* cur = (uintptr_t*)align_up(low, sizeof(uintptr_t));
            uintptr_t* end = (uintptr_t*)align_down(high, sizeof(uintptr_t));
            _threads_scanned++;
            while (cur < end) {
              maybe_guard_candidate(*cur, thread, cur);
              cur++;
              _words_scanned++;
            }
          }

          int regions_guarded() const { return _regions_guarded; }
          int stack_words_found() const { return _stack_words_found; }
          int threads_scanned() const { return _threads_scanned; }
          size_t words_scanned() const { return _words_scanned; }
        };

        PreEvictionRawStackGuardClosure raw_stack_cl(_g1h, eviction_candidates, num_regions);
        Threads::java_threads_do(&raw_stack_cl);
        if (raw_stack_cl.regions_guarded() > 0) {
          total_candidates -= raw_stack_cl.regions_guarded();
          log_info(gc)("Pre-E raw stack guard: removed %d candidate regions with %d stack words "
                       "(scanned " SIZE_FORMAT " words in %d Java threads)",
                       raw_stack_cl.regions_guarded(), raw_stack_cl.stack_words_found(),
                       raw_stack_cl.words_scanned(), raw_stack_cl.threads_scanned());
        } else {
          log_info(gc)("Pre-E raw stack guard: scanned " SIZE_FORMAT
                       " words in %d Java threads, 0 candidate stack words",
                       raw_stack_cl.words_scanned(), raw_stack_cl.threads_scanned());
        }
      }

      // ---- Phase E: Evict non-pinned candidates (batched) ----
      {
      Ticks phase_e_start = Ticks::now();

      // E1: Collect eviction metadata. Edge tables and backend slots are
      // finished after late guards so rejected dense regions are cheap to drop.
      typedef G1RemoteMemoryManager::PreparedEviction PreparedEviction;
      int max_entries = 0;
      for (uint i = 0; i < num_regions; i++) {
        if (!eviction_candidates[i]) continue;
        HeapRegion* hr = _g1h->region_at(i);
        max_entries += (int)((hr->top() - hr->bottom()) / MinObjAlignmentInBytes) + 1;
      }
      if (max_entries > 8 * 1024 * 1024) max_entries = 8 * 1024 * 1024;
      PreparedEviction* entries = NEW_C_HEAP_ARRAY(PreparedEviction, max_entries, mtGC);
      int* region_start = NEW_C_HEAP_ARRAY(int, num_regions, mtGC);
      int* region_count_arr = NEW_C_HEAP_ARRAY(int, num_regions, mtGC);
      bool* region_complete = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
      bool* entry_active = NEW_C_HEAP_ARRAY(bool, max_entries, mtGC);
      bool* entry_sent = NEW_C_HEAP_ARRAY(bool, max_entries, mtGC);
      memset(region_start, 0, num_regions * sizeof(int));
      memset(region_count_arr, 0, num_regions * sizeof(int));
      memset(region_complete, 0, num_regions * sizeof(bool));
      memset(entry_active, 0, max_entries * sizeof(bool));
      memset(entry_sent, 0, max_entries * sizeof(bool));
      int num_entries = 0;

      for (uint i = 0; i < num_regions; i++) {
        if (!eviction_candidates[i]) continue;
        HeapRegion* hr = _g1h->region_at(i);
        region_start[i] = num_entries;
        int rcount = 0;
        bool all_prepared = true;
        HeapWord* p = hr->bottom();
        while (p < hr->top() && num_entries < max_entries) {
          oop obj = cast_to_oop(p);
          if (obj->is_forwarded()) {
            p += obj->size_given_klass(obj->klass());
            continue;
          }
          size_t sz = obj->size();
          if (rmm->prepare_eviction_metadata(obj, &hab, &entries[num_entries])) {
            entry_active[num_entries] = true;
            num_entries++;
            rcount++;
          } else {
            all_prepared = false;
          }
          p += sz;
        }
        if (p < hr->top()) all_prepared = false;
        region_count_arr[i] = rcount;
        region_complete[i] = all_prepared;
      }
      double e1_ms = (Ticks::now() - phase_e_start).seconds() * 1000.0;

      // E1.5: Partial-region eviction salvage.
      //
      // For regions where some objects failed prepare_eviction (typically
      // because their oop-edge count exceeded the build buffer, or a future
      // failure mode like locked-mark or slot-allocation), the original
      // policy was to keep the ENTIRE region pinned local. With dynamic
      // edge tables this should now be rare, but we still salvage:
      // we relocate the failed objects to a fresh FCR catch region (same
      // mechanism as Phase D root-catch) and mark the region complete so
      // E3 can free it. Without this, a single oversize array can pin a
      // 16MB region full of evictable objects.
      Ticks e1_5_start = Ticks::now();
      HeapRegion* edge_catch = nullptr;
      HeapWord*   catch_top  = nullptr;
      int relocated_count = 0;
      size_t relocated_bytes = 0;
      int salvaged_regions = 0;
      bool needs_catch = false;
      for (uint i = 0; i < num_regions; i++) {
        if (eviction_candidates[i] && !region_complete[i] && region_count_arr[i] > 0) {
          needs_catch = true;
          break;
        }
      }
      if (needs_catch) {
        edge_catch = _g1h->allocate_fcr_region();
        if (edge_catch != nullptr) catch_top = edge_catch->bottom();
      }
      if (edge_catch != nullptr) {
        for (uint i = 0; i < num_regions; i++) {
          if (!eviction_candidates[i]) continue;
          if (region_complete[i]) continue;
          if (region_count_arr[i] == 0) continue;  // nothing to salvage
          HeapRegion* hr = _g1h->region_at(i);
          int rstart = region_start[i];
          int rend   = rstart + region_count_arr[i];
          int next_entry = rstart;
          HeapWord* p = hr->bottom();
          bool fully_relocated = true;
          while (p < hr->top()) {
            oop obj = cast_to_oop(p);
            if (obj->is_forwarded()) {
              p += obj->size_given_klass(obj->klass());
              continue;
            }
            size_t sz = obj->size();
            // entries[] within a region are address-ordered (E1 walks linearly).
            bool is_prepared = (next_entry < rend && entries[next_entry].obj == obj);
            if (is_prepared) {
              next_entry++;
              p += sz;
              continue;
            }
            // Failed object — relocate to catch region. If full, try one more.
            if (catch_top + sz > edge_catch->end()) {
              edge_catch->set_top(catch_top);
              HeapRegion* next_catch = _g1h->allocate_fcr_region();
              if (next_catch == nullptr) {
                fully_relocated = false;
                break;
              }
              edge_catch = next_catch;
              catch_top = edge_catch->bottom();
              if (catch_top + sz > edge_catch->end()) {
                // Object larger than a region (humongous) — give up on this region.
                fully_relocated = false;
                break;
              }
            }
            Copy::aligned_disjoint_words(p, catch_top, sz);
            oop new_obj = cast_to_oop(catch_top);
            edge_catch->update_bot_for_obj(catch_top, sz);
            obj->forward_to(new_obj);
            rmm->update_handle_for_evacuation(obj, new_obj);
            catch_top      += sz;
            relocated_count++;
            relocated_bytes += sz * HeapWordSize;
            p              += sz;
          }
          if (fully_relocated && p >= hr->top()) {
            region_complete[i] = true;
            salvaged_regions++;
          }
        }
        if (catch_top != edge_catch->bottom()) {
          edge_catch->set_top(catch_top);
          // Dirty cards over the relocated range so next GC's Merge Heap
          // Roots scans them — same pattern as Phase D root-catch.
          G1CardTable* ct = _g1h->card_table();
          CardTable::CardValue* start_card = ct->byte_for(edge_catch->bottom());
          CardTable::CardValue* end_card   = ct->byte_for(catch_top - 1) + 1;
          memset(start_card, CardTable::dirty_card_val(), end_card - start_card);
          G1DirtyCardQueueSet& dcqs = G1BarrierSet::dirty_card_queue_set();
          G1DirtyCardQueue tmp_queue(&dcqs);
          for (CardTable::CardValue* card = start_card; card < end_card; card++) {
            dcqs.enqueue(tmp_queue, card);
          }
          dcqs.flush_queue(tmp_queue);
        } else {
          // Allocated but unused (e.g., all incomplete regions had zero successes).
          FreeRegionList tmp("tmp");
          _g1h->free_region(edge_catch, &tmp);
        }
      }
      double e1_5_ms = (Ticks::now() - e1_5_start).seconds() * 1000.0;
      if (relocated_count > 0 || salvaged_regions > 0) {
        log_info(gc)("E1.5 partial-eviction salvage: %d regions, %d objects (%zuKB) "
                     "relocated to catch region",
                     salvaged_regions, relocated_count, relocated_bytes / K);
      }

      // Regions that are still incomplete after salvage must stay entirely
      // local.  prepare_eviction_metadata() may have created LOCAL handles for
      // the evictable subset, but those objects must not get edge tables,
      // backend slots, or remote executor storage unless the whole region can
      // be committed in E3.  Otherwise the backend can store orphaned objects
      // while the JVM later logs "0 objects evicted".
      int incomplete_guarded_regions = 0;
      int incomplete_guarded_entries = 0;
      for (uint i = 0; i < num_regions; i++) {
        if (!eviction_candidates[i]) continue;
        if (region_complete[i]) continue;

        int rcount = region_count_arr[i];
        if (rcount <= 0) continue;

        int start = region_start[i];
        for (int e = start; e < start + rcount; e++) {
          if (entry_active[e]) {
            rmm->abort_prepared_eviction(&entries[e]);
            entry_active[e] = false;
            incomplete_guarded_entries++;
          }
        }
        incomplete_guarded_regions++;
      }
      if (incomplete_guarded_regions > 0) {
        log_info(gc)("Pre-E incomplete-region guard: kept %d partial regions local "
                     "and skipped backend send for %d prepared entries",
                     incomplete_guarded_regions, incomplete_guarded_entries);
      }

      // E1.6: Guard regions that still have LOCAL handles not covered by the
      // prepared object list. If such a region is sent/finalized anyway, the
      // prepared objects are fillerized while the region remains mapped for
      // the leftover LOCAL handles. Missed clean refs then observe
      // FillerElement objects instead of trapping on a protected page.
      int handle_guarded_regions = 0;
      int handle_guarded_entries = 0;
      int handle_blockers = 0;
      int* handle_blockers_by_region = NEW_C_HEAP_ARRAY(int, num_regions, mtGC);
      rmm->count_unprepared_local_handles_in_regions(eviction_candidates,
                                                     region_complete,
                                                     region_start,
                                                     region_count_arr,
                                                     num_regions,
                                                     entries,
                                                     handle_blockers_by_region,
                                                     4);
      for (uint i = 0; i < num_regions; i++) {
        if (!eviction_candidates[i]) continue;
        if (!region_complete[i]) continue;

        int rcount = region_count_arr[i];
        if (rcount <= 0) continue;

        HeapRegion* hr = _g1h->region_at(i);
        int start = region_start[i];
        int blockers = handle_blockers_by_region[i];
        if (blockers == 0) continue;

        for (int e = start; e < start + rcount; e++) {
          if (entry_active[e]) {
            rmm->abort_prepared_eviction(&entries[e]);
            entry_active[e] = false;
            handle_guarded_entries++;
          }
        }

        eviction_candidates[i] = false;
        region_complete[i] = false;
        region_count_arr[i] = 0;
        hr->clear_cold_destination();
        regions_kept_alive++;
        total_candidates--;
        handle_guarded_regions++;
        handle_blockers += blockers;
        log_warning(gc)("Pre-E local-handle guard: removed candidate region %u "
                        "with %d unprepared LOCAL handles before backend send",
                        hr->hrm_index(), blockers);
      }
      FREE_C_HEAP_ARRAY(int, handle_blockers_by_region);
      if (handle_guarded_regions > 0) {
        log_info(gc)("Pre-E local-handle guard: removed %d regions, aborted %d "
                     "prepared entries, found %d blocking LOCAL handles",
                     handle_guarded_regions, handle_guarded_entries,
                     handle_blockers);
      }

      // E1.7: Finish preparation only for entries that survived late guards.
      // Building edge tables before the local-handle guard made each rejected
      // dense region unwind tens of thousands of edge tables during STW.
      Ticks e1_7_start = Ticks::now();
      RemoteHandleAllocBuffer finish_hab;
      bool* finish_failed_region_set = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
      memset(finish_failed_region_set, 0, num_regions * sizeof(bool));
      int finish_success_entries = 0;
      int finish_failed_entries = 0;
      int finish_failed_regions = 0;
      int finish_failed_aborted = 0;
      for (int e = 0; e < num_entries; e++) {
        if (!entry_active[e]) continue;
        HeapRegion* hr = _g1h->heap_region_containing(entries[e].obj);
        if (hr == nullptr) {
          entry_active[e] = false;
          finish_failed_entries++;
          continue;
        }
        uint idx = hr->hrm_index();
        if (idx >= num_regions || finish_failed_region_set[idx]) {
          entry_active[e] = false;
          continue;
        }
        if (rmm->finish_prepared_eviction(&entries[e], &finish_hab)) {
          finish_success_entries++;
        } else {
          finish_failed_region_set[idx] = true;
          entry_active[e] = false;
          finish_failed_entries++;
        }
      }
      for (uint i = 0; i < num_regions; i++) {
        if (!finish_failed_region_set[i]) continue;
        if (!eviction_candidates[i]) continue;

        int start = region_start[i];
        int rcount = region_count_arr[i];
        for (int e = start; e < start + rcount; e++) {
          if (entry_active[e]) {
            rmm->abort_prepared_eviction(&entries[e]);
            entry_active[e] = false;
            finish_failed_aborted++;
          }
        }

        HeapRegion* hr = _g1h->region_at(i);
        eviction_candidates[i] = false;
        region_complete[i] = false;
        region_count_arr[i] = 0;
        hr->clear_cold_destination();
        regions_kept_alive++;
        total_candidates--;
        finish_failed_regions++;
      }
      FREE_C_HEAP_ARRAY(bool, finish_failed_region_set);
      double e1_7_ms = (Ticks::now() - e1_7_start).seconds() * 1000.0;
      e1_ms += e1_7_ms;
      G1RemoteMemoryManager::log_prepare_eviction_stats();
      log_info(gc)("Phase E finish prepare: %.1fms (%d entries finished, %d failed, "
                   "%d regions removed, %d finished entries aborted)",
                   e1_7_ms, finish_success_entries, finish_failed_entries,
                   finish_failed_regions, finish_failed_aborted);

      // E2: Batch-send to remote backend.
      Ticks e2_start = Ticks::now();
      G1RemoteBackend* backend = rmm->backend();
      int batches_sent = 0;

      // CMD_BATCH_EVICT_WITH_EDGES format:
      // header(24) + N × [slot_id(8) + handle_id(8) + klass(8) + word_size(4) +
      //                    num_edges(4) + obj_bytes(ws*8) + edges(num_edges*12)]
      static const size_t BATCH_HDR_SIZE = 24;
      static const size_t LOCAL_BATCH_BUF_SIZE = 4 * 1024 * 1024;
      const bool use_batch_evict = backend->supports_batch_evict();
      const size_t batch_buf_size =
          MIN2(LOCAL_BATCH_BUF_SIZE, backend->max_batch_evict_message_size());
      const size_t max_entry_payload =
          use_batch_evict && batch_buf_size > BATCH_HDR_SIZE ?
          (batch_buf_size - BATCH_HDR_SIZE) : (size_t)-1;

      int* message_blockers_by_region = NEW_C_HEAP_ARRAY(int, num_regions, mtGC);
      memset(message_blockers_by_region, 0, num_regions * sizeof(int));
      int message_guarded_regions = 0;
      int message_guarded_entries = 0;
      int oversized_entries = 0;
      size_t largest_oversized_entry = 0;
      for (int e = 0; e < num_entries; e++) {
        if (!entry_active[e]) continue;
        PreparedEviction* pe = &entries[e];
        size_t byte_size = pe->word_size * HeapWordSize;
        uint32_t num_edges = (pe->edge_table != nullptr) ? pe->edge_table->_entry_count : 0;
        size_t edge_bytes = num_edges * 12;
        size_t entry_size = 32 + byte_size + edge_bytes;
        if (entry_size > max_entry_payload) {
          HeapRegion* hr = _g1h->heap_region_containing(pe->obj);
          uint idx = hr->hrm_index();
          if (idx < num_regions) {
            message_blockers_by_region[idx]++;
          }
          oversized_entries++;
          largest_oversized_entry = MAX2(largest_oversized_entry, entry_size);
        }
      }
      for (uint i = 0; i < num_regions; i++) {
        if (message_blockers_by_region[i] == 0) continue;
        if (!eviction_candidates[i]) continue;

        int start = region_start[i];
        int rcount = region_count_arr[i];
        for (int e = start; e < start + rcount; e++) {
          if (entry_active[e]) {
            rmm->abort_prepared_eviction(&entries[e]);
            entry_active[e] = false;
            message_guarded_entries++;
          }
        }

        HeapRegion* hr = _g1h->region_at(i);
        eviction_candidates[i] = false;
        region_complete[i] = false;
        region_count_arr[i] = 0;
        hr->clear_cold_destination();
        regions_kept_alive++;
        total_candidates--;
        message_guarded_regions++;
      }
      FREE_C_HEAP_ARRAY(int, message_blockers_by_region);
      if (message_guarded_regions > 0) {
        log_warning(gc)("Pre-E message-size guard: removed %d regions, aborted %d "
                        "prepared entries, found %d oversized entries "
                        "(largest=" SIZE_FORMAT "KB, limit=" SIZE_FORMAT "KB)",
                        message_guarded_regions, message_guarded_entries,
                        oversized_entries, largest_oversized_entry / K,
                        max_entry_payload / K);
      }

      bool* send_failed_regions = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
      memset(send_failed_regions, 0, num_regions * sizeof(bool));
      int send_failed_entries = 0;
      int send_failed_sent_entries = 0;
      static const int FAILED_LOCALIZE_BATCH = 8192;
      uintptr_t* failed_localize_ids =
          NEW_C_HEAP_ARRAY(uintptr_t, FAILED_LOCALIZE_BATCH, mtGC);

      if (num_entries > 0 && use_batch_evict && batch_buf_size > BATCH_HDR_SIZE) {
        uint8_t* batch_buf = (uint8_t*)os::malloc(batch_buf_size, mtGC);
        size_t batch_offset = BATCH_HDR_SIZE;
        int batch_count = 0;
        int batch_start_entry = 0;
        bool backend_send_failed = false;

        if (batch_buf == nullptr) {
          log_warning(gc)("Pre-E batch send skipped: failed to allocate " SIZE_FORMAT
                          "KB batch buffer; keeping prepared entries local",
                          batch_buf_size / K);
          for (uint i = 0; i < num_regions; i++) {
            if (eviction_candidates[i] && region_count_arr[i] > 0) {
              send_failed_regions[i] = true;
            }
          }
        } else {
          for (int e = 0; e < num_entries; e++) {
            if (!entry_active[e]) continue;
            PreparedEviction* pe = &entries[e];
            size_t byte_size = pe->word_size * HeapWordSize;
            uint32_t num_edges = (pe->edge_table != nullptr) ? pe->edge_table->_entry_count : 0;
            size_t edge_bytes = num_edges * 12;
            size_t entry_size = 32 + byte_size + edge_bytes;

            if (entry_size > max_entry_payload) {
              // Guard should have removed the containing region before E2.
              rmm->abort_prepared_eviction(pe);
              entry_active[e] = false;
              continue;
            }

            if (batch_count == 0) {
              batch_start_entry = e;
            }

            if (batch_offset + entry_size > batch_buf_size && batch_count > 0) {
              *(uint32_t*)(batch_buf + 0) = 0x16; // CMD_BATCH_EVICT_WITH_EDGES
              *(uint32_t*)(batch_buf + 4) = (uint32_t)batch_offset;
              *(uint64_t*)(batch_buf + 8) = 0;
              *(uint32_t*)(batch_buf + 16) = (uint32_t)batch_count;
              int rc = backend->batch_evict(batch_buf, batch_offset);
              if (rc < 0) {
                log_warning(gc)("Pre-E batch send failed for %d objects (" SIZE_FORMAT "KB); "
                                "keeping affected regions local",
                                batch_count, batch_offset / K);
                for (int f = batch_start_entry; f < e; f++) {
                  if (!entry_active[f]) continue;
                  HeapRegion* hr = _g1h->heap_region_containing(entries[f].obj);
                  uint idx = hr->hrm_index();
                  if (idx < num_regions) send_failed_regions[idx] = true;
                  send_failed_entries++;
                }
                for (int f = e; f < num_entries; f++) {
                  if (!entry_active[f]) continue;
                  HeapRegion* hr = _g1h->heap_region_containing(entries[f].obj);
                  uint idx = hr->hrm_index();
                  if (idx < num_regions) send_failed_regions[idx] = true;
                  send_failed_entries++;
                }
                backend_send_failed = true;
                break;
              } else {
                for (int f = batch_start_entry; f < e; f++) {
                  if (entry_active[f]) entry_sent[f] = true;
                }
              }
              batches_sent++;
              batch_offset = BATCH_HDR_SIZE;
              batch_count = 0;
              batch_start_entry = e;
            }

            *(uint64_t*)(batch_buf + batch_offset)      = (uint64_t)pe->slot_id;
            *(uint64_t*)(batch_buf + batch_offset + 8)   = (uintptr_t)pe->handle;
            *(uint64_t*)(batch_buf + batch_offset + 16)  = (uint64_t)(uintptr_t)pe->klass;
            *(uint32_t*)(batch_buf + batch_offset + 24)  = (uint32_t)pe->word_size;
            *(uint32_t*)(batch_buf + batch_offset + 28)  = num_edges;
            memcpy(batch_buf + batch_offset + 32, cast_from_oop<void*>(pe->obj), byte_size);
            {
              uintptr_t* mw_in_buf = (uintptr_t*)(batch_buf + batch_offset + 32);
              markWord mw(*mw_in_buf);
              if (!mw.is_unlocked()) {
                *mw_in_buf = markWord::prototype().value();
              }
            }
            uint8_t* edge_ptr = batch_buf + batch_offset + 32 + byte_size;
            if (pe->edge_table != nullptr) {
              for (uint32_t j = 0; j < num_edges; j++) {
                *(uint32_t*)(edge_ptr)     = pe->edge_table->_entries[j]._field_offset;
                *(uint64_t*)(edge_ptr + 4) = (uintptr_t)pe->edge_table->_entries[j]._target_handle;
                edge_ptr += 12;
              }
            }
            batch_offset += entry_size;
            batch_count++;
          }

          if (!backend_send_failed && batch_count > 0) {
            *(uint32_t*)(batch_buf + 0) = 0x16; // CMD_BATCH_EVICT_WITH_EDGES
            *(uint32_t*)(batch_buf + 4) = (uint32_t)batch_offset;
            *(uint64_t*)(batch_buf + 8) = 0;
            *(uint32_t*)(batch_buf + 16) = (uint32_t)batch_count;
            int rc = backend->batch_evict(batch_buf, batch_offset);
            if (rc < 0) {
              log_warning(gc)("Pre-E batch send failed for %d objects (" SIZE_FORMAT "KB); "
                              "keeping affected regions local",
                              batch_count, batch_offset / K);
              for (int f = batch_start_entry; f < num_entries; f++) {
                if (!entry_active[f]) continue;
                HeapRegion* hr = _g1h->heap_region_containing(entries[f].obj);
                uint idx = hr->hrm_index();
                if (idx < num_regions) send_failed_regions[idx] = true;
                send_failed_entries++;
              }
            } else {
              for (int f = batch_start_entry; f < num_entries; f++) {
                if (entry_active[f]) entry_sent[f] = true;
              }
            }
            batches_sent++;
          }

          os::free(batch_buf);
        }
      } else if (num_entries > 0 && use_batch_evict) {
        log_warning(gc)("Pre-E batch send skipped: backend batch message limit "
                        SIZE_FORMAT "B is too small; keeping prepared entries local",
                        batch_buf_size);
        for (uint i = 0; i < num_regions; i++) {
          if (eviction_candidates[i] && region_count_arr[i] > 0) {
            send_failed_regions[i] = true;
          }
        }
      } else if (num_entries > 0 && !use_batch_evict) {
        for (int e = 0; e < num_entries; e++) {
          if (!entry_active[e]) continue;
          PreparedEviction* pe = &entries[e];
          uint32_t num_edges = (pe->edge_table != nullptr) ? pe->edge_table->_entry_count : 0;
          G1RemoteBackend::EdgeInfo* edge_infos = nullptr;
          if (num_edges > 0) {
            edge_infos = NEW_C_HEAP_ARRAY(G1RemoteBackend::EdgeInfo, num_edges, mtGC);
            for (uint32_t j = 0; j < num_edges; j++) {
              edge_infos[j].field_offset = pe->edge_table->_entries[j]._field_offset;
              edge_infos[j].target_handle_id =
                  (uintptr_t)pe->edge_table->_entries[j]._target_handle;
            }
          }

          size_t sid = backend->evict_with_edges(cast_from_oop<void*>(pe->obj),
                                                 pe->word_size,
                                                 pe->klass,
                                                 (uintptr_t)pe->handle,
                                                 edge_infos,
                                                 num_edges,
                                                 pe->slot_id);
          if (edge_infos != nullptr) {
            FREE_C_HEAP_ARRAY(G1RemoteBackend::EdgeInfo, edge_infos);
          }
          if (sid == (size_t)-1) {
            HeapRegion* hr = _g1h->heap_region_containing(pe->obj);
            uint idx = hr->hrm_index();
            if (idx < num_regions) send_failed_regions[idx] = true;
            send_failed_entries++;
          } else {
            entry_sent[e] = true;
          }
        }
      }

      int send_failed_region_count = 0;
      int send_failed_aborted = 0;
      int failed_localize_count = 0;
      for (uint i = 0; i < num_regions; i++) {
        if (!send_failed_regions[i]) continue;
        if (!eviction_candidates[i]) continue;

        int start = region_start[i];
        int rcount = region_count_arr[i];
        for (int e = start; e < start + rcount; e++) {
          if (entry_active[e]) {
            if (entry_sent[e]) {
              failed_localize_ids[failed_localize_count++] = (uintptr_t)entries[e].handle;
              send_failed_sent_entries++;
              entry_sent[e] = false;
              if (failed_localize_count == FAILED_LOCALIZE_BATCH) {
                backend->localize_batch(failed_localize_ids, failed_localize_count);
                failed_localize_count = 0;
              }
            }
            rmm->abort_prepared_eviction(&entries[e]);
            entry_active[e] = false;
            send_failed_aborted++;
          }
        }

        HeapRegion* hr = _g1h->region_at(i);
        eviction_candidates[i] = false;
        region_complete[i] = false;
        region_count_arr[i] = 0;
        hr->clear_cold_destination();
        regions_kept_alive++;
        total_candidates--;
        send_failed_region_count++;
      }
      if (failed_localize_count > 0) {
        backend->localize_batch(failed_localize_ids, failed_localize_count);
      }
      FREE_C_HEAP_ARRAY(bool, send_failed_regions);
      FREE_C_HEAP_ARRAY(uintptr_t, failed_localize_ids);
      if (send_failed_region_count > 0) {
        log_warning(gc)("Pre-E batch failure guard: removed %d regions, aborted %d "
                        "prepared entries (%d send-failed entries observed, "
                        "%d already-sent entries localized)",
                        send_failed_region_count, send_failed_aborted,
                        send_failed_entries, send_failed_sent_entries);
      }
      double e2_ms = (Ticks::now() - e2_start).seconds() * 1000.0;

      // E3: Finalize complete regions' evictions + free them.
      // Incomplete regions (where prepare_eviction failed for some objects)
      // must NOT be freed — their LOCAL handles still point into the region.
      Ticks e3_start = Ticks::now();

      for (uint i = 0; i < num_regions; i++) {
        if (!eviction_candidates[i]) continue;
        HeapRegion* hr = _g1h->region_at(i);
        int rcount = region_count_arr[i];

        if (rcount > 0 && region_complete[i]) {
          int start = region_start[i];
          for (int e = start; e < start + rcount; e++) {
            rmm->finalize_eviction(&entries[e]);
          }

          int remaining_local_handles = rmm->count_local_handles_in_region(hr, 4);
          if (remaining_local_handles > 0) {
            log_warning(gc)("Region %u NOT freed after eviction: %d LOCAL handles "
                            "still point into [" PTR_FORMAT ", " PTR_FORMAT ")",
                            hr->hrm_index(), remaining_local_handles,
                            p2i(hr->bottom()), p2i(hr->end()));
            regions_kept_alive++;
            hr->clear_cold_destination();
            continue;
          }

          total_evicted += rcount;
          regions_evicted++;
          size_t region_used = hr->used();
          total_freed_bytes += region_used;
          rmm->invalidate_fcr_if_freed(hr);
          log_info(gc)("Evicted region %u (%d objects, " SIZE_FORMAT "KB) "
                       "[" PTR_FORMAT ", " PTR_FORMAT ")",
                       hr->hrm_index(), rcount, region_used / K,
                       p2i(hr->bottom()), p2i(hr->top()));
          hr->clear_cardtable();
          _g1h->free_region(hr, &freed_list);
          ::madvise((char*)hr->bottom(), HeapRegion::GrainBytes, MADV_DONTNEED);
          os::guard_memory((char*)hr->bottom(), HeapRegion::GrainBytes);
          hr->set_evict_guarded();
          freed_regions++;
        } else if (rcount > 0 && !region_complete[i]) {
          log_info(gc)("Region %u kept alive: %d objects prepared but some failed "
                       "prepare_eviction — handles stay LOCAL",
                       hr->hrm_index(), rcount);
          regions_kept_alive++;
          hr->clear_cold_destination();
        } else {
          hr->clear_cold_destination();
        }
      }
      double e3_ms = (Ticks::now() - e3_start).seconds() * 1000.0;

      FREE_C_HEAP_ARRAY(PreparedEviction, entries);
      FREE_C_HEAP_ARRAY(int, region_start);
      FREE_C_HEAP_ARRAY(int, region_count_arr);
      FREE_C_HEAP_ARRAY(bool, region_complete);
      FREE_C_HEAP_ARRAY(bool, entry_active);
      FREE_C_HEAP_ARRAY(bool, entry_sent);

      if (total_candidates > 0) {
        double phase_e_ms = (Ticks::now() - phase_e_start).seconds() * 1000.0;
        log_info(gc)("Phase E eviction: %.1fms (E1=%.1fms E1.5=%.1fms E2=%.1fms/%d batches E3=%.1fms) "
                     "(%d objects, %d regions)",
                     phase_e_ms, e1_ms, e1_5_ms, e2_ms, batches_sent, e3_ms,
                     total_evicted, regions_evicted);
      }
      }
    }

    // Post-eviction diagnostic: verify no root oops point into freed regions.
    // Uses eviction_candidates boolean array (independent of region state).
    if (regions_evicted > 0) {
      class VerifyNoRootToFreedClosure : public OopClosure {
        G1CollectedHeap* _g1h;
        bool* _freed_set;
        uint _num_regions;
        int _bad;
      public:
        VerifyNoRootToFreedClosure(G1CollectedHeap* g1h, bool* fset, uint n)
          : _g1h(g1h), _freed_set(fset), _num_regions(n), _bad(0) {}
        void do_oop(oop* p) {
          uintptr_t raw = *(uintptr_t*)p;
          if (raw == 0) return;
          oop obj;
          if ((raw >> 63) != 0) {
            if (raw & G1_OOP_INDIRECT_BIT) return; // Shared → Handle
            obj = (oop)(raw & G1_OOP_ADDR_MASK); // Unique → strip
          } else {
            obj = (oop)raw;
          }
          if (!_g1h->is_in(obj)) return;
          HeapRegion* hr = _g1h->heap_region_containing(obj);
          uint idx = hr->hrm_index();
          if (idx < _num_regions && _freed_set[idx]) {
            _bad++;
            log_warning(gc)("POST-EVICTION ROOT DANGLE: root_slot=" PTR_FORMAT
                            " -> obj=" PTR_FORMAT " in freed region %u raw=0x%lx",
                            p2i(p), p2i((void*)obj), idx, (unsigned long)raw);
          }
        }
        void do_oop(narrowOop* p) {}
        int bad() const { return _bad; }
      };
      VerifyNoRootToFreedClosure vr(_g1h, eviction_candidates, num_regions);
      Threads::oops_do(&vr, nullptr);
      JNIHandles::oops_do(&vr);
      OopStorageSet::strong_oops_do(&vr);
      for (auto id : EnumRange<OopStorageSet::WeakId>()) {
        OopStorageSet::storage(id)->oops_do(&vr);
      }
      {
        CLDToOopClosure cld_cl(&vr, ClassLoaderData::_claim_none);
        ClassLoaderDataGraph::cld_do(&cld_cl);
      }
      {
        CodeBlobToOopClosure code_cl(&vr, false);
        CodeCache::blobs_do(&code_cl);
      }
      _g1h->ref_processor_cm()->weak_oops_do(&vr);
      // Skip oops_do_remote_anchors: handles for objects in eviction candidates
      // naturally point into those regions (that's the eviction infrastructure).
      if (vr.bad() > 0) {
        log_warning(gc)("POST-EVICTION: %d root oops point into %d freed regions!",
                        vr.bad(), regions_evicted);
      }
    }

    if (regions_evicted > 0) {
      rmm->purge_stale_local_handles("POST-REMOTE-EVICTION-HANDLE-SWEEP", 16);
    }

    if (G1VerifyAfterEviction && regions_evicted > 0) {
      log_info(gc)("G1VerifyAfterEviction: running full heap+root sweep for stale refs...");
      int stale = rmm->verify_no_stale_refs_to_freed_regions();
      if (stale > 0) {
        log_warning(gc)("G1VerifyAfterEviction: found %d stale references into freed/guarded regions!", stale);
      } else {
        log_info(gc)("G1VerifyAfterEviction: sweep clean — no stale refs detected.");
      }
    }

    FREE_C_HEAP_ARRAY(bool, eviction_candidates);

    // Return freed regions to the free pool
    if (freed_regions > 0) {
      _g1h->remove_from_old_gen_sets(freed_regions, 0);
      _g1h->prepend_to_freelist(&freed_list);
      _g1h->decrement_summary_bytes(total_freed_bytes);
    }

    if (total_evicted > 0 || regions_pinned > 0) {
      log_info(gc)("Remote eviction: %d objects in %d regions evicted (" SIZE_FORMAT "KB freed), "
                   "%d regions pinned (total evicted: " SIZE_FORMAT ", total fetched: " SIZE_FORMAT ")",
                   total_evicted, regions_evicted, total_freed_bytes / K,
                   regions_pinned,
                   rmm->backend()->total_evicted(), rmm->backend()->total_fetched());
    }
    rmm->log_remote_access_stats();
    } // end else (not concurrent start)
  }

  _g1h->rebuild_free_region_list();

  if (LocalMemoryRatio < 100) {
    trim_free_region_rss(_g1h);
  }

  _g1h->record_obj_copy_mem_stats();

  evacuation_info->set_bytes_used(_g1h->bytes_used_during_gc());

  _g1h->prepare_for_mutator_after_young_collection();

  _g1h->gc_epilogue(false);

  _g1h->expand_heap_after_young_collection();
}

bool G1YoungCollector::evacuation_failed() const {
  return _evac_failure_regions.evacuation_failed();
}

G1YoungCollector::G1YoungCollector(GCCause::Cause gc_cause) :
  _g1h(G1CollectedHeap::heap()),
  _gc_cause(gc_cause),
  _concurrent_operation_is_full_mark(false),
  _evac_failure_regions(),
  _pre_evac_tops(nullptr)
{
}

void G1YoungCollector::collect() {
  // Do timing/tracing/statistics/pre- and post-logging/verification work not
  // directly related to the collection. They should not be accounted for in
  // collection work timing.

  // The G1YoungGCTraceTime message depends on collector state, so must come after
  // determining collector state.
  G1YoungGCTraceTime tm(this, _gc_cause);

  // JFR
  G1YoungGCJFRTracerMark jtm(gc_timer_stw(), gc_tracer_stw(), _gc_cause);
  // JStat/MXBeans
  G1YoungGCMonitoringScope ms(monitoring_support(),
                              !collection_set()->candidates()->is_empty() /* all_memory_pools_affected */);
  // Create the heap printer before internal pause timing to have
  // heap information printed as last part of detailed GC log.
  G1HeapPrinterMark hpm(_g1h);
  // Young GC internal pause timing
  G1YoungGCNotifyPauseMark npm(this);

  // Verification may use the workers, so they must be set up before.
  // Individual parallel phases may override this.
  set_young_collection_default_active_worker_threads();

  // Wait for root region scan here to make sure that it is done before any
  // use of the STW workers to maximize cpu use (i.e. all cores are available
  // just to do that).
  log_trace(gc)("DIAG: wait_for_root_region_scanning START");
  wait_for_root_region_scanning();
  log_trace(gc)("DIAG: wait_for_root_region_scanning DONE");

  G1YoungGCVerifierMark vm(this);
  {
    // Actual collection work starts and is executed (only) in this scope.

    // Young GC internal collection timing. The elapsed time recorded in the
    // policy for the collection deliberately elides verification (and some
    // other trivial setup above).
    policy()->record_young_collection_start();

    // Increment hotness epoch for recency tracking.
    _g1h->remote_memory_manager()->increment_gc_epoch();

    log_trace(gc)("DIAG: pre_evacuate_collection_set START");
    pre_evacuate_collection_set(jtm.evacuation_info());
    log_trace(gc)("DIAG: pre_evacuate_collection_set DONE");

    // Save region tops before evacuation for fast Phase C destination scan.
    uint num_regions = _g1h->num_regions();
    _pre_evac_tops = NEW_C_HEAP_ARRAY(HeapWord*, num_regions, mtGC);
    for (uint i = 0; i < num_regions; i++) {
      HeapRegion* hr = _g1h->region_at(i);
      _pre_evac_tops[i] = hr->top();
    }

    G1ParScanThreadStateSet per_thread_states(_g1h,
                                              workers()->active_workers(),
                                              collection_set(),
                                              &_evac_failure_regions);

    bool may_do_optional_evacuation = collection_set()->optional_region_length() != 0;
    // Actually do the work...
    log_trace(gc)("DIAG: evacuate_initial_collection_set START");
    evacuate_initial_collection_set(&per_thread_states, may_do_optional_evacuation);
    log_trace(gc)("DIAG: evacuate_initial_collection_set DONE");

    if (may_do_optional_evacuation) {
      log_trace(gc)(">>> evacuate_optional START");
      evacuate_optional_collection_set(&per_thread_states);
      log_trace(gc)(">>> evacuate_optional DONE");
    }
    log_trace(gc)(">>> post_evacuate_collection_set START");
    post_evacuate_collection_set(jtm.evacuation_info(), &per_thread_states);
    log_trace(gc)(">>> post_evacuate_collection_set DONE");
    FREE_C_HEAP_ARRAY(HeapWord*, _pre_evac_tops);
    _pre_evac_tops = nullptr;

    // Refine the type of a concurrent mark operation now that we did the
    // evacuation, eventually aborting it.
    _concurrent_operation_is_full_mark = policy()->concurrent_operation_is_full_mark("Revise IHOP");

    // Need to report the collection pause now since record_collection_pause_end()
    // modifies it to the next state.
    jtm.report_pause_type(collector_state()->young_gc_pause_type(_concurrent_operation_is_full_mark));

    policy()->record_young_collection_end(_concurrent_operation_is_full_mark, evacuation_failed());
  }
  TASKQUEUE_STATS_ONLY(_g1h->task_queues()->print_and_reset_taskqueue_stats("Oop Queue");)
}

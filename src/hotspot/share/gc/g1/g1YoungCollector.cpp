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

#include "classfile/classLoaderDataGraph.inline.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "compiler/oopMap.hpp"
#include "gc/g1/g1Allocator.hpp"
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
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RemoteBackend.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/g1/g1RedirtyCardsQueue.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1RootProcessor.hpp"
#include "gc/g1/g1Trace.hpp"
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
#include "runtime/jniHandles.hpp"
#include "runtime/os.hpp"
#include "runtime/threads.hpp"
#include "utilities/ticks.hpp"

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
            h->set_local_release((void*)cast_from_oop<uintptr_t>(forwardee));
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
    Ticks start = Ticks::now();
    rem_set()->merge_heap_roots(true /* initial_evacuation */);
    p->record_merge_heap_roots_time((Ticks::now() - start).seconds() * 1000.0);
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
    task_time = run_task_timed(&g1_par_task);
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
};
static int rpe_cmp(const void* a, const void* b) {
  uintptr_t aa = ((const RootPinEntry*)a)->obj_addr;
  uintptr_t bb = ((const RootPinEntry*)b)->obj_addr;
  return (aa < bb) ? -1 : (aa > bb) ? 1 : 0;
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

void G1YoungCollector::post_evacuate_collection_set(G1EvacInfo* evacuation_info,
                                                    G1ParScanThreadStateSet* per_thread_states) {
  G1GCPhaseTimes* p = phase_times();

  // Capture heap usage before cleanup for eviction threshold check.
  // After cleanup, used() drops (young regions reclaimed) and may fall
  // below threshold even when Old gen pressure warrants eviction.
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
    if (rmm != nullptr && rmm->tagged_field_count() > 0) {
      rmm->fixup_tagged_field_handles();
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
  post_evacuate_cleanup_1(per_thread_states);
  log_trace(gc)(">>>   post_evacuate_cleanup_1 DONE");

  log_trace(gc)(">>>   post_evacuate_cleanup_2 START");
  post_evacuate_cleanup_2(per_thread_states, evacuation_info);
  log_trace(gc)(">>>   post_evacuate_cleanup_2 DONE");

  _evac_failure_regions.post_collection();

  assert_used_and_recalculate_used_equal(_g1h);

  // Remote collection: free dead remote objects without fetching.
  // "Garbage never crosses the network" — dispatches to the selected backend
  // (sim, TCP executor, or RDMA executor) via G1RemoteMemoryManager.
  {
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    if (rmm != nullptr) {
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
  if (G1RemoteEvictionThreshold > 0 || G1SimulateRemoteEviction) {
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

    // Path 2: existing old regions above threshold
    if (G1RemoteEvictionThreshold > 0) {
      size_t heap_capacity = _g1h->max_capacity();
      size_t threshold_bytes = (heap_capacity * G1RemoteEvictionThreshold) / 100;

      if (pre_cleanup_heap_used > threshold_bytes) {
        size_t to_free = pre_cleanup_heap_used - threshold_bytes;
        log_info(gc)("Path 2 eviction: heap " SIZE_FORMAT "MB / " SIZE_FORMAT "MB "
                     "(threshold %u%% = " SIZE_FORMAT "MB), need " SIZE_FORMAT "KB",
                     pre_cleanup_heap_used / M, heap_capacity / M,
                     G1RemoteEvictionThreshold, threshold_bytes / M, to_free / K);

        size_t path2_bytes = 0;
        for (uint i = 0; i < num_regions && path2_bytes < to_free; i++) {
          HeapRegion* hr = _g1h->region_at(i);
          if (!hr->is_old() || hr->is_humongous() || hr->is_empty()) continue;
          if (hr->is_cold_destination() || hr->is_fetch_cache()) continue;
          if (eviction_candidates[i]) continue;

          hr->set_cold_destination();
          eviction_candidates[i] = true;
          path2_candidates++;
          path2_bytes += hr->used();
        }
      }
    }

    int total_candidates = path1_candidates + path2_candidates;

    // ---- Phase D: Root-catch relocation ----
    // Instead of pinning entire 16MB regions for a few root-referenced
    // objects (often just a Double or small array), relocate root-pinned
    // objects to a dedicated "root-catch" region. This allows eviction of
    // regions that previously had 1-2 root refs blocking them.
    if (total_candidates > 0) {
      const int max_pins = 8192;
      RootPinEntry* pins = NEW_C_HEAP_ARRAY(RootPinEntry, max_pins, mtGC);
      int num_pins = 0;

      class EvictionRootCollectClosure : public OopClosure {
        G1CollectedHeap* _g1h;
        const bool*      _eviction_candidates;
        uint             _num_regions;
        RootPinEntry*    _pins;
        int&             _num_pins;
        int              _max_pins;
      public:
        EvictionRootCollectClosure(G1CollectedHeap* g1h, const bool* candidates,
                                   uint num_regions, RootPinEntry* pins,
                                   int& num_pins, int max_pins)
          : _g1h(g1h), _eviction_candidates(candidates), _num_regions(num_regions),
            _pins(pins), _num_pins(num_pins), _max_pins(max_pins) {}
        void do_oop(oop* p) {
          oop obj = *p;
          if (obj == nullptr) return;
          uintptr_t raw = cast_from_oop<uintptr_t>(obj);
          if ((raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
              (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) return;
          if ((raw & G1_OOP_TAG_MASK) != 0) {
            obj = cast_to_oop(raw & G1_OOP_ADDR_MASK);
          }
          if (!_g1h->is_in(obj)) return;
          HeapRegion* hr = _g1h->heap_region_containing(obj);
          if (hr == nullptr) return;
          uint idx = hr->hrm_index();
          if (idx < _num_regions && _eviction_candidates[idx] && _num_pins < _max_pins) {
            _pins[_num_pins] = {p, cast_from_oop<uintptr_t>(obj)};
            _num_pins++;
          }
        }
        void do_oop(narrowOop* p) { /* UseCompressedOops=false */ }
      };

      EvictionRootCollectClosure collect_cl(_g1h, eviction_candidates, num_regions,
                                             pins, num_pins, max_pins);
      Threads::oops_do(&collect_cl, nullptr);
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
      rmm->oops_do_remote_anchors(&collect_cl);
      rmm->oops_do_remote_cross_roots(&collect_cl);

      // Relocate collected root-pinned objects to a catch region
      if (num_pins > 0) {
        qsort(pins, num_pins, sizeof(RootPinEntry), rpe_cmp);

        HeapRegion* root_catch = _g1h->allocate_fcr_region();
        if (root_catch == nullptr) {
          // Fallback: pin regions the old way
          for (int i = 0; i < num_pins; i++) {
            oop obj = cast_to_oop(pins[i].obj_addr);
            if (!_g1h->is_in(obj)) continue;
            HeapRegion* hr = _g1h->heap_region_containing(obj);
            uint idx = hr->hrm_index();
            if (idx < num_regions && eviction_candidates[idx]) {
              eviction_candidates[idx] = false;
              regions_pinned++;
              total_candidates--;
            }
          }
          log_warning(gc)("Root-catch: cannot allocate catch region, pinned %d regions", regions_pinned);
        } else {
          HeapWord* catch_top = root_catch->bottom();
          int relocated = 0;
          int roots_updated = 0;
          int fallback_pinned = 0;

          uintptr_t prev_addr = 0;
          HeapWord* prev_new = nullptr;

          for (int i = 0; i < num_pins; i++) {
            uintptr_t obj_addr = pins[i].obj_addr;
            oop* root_p = pins[i].root;

            if (obj_addr == prev_addr && prev_new != nullptr) {
              // Duplicate root to same object — update this root too
              uintptr_t raw = cast_from_oop<uintptr_t>(*root_p);
              uintptr_t tags = raw & G1_OOP_TAG_MASK;
              *root_p = cast_to_oop(tags | ((uintptr_t)prev_new & G1_OOP_ADDR_MASK));
              roots_updated++;
              continue;
            }

            oop obj = cast_to_oop(obj_addr);
            if (obj->is_forwarded()) {
              // Already relocated by an earlier entry — update root to forwardee
              oop fwd = obj->forwardee();
              uintptr_t raw = cast_from_oop<uintptr_t>(*root_p);
              uintptr_t tags = raw & G1_OOP_TAG_MASK;
              *root_p = cast_to_oop(tags | (cast_from_oop<uintptr_t>(fwd) & G1_OOP_ADDR_MASK));
              prev_addr = obj_addr;
              prev_new = cast_from_oop<HeapWord*>(fwd);
              roots_updated++;
              continue;
            }

            size_t word_sz = obj->size();
            if (catch_top + word_sz > root_catch->end()) {
              // Catch region full — fallback pin for this region
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

            // Copy object to catch region
            Copy::aligned_disjoint_words((HeapWord*)obj_addr, catch_top, word_sz);
            oop new_obj = cast_to_oop(catch_top);
            root_catch->update_bot_for_obj(catch_top, word_sz);

            // Install forwarding pointer in old location (Phase B/C/E check this)
            obj->forward_to(new_obj);

            // Update root (preserve tag bits)
            uintptr_t raw = cast_from_oop<uintptr_t>(*root_p);
            uintptr_t tags = raw & G1_OOP_TAG_MASK;
            *root_p = cast_to_oop(tags | ((uintptr_t)catch_top & G1_OOP_ADDR_MASK));

            // Rekey handle table (old_addr → new_addr)
            rmm->update_handle_for_evacuation(obj, new_obj);

            prev_addr = obj_addr;
            prev_new = catch_top;
            catch_top += word_sz;
            relocated++;
            roots_updated++;
          }

          root_catch->set_top(catch_top);
          if (relocated > 0 || fallback_pinned > 0) {
            log_info(gc)("Root-catch relocation: %d objects (%zuKB) relocated to region %u, "
                         "%d roots updated, %d regions fallback-pinned",
                         relocated,
                         (size_t)(catch_top - root_catch->bottom()) * HeapWordSize / K,
                         root_catch->hrm_index(), roots_updated, fallback_pinned);
          }
          if (catch_top == root_catch->bottom()) {
            FreeRegionList tmp("tmp");
            _g1h->free_region(root_catch, &tmp);
          }
        }
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

      // ---- Phase C: Fast targeted scan — RSet + young + candidate + destination ----
      {
        Ticks phase_c_start = Ticks::now();
        uint nworkers = _g1h->workers()->active_workers();
        rmm->tag_refs_to_eviction_set_fast(eviction_candidates, num_regions,
                                           _pre_evac_tops,
                                           _g1h->workers(), nworkers);
        double phase_c_ms = (Ticks::now() - phase_c_start).seconds() * 1000.0;
        log_info(gc)("Phase C fast scan: %.1fms (%u workers)", phase_c_ms, nworkers);
      }

      // ---- Phase C.5: Verify no untagged refs remain ----
      {
        int missed = rmm->verify_no_untagged_refs_to_eviction_set(eviction_candidates, num_regions);
        if (missed > 0) {
          log_warning(gc)("Eviction ABORTED: %d untagged refs found after tagging", missed);
          for (uint i = 0; i < num_regions; i++) {
            if (eviction_candidates[i]) {
              HeapRegion* hr = _g1h->region_at(i);
              hr->clear_cold_destination();
              eviction_candidates[i] = false;
            }
          }
          total_candidates = 0;
        }
      }

      // ---- Phase E: Evict non-pinned candidates (batched) ----
      {
      Ticks phase_e_start = Ticks::now();

      // E1: Prepare all evictions (build edge tables, assign slot_ids).
      typedef G1RemoteMemoryManager::PreparedEviction PreparedEviction;
      int max_entries = 0;
      for (uint i = 0; i < num_regions; i++) {
        if (!eviction_candidates[i]) continue;
        HeapRegion* hr = _g1h->region_at(i);
        max_entries += (int)((hr->top() - hr->bottom()) / MinObjAlignmentInBytes) + 1;
      }
      if (max_entries > 2 * 1024 * 1024) max_entries = 2 * 1024 * 1024;
      PreparedEviction* entries = NEW_C_HEAP_ARRAY(PreparedEviction, max_entries, mtGC);
      int* region_start = NEW_C_HEAP_ARRAY(int, num_regions, mtGC);
      int* region_count_arr = NEW_C_HEAP_ARRAY(int, num_regions, mtGC);
      bool* region_complete = NEW_C_HEAP_ARRAY(bool, num_regions, mtGC);
      memset(region_start, 0, num_regions * sizeof(int));
      memset(region_count_arr, 0, num_regions * sizeof(int));
      memset(region_complete, 0, num_regions * sizeof(bool));
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
          if (rmm->prepare_eviction(obj, &hab, &entries[num_entries])) {
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
      G1RemoteMemoryManager::log_prepare_eviction_stats();

      // E2: Batch-send to remote backend.
      Ticks e2_start = Ticks::now();
      G1RemoteBackend* backend = rmm->backend();
      int batches_sent = 0;

      if (num_entries > 0) {
        // CMD_BATCH_EVICT_WITH_EDGES format:
        // header(24) + N × [slot_id(8) + handle_id(8) + klass(8) + word_size(4) +
        //                    num_edges(4) + obj_bytes(ws*8) + edges(num_edges*12)]
        static const size_t BATCH_HDR_SIZE = 24;
        static const size_t BATCH_BUF_SIZE = 4 * 1024 * 1024;
        uint8_t* batch_buf = (uint8_t*)os::malloc(BATCH_BUF_SIZE, mtGC);
        size_t batch_offset = BATCH_HDR_SIZE;
        int batch_count = 0;
        int batch_start_entry = 0;

        for (int e = 0; e < num_entries; e++) {
          PreparedEviction* pe = &entries[e];
          size_t byte_size = pe->word_size * HeapWordSize;
          uint32_t num_edges = (pe->edge_table != nullptr) ? pe->edge_table->_entry_count : 0;
          size_t edge_bytes = num_edges * 12;
          size_t entry_size = 32 + byte_size + edge_bytes;

          if (batch_offset + entry_size > BATCH_BUF_SIZE && batch_count > 0) {
            *(uint32_t*)(batch_buf + 0) = 0x16; // CMD_BATCH_EVICT_WITH_EDGES
            *(uint32_t*)(batch_buf + 4) = (uint32_t)batch_offset;
            *(uint64_t*)(batch_buf + 8) = 0;
            *(uint32_t*)(batch_buf + 16) = (uint32_t)batch_count;
            int rc = backend->batch_evict(batch_buf, batch_offset);
            if (rc < 0) {
              for (int f = batch_start_entry; f < e; f++) {
                backend->evict(cast_from_oop<void*>(entries[f].obj), entries[f].word_size,
                               entries[f].klass, entries[f].slot_id);
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

        if (batch_count > 0) {
          *(uint32_t*)(batch_buf + 0) = 0x16; // CMD_BATCH_EVICT_WITH_EDGES
          *(uint32_t*)(batch_buf + 4) = (uint32_t)batch_offset;
          *(uint64_t*)(batch_buf + 8) = 0;
          *(uint32_t*)(batch_buf + 16) = (uint32_t)batch_count;
          int rc = backend->batch_evict(batch_buf, batch_offset);
          if (rc < 0) {
            int start = num_entries - batch_count;
            for (int f = start; f < num_entries; f++) {
              backend->evict(cast_from_oop<void*>(entries[f].obj), entries[f].word_size,
                             entries[f].klass, entries[f].slot_id);
            }
          }
          batches_sent++;
        }

        os::free(batch_buf);
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

      if (total_candidates > 0) {
        double phase_e_ms = (Ticks::now() - phase_e_start).seconds() * 1000.0;
        log_info(gc)("Phase E eviction: %.1fms (E1=%.1fms E2=%.1fms/%d batches E3=%.1fms) "
                     "(%d objects, %d regions)",
                     phase_e_ms, e1_ms, e2_ms, batches_sent, e3_ms,
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
    } // end else (not concurrent start)
  }

  _g1h->rebuild_free_region_list();

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
  wait_for_root_region_scanning();

  G1YoungGCVerifierMark vm(this);
  {
    // Actual collection work starts and is executed (only) in this scope.

    // Young GC internal collection timing. The elapsed time recorded in the
    // policy for the collection deliberately elides verification (and some
    // other trivial setup above).
    policy()->record_young_collection_start();

    // Increment hotness epoch for recency tracking.
    _g1h->remote_memory_manager()->increment_gc_epoch();

    pre_evacuate_collection_set(jtm.evacuation_info());

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
    log_trace(gc)(">>> evacuate_initial_collection_set START");
    evacuate_initial_collection_set(&per_thread_states, may_do_optional_evacuation);
    log_trace(gc)(">>> evacuate_initial_collection_set DONE");

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

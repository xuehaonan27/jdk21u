/*
 * Copyright (c) 2014, 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1PARSCANTHREADSTATE_INLINE_HPP
#define SHARE_GC_G1_G1PARSCANTHREADSTATE_INLINE_HPP

#include "gc/g1/g1ParScanThreadState.hpp"

#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1OopStarChunkedList.inline.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "oops/access.inline.hpp"
#include "oops/oop.inline.hpp"

inline void G1ParScanThreadState::push_on_queue(ScannerTask task) {
#ifdef XHN_EVAC_RC
  if (_evacuation_stage == EvacuationStage::OldIteration) {
    fatal("[xhn:evac-rc] old iteration called push_on_queue\n");
  }
  // if (task.is_narrow_oop_ptr()) {
  //   narrowOop* p = task.to_narrow_oop_ptr();
  //   printf("[xhn:evac-rc] push_on_queue narrowOop* p = %p\n", p);
  // } else if (task.is_oop_ptr()) {
  //   oop* p = task.to_oop_ptr();
  //   printf("[xhn:evac-rc] push_on_queue oop* p = %p\n", p);
  // } else {
  //   oop src_array = task.to_partial_array_task().to_source_array();
  //   printf("[xhn:evac-rc] push_on_queue oop src_array = %p\n", cast_from_oop<void *>(src_array));
  // }
#endif // XHN_EVAC_RC
  verify_task(task);
  _task_queue->push(task);
}

bool G1ParScanThreadState::needs_partial_trimming() const {
  return !_task_queue->overflow_empty() ||
         (_task_queue->size() > _stack_trim_upper_threshold);
}

void G1ParScanThreadState::trim_queue_partially() {
  if (!needs_partial_trimming()) {
    return;
  }

  const Ticks start = Ticks::now();
  trim_queue_to_threshold(_stack_trim_lower_threshold);
  assert(_task_queue->overflow_empty(), "invariant");
  assert(_task_queue->size() <= _stack_trim_lower_threshold, "invariant");
  _trim_ticks += Ticks::now() - start;
}

void G1ParScanThreadState::trim_queue() {
  trim_queue_to_threshold(0);
  assert(_task_queue->overflow_empty(), "invariant");
  assert(_task_queue->taskqueue_empty(), "invariant");
}

#ifdef XHN_EVAC_RC
inline void G1ParScanThreadState::push_on_old_queue(ScannerTask task) {
  // print debug info
  // if (task.is_narrow_oop_ptr()) {
  //   narrowOop* p = task.to_narrow_oop_ptr();
  //   printf("[xhn:evac-rc] push_on_old_queue narrowOop* p = %p\n", p);
  // } else if (task.is_oop_ptr()) {
  //   oop* p = task.to_oop_ptr();
  //   printf("[xhn:evac-rc] push_on_old_queue oop* p = %p\n", p);
  // } else {
  //   oop src_array = task.to_partial_array_task().to_source_array();
  //   printf("[xhn:evac-rc] push_on_old_queue oop src_array = %p\n", cast_from_oop<void *>(src_array));
  // }

  verify_task(task);
  _old_task_queue->push(task);
}

bool G1ParScanThreadState::old_needs_partial_trimming() const {
  return !_old_task_queue->overflow_empty() ||
         (_old_task_queue->size() > _stack_trim_upper_threshold);
}

void G1ParScanThreadState::trim_old_queue_partially() {
  if (!old_needs_partial_trimming()) {
    return;
  }

  const Ticks start = Ticks::now();
  trim_old_queue_to_threshold(_stack_trim_lower_threshold);
  assert(_old_task_queue->overflow_empty(), "invariant");
  assert(_old_task_queue->size() <= _stack_trim_lower_threshold, "invariant");
  _trim_ticks += Ticks::now() - start;
}

void G1ParScanThreadState::trim_old_queue() {
  trim_old_queue_to_threshold(0);
  assert(_old_task_queue->overflow_empty(), "invariant");
  assert(_old_task_queue->taskqueue_empty(), "invariant");
}
#endif // XHN_EVAC_RC

inline Tickspan G1ParScanThreadState::trim_ticks() const {
  return _trim_ticks;
}

inline void G1ParScanThreadState::reset_trim_ticks() {
  _trim_ticks = Tickspan();
}

template <typename T>
inline void G1ParScanThreadState::remember_root_into_optional_region(T* p) {
  oop o = RawAccess<IS_NOT_NULL>::oop_load(p);
  uint index = _g1h->heap_region_containing(o)->index_in_opt_cset();
  assert(index < _max_num_optional_regions,
         "Trying to access optional region idx %u beyond " SIZE_FORMAT, index, _max_num_optional_regions);
  _oops_into_optional_regions[index].push_root(p);
}

template <typename T>
inline void G1ParScanThreadState::remember_reference_into_optional_region(T* p) {
  oop o = RawAccess<IS_NOT_NULL>::oop_load(p);
  uint index = _g1h->heap_region_containing(o)->index_in_opt_cset();
  assert(index < _max_num_optional_regions,
         "Trying to access optional region idx %u beyond " SIZE_FORMAT, index, _max_num_optional_regions);
  _oops_into_optional_regions[index].push_oop(p);
  verify_task(p);
}

G1OopStarChunkedList* G1ParScanThreadState::oops_into_optional_region(const HeapRegion* hr) {
  assert(hr->index_in_opt_cset() < _max_num_optional_regions,
         "Trying to access optional region idx %u beyond " SIZE_FORMAT " " HR_FORMAT,
         hr->index_in_opt_cset(), _max_num_optional_regions, HR_FORMAT_PARAMS(hr));
  return &_oops_into_optional_regions[hr->index_in_opt_cset()];
}

template <class T> void G1ParScanThreadState::write_ref_field_post(T* p, oop obj) {
  assert(obj != nullptr, "Must be");
  if (HeapRegion::is_in_same_region(p, obj)) {
    return;
  }
  G1HeapRegionAttr from_attr = _g1h->region_attr(p);
  // If this is a reference from (current) survivor regions, we do not need
  // to track references from it.
  if (from_attr.is_new_survivor()) {
    return;
  }
  G1HeapRegionAttr dest_attr = _g1h->region_attr(obj);
  // References to the current collection set are references to objects that failed
  // evacuation. Currently these regions are always relabelled as old without
  // remembered sets, so skip them.
  if (dest_attr.is_in_cset()) {
    assert(obj->is_forwarded(), "evac-failed but not forwarded: " PTR_FORMAT, p2i(obj));
    assert(obj->forwardee() == obj, "evac-failed but not self-forwarded: " PTR_FORMAT, p2i(obj));
    return;
  }
  enqueue_card_if_tracked(dest_attr, p, obj);
}

template <class T> void G1ParScanThreadState::enqueue_card_if_tracked(G1HeapRegionAttr region_attr, T* p, oop o) {
  assert(!HeapRegion::is_in_same_region(p, o), "Should have filtered out cross-region references already.");
  assert(!_g1h->heap_region_containing(p)->is_survivor(), "Should have filtered out from-newly allocated survivor references already.");
  // We relabel all regions that failed evacuation as old gen without remembered,
  // and so pre-filter them out in the caller.
  assert(!_g1h->heap_region_containing(o)->in_collection_set(), "Should not try to enqueue reference into collection set region");

#ifdef ASSERT
  HeapRegion* const hr_obj = _g1h->heap_region_containing(o);
  assert(region_attr.remset_is_tracked() == hr_obj->rem_set()->is_tracked(),
         "State flag indicating remset tracking disagrees (%s) with actual remembered set (%s) for region %u",
         BOOL_TO_STR(region_attr.remset_is_tracked()),
         BOOL_TO_STR(hr_obj->rem_set()->is_tracked()),
         hr_obj->hrm_index());
#endif
  if (!region_attr.remset_is_tracked()) {
    return;
  }
  size_t card_index = ct()->index_for(p);
  // If the card hasn't been added to the buffer, do it.
  if (_last_enqueued_card != card_index) {
    _rdc_local_qset.enqueue(ct()->byte_for_index(card_index));
    _last_enqueued_card = card_index;
  }
}

#endif // SHARE_GC_G1_G1PARSCANTHREADSTATE_INLINE_HPP

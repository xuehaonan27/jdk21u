/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1OOPCLOSURES_INLINE_HPP
#define SHARE_GC_G1_G1OOPCLOSURES_INLINE_HPP

#include "gc/g1/g1OopClosures.hpp"

#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1RemoteHandle.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1ParScanThreadState.inline.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1_globals.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.inline.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"
#include "utilities/align.hpp"

// ============================================================
// De-handleification: opportunistic downgrade of tagged oops during GC
// ============================================================
// When GC encounters a shared_oop(handle) field whose target is LOCAL
// and has remote_refcount == 0 (no remote object references it), rewrite
// the field to a clean oop. This eliminates the Handle indirection overhead
// for hot local objects that were previously referenced by remote objects.
//
// Ladder: shared_oop(handle) → unique_oop(addr) → clean oop(addr)
// For prototype simplicity, go directly shared → clean when safe.
static inline bool g1_remote_gc_scan_checks_enabled() {
  return LocalMemoryRatio < 100 || G1TagRefSites ||
         G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0;
}

static inline bool g1_gc_scan_region_contains_oop(G1CollectedHeap* g1h, oop obj) {
  if (obj == nullptr) {
    return false;
  }

  uintptr_t addr = cast_from_oop<uintptr_t>(obj);
  if (!g1_remote_oop_is_aligned(addr) ||
      !g1h->is_in_reserved((void*)addr)) {
    return false;
  }

  HeapRegion* hr = g1h->heap_region_containing_or_null((void*)addr);
  if (hr == nullptr || hr->is_free() || hr->is_evict_guarded()) {
    return false;
  }

  HeapWord* obj_addr = (HeapWord*)addr;
  return obj_addr >= hr->bottom() && obj_addr < hr->top();
}

template <class T>
static inline bool g1_retag_stale_gc_slot_if_possible(G1CollectedHeap* g1h, T* p, oop obj) {
  if (sizeof(T) != sizeof(uintptr_t) || p == nullptr) {
    return false;
  }
  if (!g1h->is_in_reserved((void*)p)) {
    return false;
  }

  HeapRegion* field_hr = g1h->heap_region_containing_or_null((void*)p);
  if (field_hr == nullptr || field_hr->is_free() || field_hr->is_evict_guarded()) {
    return false;
  }

  uintptr_t raw = *(uintptr_t*)p;
  if (raw == 0 ||
      (raw & (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) ==
      (G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT)) {
    return false;
  }

  uintptr_t addr = (raw & G1_OOP_TAG_MASK) != 0 ? (raw & G1_OOP_ADDR_MASK) :
                                                  cast_from_oop<uintptr_t>(obj);
  if (addr == 0 || !g1_remote_oop_is_aligned(addr)) {
    return false;
  }

  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
  RemoteHandle* h = rmm == nullptr ? nullptr : rmm->handle_for_addr_any_state(addr);
  if (h == nullptr) {
    return false;
  }

  uintptr_t sa = h->load_state_and_addr_acquire();
  if ((sa & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_DEAD) {
    return false;
  }

  *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
  return true;
}

template <class T>
static inline bool g1_gc_resolve_oop_for_scan(G1CollectedHeap* g1h, T* p, oop* obj_addr) {
  if (!g1_remote_gc_scan_checks_enabled()) {
    return true;
  }
  oop obj = *obj_addr;
  if (g1_gc_scan_region_contains_oop(g1h, obj)) {
    return true;
  }

  // A clean pre-eviction oop can survive in a dirty card/root slot. If the
  // field is a writable heap slot, repair it back to a shared handle and
  // rescan that repaired edge in the same GC pass.  Full GC in particular has
  // no later remembered-set pass that would rediscover an edge repaired here.
  if (g1_retag_stale_gc_slot_if_possible(g1h, p, obj)) {
    oop repaired = g1_resolved_load(p);
    if (g1_gc_scan_region_contains_oop(g1h, repaired)) {
      *obj_addr = repaired;
      return true;
    }
  }
  return false;
}

template <class T>
static inline bool g1_gc_resolved_oop_safe_for_scan(G1CollectedHeap* g1h, T* p, oop obj) {
  oop resolved = obj;
  return g1_gc_resolve_oop_for_scan(g1h, p, &resolved);
}

template <class T>
inline void G1ScanClosureBase::prefetch_and_push(T* p, const oop obj) {
  // We're not going to even bother checking whether the object is
  // already forwarded or not, as this usually causes an immediate
  // stall. We'll try to prefetch the object (for write, given that
  // we might need to install the forwarding reference) and we'll
  // get back to it when pop it from the queue
  Prefetch::write(obj->mark_addr(), 0);
  Prefetch::read(obj->mark_addr(), (HeapWordSize*2));

  // slightly paranoid test; I'm trying to catch potential
  // problems before we go into push_on_queue to know where the
  // problem is coming from
  // Note: use g1_resolved_load instead of RawAccess to handle tagged oops.
  assert((obj == g1_resolved_load(p)) ||
         (obj->is_forwarded() &&
         obj->forwardee() == g1_resolved_load(p)),
         "p should still be pointing to obj or to its forwardee");

  _par_scan_state->push_on_queue(ScannerTask(p));
}

template <class T>
inline void G1ScanClosureBase::handle_non_cset_obj_common(G1HeapRegionAttr const region_attr, T* p, oop const obj) {
  if (region_attr.is_humongous_candidate()) {
    _g1h->set_humongous_is_live(obj);
  } else if (region_attr.is_optional()) {
    _par_scan_state->remember_reference_into_optional_region(p);
  }
}

inline void G1ScanClosureBase::trim_queue_partially() {
  _par_scan_state->trim_queue_partially();
}

template <class T>
inline void G1ScanEvacuatedObjClosure::do_oop_work(T* p) {
  oop obj = g1_resolved_load(p);
  if (obj == nullptr) {
    return;
  }
  if (!g1_gc_resolve_oop_for_scan(_g1h, p, &obj)) {
    return;
  }
  // Phase 6: skip remote objects (resolved to non-heap slot_id)
  if (!_g1h->is_in(obj)) {
    return;
  }
  const G1HeapRegionAttr region_attr = _g1h->region_attr(obj);
  if (region_attr.is_in_cset()) {
    prefetch_and_push(p, obj);
  } else if (!HeapRegion::is_in_same_region(p, obj)) {
    handle_non_cset_obj_common(region_attr, p, obj);
    assert(_skip_card_enqueue != Uninitialized, "Scan location has not been initialized.");
    if (_skip_card_enqueue == True) {
      return;
    }
    _par_scan_state->enqueue_card_if_tracked(region_attr, p, obj);
  }
}

template <class T>
inline void G1CMOopClosure::do_oop_work(T* p) {
  _task->deal_with_reference(p);
}

template <class T>
inline void G1RootRegionScanClosure::do_oop_work(T* p) {
  oop obj = g1_resolved_load<MO_RELAXED>(p);
  if (obj == nullptr) {
    return;
  }
  if (!g1_gc_resolve_oop_for_scan(_g1h, p, &obj)) {
    return;
  }
  // Phase 6: skip remote objects (resolved to non-heap slot_id)
  if (!_g1h->is_in(obj)) {
    return;
  }
  if (LocalMemoryRatio < 100 || G1TagRefSites ||
      G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0) {
    G1RemoteMemoryManager* rmm = _g1h->remote_memory_manager();
    if (rmm != nullptr) {
      RemoteHandle* h = rmm->handle_for_addr_any_state(cast_from_oop<uintptr_t>(obj));
      if (h != nullptr && !h->is_local()) {
        return;
      }
    }
    if (!g1_cm_mark_safe_local_oop(_g1h, obj)) {
      return;
    }
  }
  _cm->mark_in_bitmap(_worker_id, obj);
}

template <class T>
inline static void check_obj_during_refinement(T* p, oop const obj) {
#ifdef ASSERT
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  // can't do because of races
  // assert(oopDesc::is_oop_or_null(obj), "expected an oop");
  assert(is_object_aligned(obj), "obj must be aligned");
  assert(g1h->is_in(obj), "invariant");
  assert(g1h->is_in(p), "invariant");
#endif // ASSERT
}

template <class T>
inline void G1ConcurrentRefineOopClosure::do_oop_work(T* p) {
  oop obj = g1_resolved_load<MO_RELAXED>(p);
  if (obj == nullptr) {
    return;
  }
  if (!g1_gc_resolve_oop_for_scan(_g1h, p, &obj)) {
    return;
  }
  // Phase 6: skip remote objects (resolved to non-heap slot_id)
  if (!_g1h->is_in(obj)) {
    return;
  }

  check_obj_during_refinement(p, obj);

  if (HeapRegion::is_in_same_region(p, obj)) {
    // Normally this closure should only be called with cross-region references.
    // But since Java threads are manipulating the references concurrently and we
    // reload the values things may have changed.
    // Also this check lets slip through references from a humongous continues region
    // to its humongous start region, as they are in different regions, and adds a
    // remembered set entry. This is benign (apart from memory usage), as we never
    // try to either evacuate or eager reclaim humonguous arrays of j.l.O.
    return;
  }

  HeapRegionRemSet* to_rem_set = _g1h->heap_region_containing(obj)->rem_set();

  assert(to_rem_set != nullptr, "Need per-region 'into' remsets.");
  if (to_rem_set->is_tracked()) {
    to_rem_set->add_reference(p, _worker_id);
  }
}

template <class T>
inline void G1ScanCardClosure::do_oop_work(T* p) {
  oop obj = g1_resolved_load(p);
  if (obj == nullptr) {
    return;
  }
  if (!g1_gc_resolve_oop_for_scan(_g1h, p, &obj)) {
    return;
  }
  // Phase 6: skip remote objects (resolved to non-heap slot_id)
  if (!_g1h->is_in(obj)) {
    return;
  }

  check_obj_during_refinement(p, obj);

  assert(!_g1h->is_in_cset((HeapWord*)p),
         "Oop originates from " PTR_FORMAT " (region: %u) which is in the collection set.",
         p2i(p), _g1h->addr_to_region(p));

  const G1HeapRegionAttr region_attr = _g1h->region_attr(obj);
  if (region_attr.is_in_cset()) {
    // Since the source is always from outside the collection set, here we implicitly know
    // that this is a cross-region reference too.
    prefetch_and_push(p, obj);
    _heap_roots_found++;
  } else if (!HeapRegion::is_in_same_region(p, obj)) {
    handle_non_cset_obj_common(region_attr, p, obj);
    _par_scan_state->enqueue_card_if_tracked(region_attr, p, obj);
  }
}

template <class T>
inline void G1ScanRSForOptionalClosure::do_oop_work(T* p) {
  const G1HeapRegionAttr region_attr = _g1h->region_attr(p);
  // Entries in the optional collection set may start to originate from the collection
  // set after one or more increments. In this case, previously optional regions
  // became actual collection set regions. Filter them out here.
  if (region_attr.is_in_cset()) {
    return;
  }
  _scan_cl->do_oop_work(p);
  _scan_cl->trim_queue_partially();
}

void G1ParCopyHelper::do_cld_barrier(oop new_obj) {
  if (_g1h->heap_region_containing(new_obj)->is_young()) {
    _scanned_cld->record_modified_oops();
  }
}

void G1ParCopyHelper::mark_object(oop obj) {
  if (!g1_gc_resolved_oop_safe_for_scan<oop>(_g1h, nullptr, obj)) {
    return;
  }
  assert(!_g1h->heap_region_containing(obj)->in_collection_set(), "should not mark objects in the CSet");

  // We know that the object is not moving so it's safe to read its size.
  _cm->mark_in_bitmap(_worker_id, obj);
}

void G1ParCopyHelper::trim_queue_partially() {
  _par_scan_state->trim_queue_partially();
}

template <G1Barrier barrier, bool should_mark>
template <class T>
void G1ParCopyClosure<barrier, should_mark>::do_oop_work(T* p) {
  oop obj = g1_resolved_load(p);
  if (obj == nullptr) {
    return;
  }
  if (!g1_gc_resolve_oop_for_scan(_g1h, p, &obj)) {
    return;
  }
  // Phase 6: skip remote objects (resolved to non-heap slot_id)
  if (!_g1h->is_in(obj)) {
    return;
  }
  assert(_worker_id == _par_scan_state->worker_id(), "sanity");

  const G1HeapRegionAttr state = _g1h->region_attr(obj);
  if (state.is_in_cset()) {
    oop forwardee;
    markWord m = obj->mark();
    if (m.is_marked()) {
      forwardee = cast_to_oop(m.decode_pointer());
    } else {
      forwardee = _par_scan_state->copy_to_survivor_space(state, obj, m);
    }
    assert(forwardee != nullptr, "forwardee should not be null");

    // Tag-aware write-back: if the field contains a shared_oop(Handle),
    // update the Handle's address instead of overwriting the tagged field.
    // The field (in the copied object) retains the tagged encoding.
    bool wrote_clean_oop = false;
    if (sizeof(T) == sizeof(uintptr_t)) {
      uintptr_t raw = *(uintptr_t*)p;
      if (raw & G1_OOP_INDIRECT_BIT) {
        RemoteHandle* h = (RemoteHandle*)(raw & G1_OOP_ADDR_MASK);
        _g1h->remote_memory_manager()->update_handle_for_evacuation(h, obj, forwardee);
      } else {
        RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);
        wrote_clean_oop = true;
      }
    } else {
      RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);
      wrote_clean_oop = true;
    }

    // After writing a clean forwardee to a field outside the collection set,
    // re-dirty the card and enqueue it in the DCQS. merge_heap_roots already
    // drained the DCQS for this pause, so the enqueued card survives until
    // the next GC processes it — ensuring the forwardee's region finds this
    // incoming reference regardless of whether it tracks RSets.
    if (wrote_clean_oop && _g1h->is_in(p) &&
        !_g1h->region_attr(p).is_in_cset() &&
        !HeapRegion::is_in_same_region(p, forwardee)) {
      G1CardTable* ct = _g1h->card_table();
      CardTable::CardValue* card = ct->byte_for((HeapWord*)p);
      if (*card != G1CardTable::g1_young_card_val()) {
        *card = G1CardTable::dirty_card_val();
        G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
        G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(Thread::current());
        qset.enqueue(queue, card);
      }
    }

    if (barrier == G1BarrierCLD) {
      do_cld_barrier(forwardee);
    }
  } else {
    if (state.is_humongous_candidate()) {
      _g1h->set_humongous_is_live(obj);
    } else if ((barrier != G1BarrierNoOptRoots) && state.is_optional()) {
      _par_scan_state->remember_root_into_optional_region(p);
    }

    // The object is not in the collection set. should_mark is true iff the
    // current closure is applied on strong roots (and weak roots when class
    // unloading is disabled) in a concurrent mark start pause.
    if (should_mark) {
      mark_object(obj);
    }
  }
  trim_queue_partially();
}

template <class T> void G1RebuildRemSetClosure::do_oop_work(T* p) {
  oop obj = g1_resolved_load<MO_RELAXED>(p);
  if (obj == nullptr) {
    return;
  }
  if (!g1_gc_resolve_oop_for_scan(_g1h, p, &obj)) {
    return;
  }
  // Phase 6: skip remote objects (resolved to non-heap slot_id)
  if (!_g1h->is_in(obj)) {
    return;
  }

  if (HeapRegion::is_in_same_region(p, obj)) {
    return;
  }

  HeapRegion* to = _g1h->heap_region_containing(obj);
  HeapRegionRemSet* rem_set = to->rem_set();
  if (rem_set->is_tracked()) {
    rem_set->add_reference(p, _worker_id);
  }
}

#endif // SHARE_GC_G1_G1OOPCLOSURES_INLINE_HPP

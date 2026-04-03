/*
 * Copyright (c) 2016, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1BARRIERSET_INLINE_HPP
#define SHARE_GC_G1_G1BARRIERSET_INLINE_HPP

#include "gc/g1/g1BarrierSet.hpp"

#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/shared/accessBarrierSupport.inline.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.hpp"
#include "runtime/thread.hpp"

inline void G1BarrierSet::enqueue_preloaded(oop pre_val) {
  // Nulls should have been already filtered.
  assert(oopDesc::is_oop(pre_val, true), "Error");

  G1SATBMarkQueueSet& queue_set = G1BarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(Thread::current());
  queue_set.enqueue_known_active(queue, pre_val);
}

template <class T>
inline void G1BarrierSet::enqueue(T* dst) {
  G1SATBMarkQueueSet& queue_set = G1BarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  T heap_oop = RawAccess<MO_RELAXED>::oop_load(dst);
  if (!CompressedOops::is_null(heap_oop)) {
    // Resolve tag bits before SATB enqueue — SATB queue asserts valid heap pointers
    // (g1SATBMarkQueueSet.cpp:83). Tagged oops would fail this assertion.
    oop resolved = resolve_oop_raw(CompressedOops::decode_not_null(heap_oop));
    SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(Thread::current());
    queue_set.enqueue_known_active(queue, resolved);
  }
}

template <DecoratorSet decorators, typename T>
inline void G1BarrierSet::write_ref_field_pre(T* field) {
  if (HasDecorator<decorators, IS_DEST_UNINITIALIZED>::value ||
      HasDecorator<decorators, AS_NO_KEEPALIVE>::value) {
    return;
  }

  enqueue(field);
}

inline void G1BarrierSet::invalidate(MemRegion mr) {
  invalidate(JavaThread::current(), mr);
}

inline void G1BarrierSet::write_region(JavaThread* thread, MemRegion mr) {
  invalidate(thread, mr);
}

inline void G1BarrierSet::write_ref_array_work(MemRegion mr) {
  invalidate(mr);
}

template <DecoratorSet decorators, typename T>
inline void G1BarrierSet::write_ref_field_post(T* field) {
  volatile CardValue* byte = _card_table->byte_for(field);
  if (*byte != G1CardTable::g1_young_card_val()) {
    // Take a slow path for cards in old
    write_ref_field_post_slow(byte);
  }
}

inline void G1BarrierSet::enqueue_preloaded_if_weak(DecoratorSet decorators, oop value) {
  assert((decorators & ON_UNKNOWN_OOP_REF) == 0, "Reference strength must be known");
  // Loading from a weak or phantom reference needs enqueueing, as
  // the object may not have been reachable (part of the snapshot)
  // when marking started.
  const bool on_strong_oop_ref = (decorators & ON_STRONG_OOP_REF) != 0;
  const bool peek              = (decorators & AS_NO_KEEPALIVE) != 0;
  const bool needs_enqueue     = (!peek && !on_strong_oop_ref);

  if (needs_enqueue && value != nullptr) {
    enqueue_preloaded(value);
  }
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_load_not_in_heap(T* addr) {
  oop value = ModRef::oop_load_not_in_heap(addr);
  // Resolve tag bits before SATB enqueue — SATB queue requires clean oops
  value = resolve_oop_raw(value);
  enqueue_preloaded_if_weak(decorators, value);
  return value;
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_load_in_heap(T* addr) {
  oop value = ModRef::oop_load_in_heap(addr);

  // === Disaggregated Memory Load Barrier ===
  // Resolve tag bits. For REMOTE objects, trigger simulated fetch.
  uintptr_t v = cast_from_oop<uintptr_t>(value);
  if ((v & G1_OOP_TAG_MASK) != 0) {
    if (v & G1_OOP_INDIRECT_BIT) {
      // Shared OOP: follow Handle
      RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;

      if (state == REMOTE_HANDLE_LOCAL) {
        // Fast path: Handle is LOCAL
        value = cast_to_oop(sa & REMOTE_HANDLE_ADDR_MASK);
      } else if (state == REMOTE_HANDLE_REMOTE) {
        // Slow path: object is remote — trigger fetch
        // CAS REMOTE -> FETCHING (we win the fetch race)
        if (h->cas_remote_to_fetching()) {
          value = G1BarrierSet::resolve_remote_fetch(h);
        } else {
          // Someone else is fetching — spin until LOCAL
          value = G1BarrierSet::wait_for_fetch(h);
        }
      } else {
        // FETCHING state — someone else is fetching, wait
        value = G1BarrierSet::wait_for_fetch(h);
      }
    } else {
      // Unique/Direct OOP: just strip tags
      value = cast_to_oop(v & G1_OOP_ADDR_MASK);
    }
  }

  enqueue_preloaded_if_weak(decorators, value);
  return value;
}

template <DecoratorSet decorators, typename BarrierSetT>
inline oop G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_load_in_heap_at(oop base, ptrdiff_t offset) {
  oop value = ModRef::oop_load_in_heap_at(base, offset);
  // Resolve tag bits before SATB enqueue — SATB queue requires clean oops
  value = resolve_oop_raw(value);
  enqueue_preloaded_if_weak(AccessBarrierSupport::resolve_possibly_unknown_oop_ref_strength<decorators>(base, offset), value);
  return value;
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline void G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_store_not_in_heap(T* addr, oop new_value) {
  // Apply SATB barriers for all non-heap references, to allow
  // concurrent scanning of such references.
  G1BarrierSet *bs = barrier_set_cast<G1BarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_field_pre<decorators>(addr);
  Raw::oop_store(addr, new_value);
}

// ============================================================
// Minimal write barrier for disaggregated memory (Phase 1).
// When storing a reference to a managed Old object, encode
// the stored value as a Shared OOP pointing to the Handle.
// This ensures all references to managed objects go through
// the Handle, maintaining coherence for remote eviction.
// ============================================================

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline void G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_store_in_heap(T* addr, oop new_value) {
  oop store_value = new_value;

  // Check if the target object is managed (has remote memory metadata).
  // If so, encode the reference as a Shared OOP pointing to its Handle.
  // This is the minimal Phase 1 write barrier — no Unique/Shared distinction,
  // all managed objects are treated as Shared.
  // Check if the target object is managed (has remote memory metadata).
  // Only check when new_value is non-null and the mark word is in
  // unlocked state (has_remote_metadata checks this internally).
  // Guard against early bootstrap when G1CollectedHeap may not be ready.
  if (new_value != nullptr && Universe::heap() != nullptr) {
    markWord mw = new_value->mark();
    if (mw.has_remote_metadata()) {
      // Target is managed — find its Handle and encode as Shared OOP.
      G1CollectedHeap* g1h = G1CollectedHeap::heap();
      G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
      if (rmm != nullptr) {
        RemoteHandle* h = rmm->handle_for(new_value);
        if (h != nullptr) {
          store_value = g1_make_shared_oop((void*)h);
        }
      }
    }
  }

  // Standard ModRef store with SATB pre-barrier and card marking post-barrier.
  ModRef::oop_store_in_heap(addr, store_value);
}

template <DecoratorSet decorators, typename BarrierSetT>
inline void G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_store_in_heap_at(oop base, ptrdiff_t offset, oop new_value) {
  oop store_value = new_value;

  if (new_value != nullptr && Universe::heap() != nullptr) {
    markWord mw = new_value->mark();
    if (mw.has_remote_metadata()) {
      G1CollectedHeap* g1h = G1CollectedHeap::heap();
      G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
      if (rmm != nullptr) {
        RemoteHandle* h = rmm->handle_for(new_value);
        if (h != nullptr) {
          store_value = g1_make_shared_oop((void*)h);
        }
      }
    }
  }

  ModRef::oop_store_in_heap_at(base, offset, store_value);
}

#endif // SHARE_GC_G1_G1BARRIERSET_INLINE_HPP

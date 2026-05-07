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

#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/accessBarrierSupport.inline.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.hpp"
#include "runtime/thread.hpp"

static inline bool g1_remote_mode_active() {
  return LocalMemoryRatio < 100 || G1TagRefSites ||
         G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0;
}

static inline bool g1_needs_remote_resolve(oop value) {
  if (value == nullptr) return false;

  uintptr_t v = cast_from_oop<uintptr_t>(value);
  if ((v & G1_OOP_TAG_MASK) != 0) return true;
  if (!g1_remote_mode_active()) return false;

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  if (g1h == nullptr || !g1h->is_in_reserved((void*)v)) return false;

  HeapRegion* hr = g1h->heap_region_containing_or_null((void*)v);
  if (hr == nullptr || hr->is_free() || hr->is_evict_guarded() ||
      !g1h->is_in((void*)v)) {
    return true;
  }

  // An evicted region can be returned to normal heap service before every
  // stale clean oop has been converted to a shared handle. Only regions that
  // have actually held eviction fillers need the heavier klass/filler check.
  if (!hr->had_remote_eviction_fillers()) {
    return false;
  }

  oop obj = cast_to_oop((HeapWord*)v);
  Klass* k = obj->klass_or_null();
  return k == nullptr || G1CollectedHeap::is_obj_filler(obj);
}

static inline oop g1_resolve_remote_oop_if_needed(oop value,
                                                  uint32_t access_hint = G1RemoteAccessHintUnknown) {
  if (g1_needs_remote_resolve(value)) {
    return cast_to_oop(G1BarrierSetRuntime::resolve_tagged_oop_no_safepoint_with_hint((oopDesc*)value,
                                                                                      access_hint));
  }
  return value;
}

inline void G1BarrierSet::enqueue_preloaded(oop pre_val) {
  // Nulls should have been already filtered.
  oop resolved = resolve_oop_raw(pre_val);
  if (resolved == nullptr ||
      !g1_remote_oop_is_aligned(cast_from_oop<uintptr_t>(resolved))) {
    return;
  }
  assert(oopDesc::is_oop(resolved, true), "Error");

  G1SATBMarkQueueSet& queue_set = G1BarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(Thread::current());
  queue_set.enqueue_known_active(queue, resolved);
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
    if (resolved == nullptr ||
        !g1_remote_oop_is_aligned(cast_from_oop<uintptr_t>(resolved))) {
      return;
    }
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

  if (G1RemoteMoleculeProfile) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    G1RemoteMemoryManager* rmm =
        g1h == nullptr ? nullptr : g1h->remote_memory_manager();
    if (rmm != nullptr && rmm->molecule_profile_enabled()) {
      rmm->record_molecule_profile_ref_overwrite(
          (void*)field, sizeof(T) == sizeof(narrowOop));
    }
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
  // Resolve tagged oops, and in remote mode also clean stale oops whose
  // old local region has since been evict-guarded.
  value = g1_resolve_remote_oop_if_needed(value,
      (decorators & IS_ARRAY) != 0 ? G1RemoteAccessHintArray : G1RemoteAccessHintField);

  guarantee(value == nullptr || (cast_from_oop<uintptr_t>(value) >> 47) == 0,
            "oop_load_in_heap: barrier returned tagged value " PTR_FORMAT " from addr " PTR_FORMAT,
            cast_from_oop<uintptr_t>(value), p2i(addr));

  enqueue_preloaded_if_weak(decorators, value);
  return value;
}

template <DecoratorSet decorators, typename BarrierSetT>
inline oop G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_load_in_heap_at(oop base, ptrdiff_t offset) {
  if (g1_needs_remote_resolve(base)) {
    base = g1_resolve_remote_oop_if_needed(base, G1RemoteAccessHintField);
    if (base == nullptr) {
      return nullptr;
    }
  }
  // Diagnostic: catch tagged base oops — means someone passed an unresolved
  // tagged oop as an object base for field access.
  guarantee((cast_from_oop<uintptr_t>(base) >> 47) == 0,
            "oop_load_in_heap_at: tagged base oop " PTR_FORMAT " at offset " INTX_FORMAT,
            cast_from_oop<uintptr_t>(base), (intx)offset);
  oop value = ModRef::oop_load_in_heap_at(base, offset);
  // Resolve tagged oops and clean stale oops via the centralized runtime.
  value = g1_resolve_remote_oop_if_needed(value,
      (decorators & IS_ARRAY) != 0 ? G1RemoteAccessHintArray : G1RemoteAccessHintField);
  assert(value == nullptr || (cast_from_oop<uintptr_t>(value) >> 47) == 0,
         "oop_load_in_heap_at: barrier returned tagged " PTR_FORMAT " base=" PTR_FORMAT " off=" INTX_FORMAT,
         cast_from_oop<uintptr_t>(value), cast_from_oop<uintptr_t>(base), (intx)offset);
  enqueue_preloaded_if_weak(AccessBarrierSupport::resolve_possibly_unknown_oop_ref_strength<decorators>(base, offset), value);
  return value;
}

// Arraycopy: for non-checkcast copies, let tagged oops propagate as-is
// (load barrier resolves on read). For checkcast copies, resolve tagged
// oops BEFORE the type check — is_instanceof dereferences the oop to
// read klass, which crashes on a tagged pointer (bit 63 set).
template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline bool G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_arraycopy_in_heap(arrayOop src_obj, size_t src_offset_in_bytes, T* src_raw,
                      arrayOop dst_obj, size_t dst_offset_in_bytes, T* dst_raw,
                      size_t length) {
  if (!HasDecorator<decorators, ARRAYCOPY_CHECKCAST>::value) {
    return ModRef::oop_arraycopy_in_heap(src_obj, src_offset_in_bytes, src_raw,
                                         dst_obj, dst_offset_in_bytes, dst_raw,
                                         length);
  }

  BarrierSetT *bs = barrier_set_cast<BarrierSetT>(BarrierSet::barrier_set());
  src_raw = arrayOopDesc::obj_offset_to_raw(src_obj, src_offset_in_bytes, src_raw);
  dst_raw = arrayOopDesc::obj_offset_to_raw(dst_obj, dst_offset_in_bytes, dst_raw);

  assert(dst_obj != nullptr, "better have an actual oop");
  Klass* bound = objArrayOop(dst_obj)->element_klass();
  T* from = const_cast<T*>(src_raw);
  T* end  = from + length;
  for (T* p = dst_raw; from < end; from++, p++) {
    T element = *from;
    oop decoded = CompressedOops::decode(element);
    bool resolved_tag = false;
    // Tagged oops (bit 63) only exist with uncompressed oops (sizeof(T)==8).
    if (sizeof(T) == sizeof(oop) && decoded != nullptr &&
        (cast_from_oop<uintptr_t>(decoded) & G1_OOP_TAG_MASK) != 0) {
      decoded = resolve_oop_full(decoded);
      resolved_tag = true;
    }
    if (oopDesc::is_instanceof_or_null(decoded, bound)) {
      bs->template write_ref_field_pre<decorators>(p);
      if (resolved_tag) {
        *(oop*)p = decoded;
      } else {
        *p = element;
      }
    } else {
      const size_t pd = pointer_delta(p, dst_raw, (size_t)heapOopSize);
      assert(pd == (size_t)(int)pd, "length field overflow");
      bs->write_ref_array((HeapWord*)dst_raw, pd);
      return false;
    }
  }
  bs->write_ref_array((HeapWord*)dst_raw, length);
  return true;
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

// Write barrier: standard G1 oop store (SATB pre-barrier + card marking).
//
// In the invisible-handle design, local stores always write clean oops.
// Handleification (shared_oop encoding) is reserved for explicit eviction-time
// rewriting only. The old resolve_managed_store() path that re-encoded stores
// as shared_oop(handle) is removed — it fought de-handleification by re-tagging
// oops that had been intentionally converted back to clean.

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline void G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_store_in_heap(T* addr, oop new_value) {
  new_value = g1_resolve_remote_oop_if_needed(new_value, G1RemoteAccessHintField);
  ModRef::oop_store_in_heap(addr, new_value);
}

template <DecoratorSet decorators, typename BarrierSetT>
inline void G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_store_in_heap_at(oop base, ptrdiff_t offset, oop new_value) {
  new_value = g1_resolve_remote_oop_if_needed(new_value, G1RemoteAccessHintField);
  if (g1_needs_remote_resolve(base)) {
    base = g1_resolve_remote_oop_if_needed(base, G1RemoteAccessHintField);
    if (base == nullptr) {
      return;
    }
  }
  ModRef::oop_store_in_heap_at(base, offset, new_value);
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_atomic_cmpxchg_in_heap(T* addr, oop compare_value, oop new_value) {
  BarrierSetT* bs = barrier_set_cast<BarrierSetT>(barrier_set());
  bs->template write_ref_field_pre<decorators>(addr);

  oop result = Raw::oop_atomic_cmpxchg(addr, compare_value, new_value);
  if (result == compare_value) {
    bs->template write_ref_field_post<decorators>(addr);
    return result;
  }

  // A field may hold a tagged remote/shared handle even though Java code got
  // the clean local oop from a previous load barrier.  In that case retry the
  // CAS against the raw tagged bits, but return the clean logical witness.
  if (!UseCompressedOops && result != nullptr &&
      (cast_from_oop<uintptr_t>(result) & G1_OOP_TAG_MASK) != 0) {
    oop resolved = resolve_oop_full(result);
    if (resolved == compare_value) {
      oop retry = Raw::oop_atomic_cmpxchg(addr, result, new_value);
      if (retry == result) {
        bs->template write_ref_field_post<decorators>(addr);
        return compare_value;
      }
      result = retry;
    } else {
      result = resolved;
    }
  }

  if (!UseCompressedOops && result != nullptr &&
      (cast_from_oop<uintptr_t>(result) & G1_OOP_TAG_MASK) != 0) {
    result = resolve_oop_full(result);
  }
  return result;
}

template <DecoratorSet decorators, typename BarrierSetT>
inline oop G1BarrierSet::AccessBarrier<decorators, BarrierSetT>::
oop_atomic_cmpxchg_in_heap_at(oop base, ptrdiff_t offset, oop compare_value, oop new_value) {
  if (g1_needs_remote_resolve(base)) {
    base = g1_resolve_remote_oop_if_needed(base, G1RemoteAccessHintAtomic);
    if (base == nullptr) {
      return nullptr;
    }
  }
  return oop_atomic_cmpxchg_in_heap(AccessInternal::oop_field_addr<decorators>(base, offset),
                                   compare_value, new_value);
}

#endif // SHARE_GC_G1_G1BARRIERSET_INLINE_HPP

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

#include "precompiled.hpp"
#include "gc/g1/g1BarrierSet.inline.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1CardTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1SATBMarkQueueSet.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/satbMarkQueue.hpp"
#include "logging/log.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER1
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#endif

class G1BarrierSetC1;
class G1BarrierSetC2;

G1BarrierSet::G1BarrierSet(G1CardTable* card_table) :
  CardTableBarrierSet(make_barrier_set_assembler<G1BarrierSetAssembler>(),
                      make_barrier_set_c1<G1BarrierSetC1>(),
                      make_barrier_set_c2<G1BarrierSetC2>(),
                      card_table,
                      BarrierSet::FakeRtti(BarrierSet::G1BarrierSet)),
  _satb_mark_queue_buffer_allocator("SATB Buffer Allocator", G1SATBBufferSize),
  _dirty_card_queue_buffer_allocator("DC Buffer Allocator", G1UpdateBufferSize),
  _satb_mark_queue_set(&_satb_mark_queue_buffer_allocator),
  _dirty_card_queue_set(&_dirty_card_queue_buffer_allocator)
{}

template <class T> void
G1BarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  G1SATBMarkQueueSet& queue_set = G1BarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;

  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(Thread::current());

  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = RawAccess<>::oop_load(elem_ptr);
    if (!CompressedOops::is_null(heap_oop)) {
      // Resolve tag bits before SATB enqueue (array pre-barrier path).
      // resolve_oop_raw returns nullptr for REMOTE handles — skip those.
      oop resolved = resolve_oop_raw(cast_to_oop((uintptr_t)(oopDesc*)heap_oop));
      if (resolved != nullptr) {
        queue_set.enqueue_known_active(queue, resolved);
      }
    }
  }
}

void G1BarrierSet::write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_field_post_slow(volatile CardValue* byte) {
  // In the slow path, we know a card is not young
  assert(*byte != G1CardTable::g1_young_card_val(), "slow path invoked without filtering");
  OrderAccess::storeload();
  if (*byte != G1CardTable::dirty_card_val()) {
    *byte = G1CardTable::dirty_card_val();
    Thread* thr = Thread::current();
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thr);
    G1BarrierSet::dirty_card_queue_set().enqueue(queue, byte);
  }
}

void G1BarrierSet::invalidate(JavaThread* thread, MemRegion mr) {
  if (mr.is_empty()) {
    return;
  }
  volatile CardValue* byte = _card_table->byte_for(mr.start());
  CardValue* last_byte = _card_table->byte_for(mr.last());

  // skip young gen cards
  if (*byte == G1CardTable::g1_young_card_val()) {
    // MemRegion should not span multiple regions for the young gen.
    DEBUG_ONLY(HeapRegion* containing_hr = G1CollectedHeap::heap()->heap_region_containing(mr.start());)
    assert(containing_hr->is_young(), "it should be young");
    assert(containing_hr->is_in(mr.start()), "it should contain start");
    assert(containing_hr->is_in(mr.last()), "it should also contain last");
    return;
  }

  OrderAccess::storeload();
  // Enqueue if necessary.
  G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
  G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thread);
  for (; byte <= last_byte; byte++) {
    CardValue bv = *byte;
    assert(bv != G1CardTable::g1_young_card_val(), "Invalid card");
    if (bv != G1CardTable::dirty_card_val()) {
      *byte = G1CardTable::dirty_card_val();
      qset.enqueue(queue, byte);
    }
  }
}

void G1BarrierSet::on_thread_create(Thread* thread) {
  // Create thread local data
  G1ThreadLocalData::create(thread);
}

void G1BarrierSet::on_thread_destroy(Thread* thread) {
  // Destroy thread local data
  G1ThreadLocalData::destroy(thread);
}

void G1BarrierSet::on_thread_attach(Thread* thread) {
  BarrierSet::on_thread_attach(thread);
  SATBMarkQueue& satbq = G1ThreadLocalData::satb_mark_queue(thread);
  assert(!satbq.is_active(), "SATB queue should not be active");
  assert(satbq.buffer() == nullptr, "SATB queue should not have a buffer");
  assert(satbq.index() == 0, "SATB queue index should be zero");
  G1DirtyCardQueue& dirtyq = G1ThreadLocalData::dirty_card_queue(thread);
  assert(dirtyq.buffer() == nullptr, "Dirty Card queue should not have a buffer");
  assert(dirtyq.index() == 0, "Dirty Card queue index should be zero");

  // If we are creating the thread during a marking cycle, we should
  // set the active field of the SATB queue to true.  That involves
  // copying the global is_active value to this thread's queue.
  satbq.set_active(_satb_mark_queue_set.is_active());
}

void G1BarrierSet::on_thread_detach(Thread* thread) {
  // Flush any deferred card marks.
  CardTableBarrierSet::on_thread_detach(thread);
  {
    SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
    G1BarrierSet::satb_mark_queue_set().flush_queue(queue);
  }
  {
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thread);
    G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
    qset.flush_queue(queue);
    qset.record_detached_refinement_stats(queue.refinement_stats());
  }
}

// ============================================================
// Disaggregated Memory: Remote Object Fetch (Tier 2 Slow Path)
// ============================================================
// Phase 1: simulated fetch (copies from local sim-remote buffer).
// Phase 2+: real RDMA READ with ThreadBlockInVM + apth_rdma_wait.

oop G1BarrierSet::resolve_remote_fetch(RemoteHandle* h) {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();

  size_t word_size = h->eviction_word_size();

  HeapWord* dest = rmm->allocate_in_fcr(word_size);
  guarantee(dest != nullptr, "FCR allocation failed for fetch");

  // Fetch object bytes from remote via backend
  Klass* fetched_klass = rmm->fetch_remote_object(h, dest);

  if (fetched_klass == nullptr) {
    // Fetch failed — rollback
    uintptr_t sa = h->load_state_and_addr_acquire();
    h->set_remote(sa & REMOTE_HANDLE_ADDR_MASK);
    log_warning(gc)("C++ runtime fetch failed for handle " PTR_FORMAT, p2i(h));
    return nullptr;
  }

  // The raw bytes from remote may have stale header data (eviction-time mark
  // word, classification bits, etc.). Overwrite both the mark word and the
  // klass pointer with known-good values before the object becomes visible.
  cast_to_oop(dest)->set_mark(markWord::prototype());
  cast_to_oop(dest)->set_klass(fetched_klass);

  // Patch fetched object's fields using edge table (same as JRT slow path)
  rmm->patch_fetched_fields(h, dest);

  // Rekey handle table for new FCR address
  rmm->rekey_handle_on_fetch(h, (void*)dest);

  h->set_local_release(dest);
  return cast_to_oop(dest);
}

oop G1BarrierSet::wait_for_fetch(RemoteHandle* h) {
  int spins = 0;
  while (true) {
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    if (state == REMOTE_HANDLE_LOCAL) {
      return cast_to_oop(sa & REMOTE_HANDLE_ADDR_MASK);
    }
    if (state == REMOTE_HANDLE_DEAD || state == REMOTE_HANDLE_REMOTE) {
      return nullptr;
    }
    if (++spins > 1000) {
      os::naked_yield();
      spins = 0;
    }
  }
}

// ============================================================
// Full tagged oop resolution for VM context (JNI, runtime).
// ============================================================
// Same logic as oop_load_in_heap but callable from any VM context.
// Does NOT do thread state transitions — caller must be in VM.
oop G1BarrierSet::resolve_tagged_oop_in_vm(oop tagged) {
  uintptr_t v = cast_from_oop<uintptr_t>(tagged);
  if ((v & G1_OOP_TAG_MASK) == 0) return tagged;

  if (v & G1_OOP_INDIRECT_BIT) {
    RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
    int fetch_attempts = 0;
    while (true) {
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) {
        return cast_to_oop(sa & REMOTE_HANDLE_ADDR_MASK);
      } else if (state == REMOTE_HANDLE_DEAD) {
        return nullptr;
      } else if (state == REMOTE_HANDLE_REMOTE) {
        if (h->cas_remote_to_fetching()) {
          oop result = resolve_remote_fetch(h);
          if (result != nullptr) return result;
          fetch_attempts++;
          if (fetch_attempts >= 3) { h->set_dead(); return nullptr; }
          continue;
        } else {
          oop result = wait_for_fetch(h);
          if (result != nullptr) return result;
          continue;
        }
      } else {
        oop result = wait_for_fetch(h);
        if (result != nullptr) return result;
        continue;
      }
    }
  }

  // Unique/Direct: strip tags
  return cast_to_oop(v & G1_OOP_ADDR_MASK);
}

// Free function wrapper declared in g1RemoteOop.hpp
oop resolve_oop_full(oop tagged) {
  return G1BarrierSet::resolve_tagged_oop_in_vm(tagged);
}

// ============================================================
// Write Barrier: Managed Object Classification Check (Phase 2b)
// ============================================================
// Called from oop_store_in_heap when new_value is non-null.
// Checks the per-region bitmap for classification.
// Returns: Shared OOP (if managed+Shared), or original oop (otherwise).

oop G1BarrierSet::resolve_managed_store(oop new_value) {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  if (g1h == nullptr) return new_value;

  // Fast negative 1: not in heap
  if (!g1h->is_in(new_value)) {
    return new_value;
  }

  // Fast negative 2: region has no classified objects
  HeapRegion* r = g1h->heap_region_containing(new_value);
  if (r == nullptr || !r->has_classified_objects()) {
    return new_value;
  }

  // CRITICAL: check is_unlocked(). In locked/inflated states, the mark word
  // holds a pointer (BasicLock* or ObjectMonitor*), and bits 39-40 of that
  // pointer are part of the ADDRESS. On x86-64, stack addresses (0x7fff...)
  // have bit 39 = 1, which falsely matches remote_class_unique.
  markWord mw = new_value->mark_acquire();
  if (!mw.is_unlocked()) {
    return new_value;
  }
  uintptr_t cls = mw.remote_class();

  if (cls == markWord::remote_class_shared) {
    // Already Shared: find Handle and encode as Shared OOP.
    G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
    RemoteHandle* h = rmm->handle_for(new_value);
    if (h != nullptr) {
      // Verify Handle points to the correct object
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) {
        void* handle_target = (void*)(sa & REMOTE_HANDLE_ADDR_MASK);
        if (handle_target != cast_from_oop<void*>(new_value)) {
          log_warning(gc)("WB: Handle mismatch! obj=" PTR_FORMAT " handle_target=" PTR_FORMAT
                          " klass=%s",
                          p2i((void*)new_value), p2i(handle_target),
                          new_value->klass()->external_name());
          return new_value; // Don't tag — Handle points to wrong object
        }
      }
      return g1_make_shared_oop((void*)h);
    }
    // Handle not found for SHARED object — classification without Handle.
    // This happens for objects classified as SHARED during fixup but
    // not yet given a Handle (Handle creation is deferred to eviction).
    // Return clean oop — no tagging.
    return new_value;
  } else if (cls == markWord::remote_class_unique) {
    // Unique → Shared upgrade: allocate Handle, CAS mark word bits.
    G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
    RemoteHandleAllocBuffer hab;
    RemoteHandle* h = rmm->create_handle_for(new_value, &hab);
    // CAS mark word: UNIQUE → SHARED (retry for concurrent lock/hash CAS)
    markWord old_mw;
    do {
      old_mw = new_value->mark_acquire();
      if (!old_mw.is_unlocked()) break;
      markWord new_mw = old_mw.set_remote_class(markWord::remote_class_shared);
      if (new_value->cas_set_mark(new_mw, old_mw) == old_mw) break;
    } while (true);
    return g1_make_shared_oop((void*)h);
  }

  return new_value;
}

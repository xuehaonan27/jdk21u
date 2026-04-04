/*
 * Copyright (c) 2018, 2020, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "runtime/os.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "utilities/macros.hpp"

void G1BarrierSetRuntime::write_ref_array_pre_oop_entry(oop* dst, size_t length) {
  G1BarrierSet *bs = barrier_set_cast<G1BarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_array_pre(dst, length, false);
}

void G1BarrierSetRuntime::write_ref_array_pre_narrow_oop_entry(narrowOop* dst, size_t length) {
  G1BarrierSet *bs = barrier_set_cast<G1BarrierSet>(BarrierSet::barrier_set());
  bs->write_ref_array_pre(dst, length, false);
}

void G1BarrierSetRuntime::write_ref_array_post_entry(HeapWord* dst, size_t length) {
  G1BarrierSet *bs = barrier_set_cast<G1BarrierSet>(BarrierSet::barrier_set());
  bs->G1BarrierSet::write_ref_array(dst, length);
}

// G1 pre write barrier slowpath
JRT_LEAF(void, G1BarrierSetRuntime::write_ref_field_pre_entry(oopDesc* orig, JavaThread* thread))
  assert(thread == JavaThread::current(), "pre-condition");
  assert(orig != nullptr, "should be optimized out");
  assert(oopDesc::is_oop(orig, true /* ignore mark word */), "Error");
  // store the original value that was in the field reference
  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
  G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, orig);
JRT_END

// G1 post write barrier slowpath
JRT_LEAF(void, G1BarrierSetRuntime::write_ref_field_post_entry(volatile G1CardTable::CardValue* card_addr,
                                                               JavaThread* thread))
  assert(thread == JavaThread::current(), "pre-condition");
  G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thread);
  G1BarrierSet::dirty_card_queue_set().enqueue(queue, card_addr);
JRT_END

// Disaggregated memory: resolve a tagged oop to a clean local oop.
// Called from G1BarrierSetAssembler::load_at() when bit 63 (sign bit) is set.
// Handles LOCAL (fast), REMOTE (fetch via backend), and FETCHING (spin-wait).
// NOTE: JRT_LEAF — real RDMA backend currently does synchronous fetch here.
// Phase 5+: should use non-leaf with ThreadBlockInVM + apth_rdma_wait.
JRT_LEAF(oopDesc*, G1BarrierSetRuntime::resolve_tagged_oop(oopDesc* tagged))
  uintptr_t v = (uintptr_t)tagged;

  if (v & G1_OOP_INDIRECT_BIT) {
    // Shared OOP: follow Handle
    RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;

    if (state == REMOTE_HANDLE_LOCAL) {
      // Fast path: Handle is LOCAL
      return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
    }

    if (state == REMOTE_HANDLE_REMOTE) {
      // Object is remote — trigger simulated fetch.
      // CAS REMOTE → FETCHING (we win the fetch race)
      if (h->cas_remote_to_fetching()) {
        G1CollectedHeap* g1h = G1CollectedHeap::heap();
        G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();

        // Read object size from Handle metadata (stored at eviction time)
        size_t word_size = h->eviction_word_size();

        // Allocate in FCR region (GC-managed, proper lifecycle).
        // Falls back to os::malloc if FCR allocation fails (e.g., during
        // JRT_LEAF when we can't acquire Heap_lock for a new FCR region).
        HeapWord* dest = rmm->allocate_in_fcr(word_size);
        if (dest == nullptr) {
          // Fallback: C heap allocation (will leak, but prevents crash)
          dest = (HeapWord*)os::malloc(word_size * HeapWordSize, mtGC);
        }
        guarantee(dest != nullptr, "Failed to allocate fetch buffer");

        // Fetch object bytes from remote via backend (SIM/TCP/RDMA)
        rmm->fetch_remote_object(h, dest);

        // Publish: release-store LOCAL with new address
        h->set_local_release(dest);
        return (oopDesc*)dest;
      }
      // CAS failed: someone else is fetching. Fall through to FETCHING wait.
      sa = h->load_state_and_addr_acquire();
      state = sa & REMOTE_HANDLE_STATE_MASK;
    }

    if (state == REMOTE_HANDLE_FETCHING) {
      // Another thread is fetching. Spin-wait until LOCAL.
      int spins = 0;
      while (true) {
        sa = h->load_state_and_addr_acquire();
        state = sa & REMOTE_HANDLE_STATE_MASK;
        if (state == REMOTE_HANDLE_LOCAL) {
          return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
        }
        if (++spins > 1000) {
          os::naked_yield();
          spins = 0;
        }
      }
    }

    // Shouldn't reach here
    return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
  }

  // Unique/Direct OOP: strip tags
  return (oopDesc*)(v & G1_OOP_ADDR_MASK);
JRT_END

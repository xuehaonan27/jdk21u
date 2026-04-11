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
#include "runtime/threadWXSetters.inline.hpp"
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

// ============================================================
// LEAF fast path: resolve LOCAL Handles and Unique OOPs.
// Returns the ORIGINAL tagged oop for REMOTE/FETCHING — the caller
// detects bit 63 still set and calls resolve_tagged_oop_slow.
// ============================================================
JRT_LEAF(oopDesc*, G1BarrierSetRuntime::resolve_tagged_oop(oopDesc* tagged))
  uintptr_t v = (uintptr_t)tagged;
  if ((v >> 63) == 0) return tagged;  // Clean oop / null

  if (v & G1_OOP_INDIRECT_BIT) {
    RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    if (state == REMOTE_HANDLE_LOCAL) {
      oopDesc* resolved = (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);

      // Sampled hotness epoch update: stamp the object's mark word with
      // the current GC epoch. Sample 1 in 16 resolutions (cheap hash on addr).
      // Best-effort: skip if locked, skip on CAS failure.
      if (((uintptr_t)resolved & 0x78) == 0) {  // ~1/16 sampling
        markWord mw = resolved->mark_acquire();
        if (mw.is_unlocked()) {
          G1CollectedHeap* g1h = G1CollectedHeap::heap();
          uint32_t epoch = g1h->remote_memory_manager()->gc_epoch() & 0xF;
          if (mw.remote_epoch() != epoch) {
            markWord new_mw = mw.set_remote_epoch(epoch);
            // Best-effort CAS — skip on failure (another thread may have
            // updated hash or lock bits concurrently).
            resolved->cas_set_mark(new_mw, mw);
          }
        }
      }

      return resolved;
    }
    // REMOTE or FETCHING: return tagged oop unchanged for slow path.
    // With G1TagRefSites (no eviction), this should never happen.
    // If it does, the Handle was created incorrectly.
    guarantee(!G1TagRefSites || state == REMOTE_HANDLE_LOCAL,
              "resolve_tagged_oop: Shared Handle not LOCAL! tagged=" PTR_FORMAT
              " handle=" PTR_FORMAT " state_and_addr=" PTR_FORMAT " state=%lu",
              v, p2i(h), sa, state);
    return tagged;
  }

  // Unique/Direct: strip tags
  return (oopDesc*)(v & G1_OOP_ADDR_MASK);
JRT_END

// ============================================================
// NON-LEAF slow path: blocking fetch from remote storage.
// NOT JRT_ENTRY — manages thread state transition internally.
// Same calling convention as the leaf (single oopDesc* arg, result in rax).
// Callers: interpreter call_VM, C1 call_runtime_leaf, C2 stub raw call.
// ============================================================
// Non-leaf slow path for REMOTE Handle fetch.
// Same C calling convention as the leaf (single oopDesc* arg) for uniform
// calling from all tiers (interpreter, C1, C2, arraycopy).
// Internally transitions _thread_in_Java → _thread_in_vm via ThreadInVMfromJava.
// This enables Heap_lock acquisition for FCR allocation and ThreadBlockInVM
// for safepoint-aware blocking I/O.
//
// Safety: sets last_Java_frame BEFORE ThreadInVMfromJava to ensure GC can
// walk the stack even when called from C1's call_runtime_leaf (which doesn't
// set last_Java_frame). The frame pointer chain (rbp) is valid because
// all callers (interpreter templates, C1 compiled code, C2 stubs) maintain
// proper frame pointers.
oopDesc* G1BarrierSetRuntime::resolve_tagged_oop_slow(oopDesc* tagged) {
  uintptr_t v = (uintptr_t)tagged;

  // Fast path: clean oop / null — no thread transition needed.
  if ((v >> 63) == 0) return tagged;

  // Leaf resolve: handles LOCAL Handles + Unique OOPs without ThreadInVMfromJava.
  // This is the common case (99.99%) — no safepoint, no blocking.
  if (!(v & G1_OOP_INDIRECT_BIT)) {
    // Unique/Direct: strip tags
    return (oopDesc*)(v & G1_OOP_ADDR_MASK);
  }
  // Shared: follow Handle
  RemoteHandle* h_fast = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
  uintptr_t sa_fast = h_fast->load_state_and_addr_acquire();
  if ((sa_fast & REMOTE_HANDLE_STATE_MASK) == REMOTE_HANDLE_LOCAL) {
    return (oopDesc*)(sa_fast & REMOTE_HANDLE_ADDR_MASK);
  }

  // Slow path: REMOTE/FETCHING — requires ThreadInVMfromJava for blocking I/O.
  // This path is rare (only for actually-evicted objects).
  JavaThread* current = JavaThread::current();
  ThreadInVMfromJava tiv(current);
  v = (uintptr_t)tagged; // re-read (stable since tagged value doesn't change)
  // Re-check after transition (Handle may have been resolved by another thread)
  if (!(v & G1_OOP_INDIRECT_BIT)) {
    return (oopDesc*)(v & G1_OOP_ADDR_MASK);
  }

  RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;

  if (state == REMOTE_HANDLE_LOCAL) {
    return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
  }

  if (state == REMOTE_HANDLE_REMOTE) {
    if (h->cas_remote_to_fetching()) {
      G1CollectedHeap* g1h = G1CollectedHeap::heap();
      G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
      size_t word_size = h->eviction_word_size();

      // Allocate in FCR — JRT_ENTRY context can acquire Heap_lock.
      // No os::malloc fallback: all fetched objects go into proper G1 regions.
      HeapWord* dest = rmm->allocate_in_fcr(word_size);
      guarantee(dest != nullptr, "FCR allocation failed for fetch");

      // Fetch from remote backend with safepoint awareness.
      // ThreadBlockInVM allows the thread to participate in safepoints
      // during blocking I/O (TCP round-trip / RDMA READ).
      {
        ThreadBlockInVM tbivm(current);
        rmm->fetch_remote_object(h, dest);
      }

      // Patch fetched object's oop fields BEFORE publishing.
      // The fetched bytes contain oop values from eviction time — targets
      // may have moved since then. The sidecar edge table maps each field
      // to the target's Handle (which tracks current address).
      rmm->patch_fetched_fields(h, dest);

      h->set_local_release(dest);
      return (oopDesc*)dest;
    }
    sa = h->load_state_and_addr_acquire();
    state = sa & REMOTE_HANDLE_STATE_MASK;
  }

  if (state == REMOTE_HANDLE_FETCHING) {
    // Wait with safepoint awareness
    ThreadBlockInVM tbivm(current);
    while (true) {
      sa = h->load_state_and_addr_acquire();
      state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state == REMOTE_HANDLE_LOCAL) break;
      os::naked_yield();
    }
    return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
  }

  ShouldNotReachHere();
  return nullptr;
}

// JRT_ENTRY wrapper for interpreter call_VM (proper oop map + frame anchor).
JRT_ENTRY(oopDesc*, G1BarrierSetRuntime::resolve_tagged_oop_slow_vm(JavaThread* current, oopDesc* tagged))
  return resolve_tagged_oop_slow(tagged);
JRT_END

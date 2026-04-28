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
#include "gc/g1/g1RemoteBackend.hpp"
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
    // This is normal when UseRemoteExecutor is active (real eviction).
    // The slow path (resolve_tagged_oop_slow) handles REMOTE fetch.
    return tagged;
  }

  // Unique/Direct: strip tags
  return (oopDesc*)(v & G1_OOP_ADDR_MASK);
JRT_END

// ============================================================
// Shared fetch helpers — used by both safepoint-safe and
// non-safepointing slow paths to avoid logic duplication.
// ============================================================

class PostFetchValidateClosure : public BasicOopIterateClosure {
  G1CollectedHeap* _g1h;
  oop _obj;
  int _bad;
public:
  PostFetchValidateClosure(G1CollectedHeap* g1h, oop obj) : _g1h(g1h), _obj(obj), _bad(0) {}
  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;
    if ((raw >> 63) != 0) return;
    if (!_g1h->is_in((void*)raw)) {
      uint32_t off = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_obj));
      uintptr_t mw_lock = raw & 0x3;
      uintptr_t mw_age = (raw >> 3) & 0xF;
      const char* mw_hint = (mw_lock == 0x1 && raw > 0xFF)
                            ? " LOOKS-LIKE-MARKWORD" : "";
      log_warning(gc)("POST-FETCH CORRUPT: obj=" PTR_FORMAT " klass=%s offset=%u "
                      "field_val=0x%lx not in heap (lock=%lu age=%u)%s",
                      p2i((void*)_obj), _obj->klass()->external_name(), off,
                      (unsigned long)raw, (unsigned long)mw_lock,
                      (unsigned)mw_age, mw_hint);
      *(uintptr_t*)p = 0;
      _bad++;
      return;
    }
    oop target = cast_to_oop(raw);
    Klass* tk = target->klass_or_null();
    if (tk == nullptr) {
      uint32_t off = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_obj));
      log_warning(gc)("POST-FETCH CORRUPT: obj=" PTR_FORMAT " klass=%s offset=%u "
                      "target=" PTR_FORMAT " has null klass",
                      p2i((void*)_obj), _obj->klass()->external_name(), off, raw);
      *(uintptr_t*)p = 0;
      _bad++;
    }
  }
  virtual void do_oop(narrowOop* p) {}
  int bad() const { return _bad; }
};

// Fetch a REMOTE object, install it locally, and publish the Handle as LOCAL.
// Caller must have already CAS'd the Handle to FETCHING.
// Returns the local oop on success, nullptr on failure (Handle set to DEAD
// after max retries, or set back to REMOTE for caller to retry).
// *out_retry is set to true if the caller should re-enter the state machine.
static oopDesc* fetch_and_install(RemoteHandle* h, int& fetch_attempts, bool* out_retry) {
  *out_retry = false;
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
  size_t word_size = h->eviction_word_size();

  HeapWord* dest = rmm->allocate_in_fcr(word_size);
  guarantee(dest != nullptr, "FCR allocation failed for fetch");

  // Zero-fill before fetch so any partial/wrong copy is detectable
  memset(dest, 0, word_size * HeapWordSize);

  Klass* fetched_klass = rmm->fetch_remote_object(h, dest);

  if (fetched_klass == nullptr) {
    fetch_attempts++;
    if (fetch_attempts >= 3) {
      h->set_dead();
      log_warning(gc)("Fetch failed %d times for handle " PTR_FORMAT " — marking DEAD",
                      fetch_attempts, p2i(h));
      return nullptr;
    }
    uintptr_t sa2 = h->load_state_and_addr_acquire();
    h->set_remote(sa2 & REMOTE_HANDLE_ADDR_MASK);
    log_warning(gc)("Fetch failed for handle " PTR_FORMAT " — retry %d/3",
                    p2i(h), fetch_attempts);
    *out_retry = true;
    return nullptr;
  }

  // Capture raw fetched header for diagnostics
  uintptr_t raw_mark_after_fetch = *(uintptr_t*)dest;
  uintptr_t raw_klass_after_fetch = *((uintptr_t*)dest + 1);

  {
    markWord fetched_mw = cast_to_oop(dest)->mark();
    if (fetched_mw.is_unlocked()) {
      // Preserve unlocked mark word from remote — keeps identity hash + age
    } else {
      // Stale lock/monitor pointer from eviction time — cannot dereference
      cast_to_oop(dest)->set_mark(markWord::prototype());
      log_trace(gc)("Fetch: normalized locked mark 0x%lx", (unsigned long)fetched_mw.value());
    }
  }
  cast_to_oop(dest)->set_klass(fetched_klass);
  rmm->patch_fetched_fields(h, dest);

  {
    oop fetched = cast_to_oop(dest);
    PostFetchValidateClosure vcl(g1h, fetched);
    fetched->oop_iterate(&vcl);
    if (vcl.bad() > 0) {
      log_warning(gc)("POST-FETCH: %d corrupt fields nulled in obj=" PTR_FORMAT
                      " klass=%s (fetched_mark=0x%lx fetched_klass=0x%lx ws=%zu)",
                      vcl.bad(), p2i(dest), fetched->klass()->external_name(),
                      (unsigned long)raw_mark_after_fetch,
                      (unsigned long)raw_klass_after_fetch, word_size);
    }
  }

  // Post-patch integrity: verify header wasn't scribbled during patch
  {
    Klass* final_klass = cast_to_oop(dest)->klass_or_null();
    uintptr_t final_mark = *(uintptr_t*)dest;
    if (final_klass != fetched_klass) {
      log_warning(gc)("FETCH-SCRIBBLE: klass changed from %s (0x%lx) to 0x%lx during"
                      " patch of handle " PTR_FORMAT " dest=" PTR_FORMAT " ws=%zu",
                      fetched_klass->external_name(), (unsigned long)(uintptr_t)fetched_klass,
                      (unsigned long)(uintptr_t)final_klass, p2i(h), p2i(dest), word_size);
    }
    if ((final_mark & 0x3) != (raw_mark_after_fetch & 0x3)) {
      log_warning(gc)("FETCH-SCRIBBLE: mark word lock bits changed from 0x%lx to 0x%lx"
                      " during patch of handle " PTR_FORMAT,
                      (unsigned long)raw_mark_after_fetch, (unsigned long)final_mark, p2i(h));
    }
  }

  rmm->rekey_handle_on_fetch(h, (void*)dest);

  uintptr_t handle_id = (uintptr_t)h;
  rmm->backend()->localize_batch(&handle_id, 1);

  h->set_local_release(dest);
  return (oopDesc*)dest;
}

// Shared fast-path checks for both slow-path variants.
// Returns non-null oop if resolved without needing the state machine.
// Returns nullptr if the caller must enter the state machine.
// Sets *handle_out to the RemoteHandle* if state machine is needed.
static oopDesc* resolve_fast_checks(oopDesc* tagged, RemoteHandle** handle_out) {
  uintptr_t v = (uintptr_t)tagged;
  if ((v >> 63) == 0) return tagged;
  if (!(v & G1_OOP_INDIRECT_BIT)) return (oopDesc*)(v & G1_OOP_ADDR_MASK);

  RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
  if (state == REMOTE_HANDLE_LOCAL) return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
  if (state == REMOTE_HANDLE_DEAD) return nullptr;

  *handle_out = h;
  return nullptr;
}

// ============================================================
// Safepoint-safe slow path: REMOTE fetch with thread transitions.
// Called from interpreter C++ barrier (oop_load_in_heap) where
// the interpreter frame anchor is set and GC can walk the stack.
// ============================================================
oopDesc* G1BarrierSetRuntime::resolve_tagged_oop_slow(oopDesc* tagged) {
  RemoteHandle* h = nullptr;
  oopDesc* fast = resolve_fast_checks(tagged, &h);
  if (h == nullptr) return fast;

  JavaThread* current = JavaThread::current();
  ThreadInVMfromJava tiv(current);

  int fetch_attempts = 0;
  while (true) {
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;

    if (state == REMOTE_HANDLE_LOCAL) return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
    if (state == REMOTE_HANDLE_DEAD)  return nullptr;

    if (state == REMOTE_HANDLE_REMOTE) {
      if (h->cas_remote_to_fetching()) {
        bool retry = false;
        oopDesc* result;
        {
          ThreadBlockInVM tbivm(current);
          result = fetch_and_install(h, fetch_attempts, &retry);
        }
        if (retry) continue;
        return result;
      }
      continue;
    }

    if (state == REMOTE_HANDLE_FETCHING) {
      ThreadBlockInVM tbivm(current);
      // Bounded wait — same rationale as the no_safepoint variant below.
      // Yield-based loop, so use lower thresholds (~10K yields ≈ 1s).
      const uint64_t YieldWarn = 10000;
      const uint64_t YieldHard = 100000;
      uint64_t yields = 0;
      while (true) {
        sa = h->load_state_and_addr_acquire();
        state = sa & REMOTE_HANDLE_STATE_MASK;
        if (state != REMOTE_HANDLE_FETCHING) break;
        if (++yields == YieldWarn) {
          log_warning(gc)("FETCHING wait (slow path) exceeded %llu yields for handle " PTR_FORMAT
                          " (slot=%lu) — fetcher may be stuck",
                          (unsigned long long)yields, p2i(h),
                          (unsigned long)(sa & REMOTE_HANDLE_ADDR_MASK));
        }
        if (yields >= YieldHard) {
          log_warning(gc)("FETCHING wait HARD LIMIT (%llu yields) for handle " PTR_FORMAT
                          " — giving up, returning nullptr",
                          (unsigned long long)yields, p2i(h));
          return nullptr;
        }
        os::naked_yield();
      }
      continue;
    }

    log_warning(gc)("resolve_tagged_oop_slow: unexpected state 0x%lx for handle " PTR_FORMAT,
                    (unsigned long)state, p2i(h));
    os::naked_yield();
  }
}

// ============================================================
// Non-safepointing slow path: REMOTE fetch WITHOUT thread transitions.
// Called from C1/C2/assembler barrier stubs where the OopMap does not
// describe all live oop registers. By staying in _thread_in_Java
// and not allowing safepoints, the empty OopMap is harmless.
// Blocking I/O (RDMA/TCP) adds at most ~50us to safepoint initiation.
// ============================================================
oopDesc* G1BarrierSetRuntime::resolve_tagged_oop_no_safepoint(oopDesc* tagged) {
  RemoteHandle* h = nullptr;
  oopDesc* fast = resolve_fast_checks(tagged, &h);
  if (h == nullptr) return fast;

  int fetch_attempts = 0;
  while (true) {
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;

    if (state == REMOTE_HANDLE_LOCAL) return (oopDesc*)(sa & REMOTE_HANDLE_ADDR_MASK);
    if (state == REMOTE_HANDLE_DEAD)  return nullptr;

    if (state == REMOTE_HANDLE_REMOTE) {
      if (h->cas_remote_to_fetching()) {
        bool retry = false;
        oopDesc* result = fetch_and_install(h, fetch_attempts, &retry);
        if (retry) continue;
        return result;
      }
      continue;
    }

    if (state == REMOTE_HANDLE_FETCHING) {
      // Bounded wait with diagnostic logging. Without the bound, threads can
      // livelock here if the fetcher itself is stuck (RDMA never completes,
      // GC takes the global lock, etc.). After SpinThreshold iterations, log
      // the suspect handle so we can investigate. After a much larger limit,
      // give up — return nullptr so Java code can throw NPE rather than hang
      // the whole JVM. The fetcher's eventual set_local_release would then
      // race against our state, but the handle's CAS protects integrity.
      const uint64_t SpinThreshold = 1ULL << 24;       // ~16M spins
      const uint64_t HardLimit     = 1ULL << 30;       // ~1B spins (~10s)
      uint64_t spins = 0;
      while (true) {
        sa = h->load_state_and_addr_acquire();
        state = sa & REMOTE_HANDLE_STATE_MASK;
        if (state != REMOTE_HANDLE_FETCHING) break;
        if (++spins == SpinThreshold) {
          log_warning(gc)("FETCHING wait exceeded %llu spins for handle " PTR_FORMAT
                          " (slot=%lu) — fetcher may be stuck",
                          (unsigned long long)spins, p2i(h),
                          (unsigned long)(sa & REMOTE_HANDLE_ADDR_MASK));
        }
        if (spins >= HardLimit) {
          log_warning(gc)("FETCHING wait HARD LIMIT (%llu spins) for handle " PTR_FORMAT
                          " — giving up, returning nullptr",
                          (unsigned long long)spins, p2i(h));
          return nullptr;
        }
        SpinPause();
      }
      continue;
    }

    SpinPause();
  }
}


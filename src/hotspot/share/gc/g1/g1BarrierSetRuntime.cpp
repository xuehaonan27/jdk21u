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
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1RemoteBackend.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1RemoteOop.hpp"
#include "gc/g1/g1YoungCollector.hpp"
#include "gc/g1/heapRegion.hpp"
#include "runtime/atomic.hpp"
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

  oop obj = resolve_oop_raw(cast_to_oop(orig));
  if (obj == nullptr ||
      !g1_remote_oop_is_aligned(cast_from_oop<uintptr_t>(obj))) {
    return;
  }

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  if (!g1h->is_in(obj)) {
    return;
  }

  if (LocalMemoryRatio < 100 || G1TagRefSites ||
      G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0) {
    HeapRegion* hr = g1h->heap_region_containing(obj);
    if (hr == nullptr || hr->is_free() || hr->is_evict_guarded()) {
      return;
    }
  }

  assert(oopDesc::is_oop(obj, true /* ignore mark word */), "Error");
  // store the original value that was in the field reference
  SATBMarkQueue& queue = G1ThreadLocalData::satb_mark_queue(thread);
  G1BarrierSet::satb_mark_queue_set().enqueue_known_active(queue, obj);
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

static bool remote_resolve_enabled() {
  return LocalMemoryRatio < 100 || G1TagRefSites ||
         G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0;
}

static bool local_handle_addr_is_stale(G1CollectedHeap* g1h, uintptr_t addr,
                                       HeapRegion** hr_out) {
  if (hr_out != nullptr) {
    *hr_out = nullptr;
  }
  if (addr == 0 || g1h == nullptr || !g1h->is_in_reserved((void*)addr)) {
    return true;
  }

  HeapRegion* hr = g1h->heap_region_containing_or_null((void*)addr);
  if (hr_out != nullptr) {
    *hr_out = hr;
  }
  if (hr == nullptr || hr->is_free() || hr->is_evict_guarded()) {
    return true;
  }
  if (!g1h->is_in((void*)addr)) {
    return true;
  }

  oop obj = cast_to_oop((HeapWord*)addr);
  Klass* k = obj->klass_or_null();
  return k == nullptr || G1CollectedHeap::is_obj_filler(obj);
}

static const char* stale_local_addr_reason(G1CollectedHeap* g1h, uintptr_t addr,
                                           HeapRegion* hr) {
  if (addr == 0) return "NULL";
  if (g1h == nullptr) return "NO-HEAP";
  if (!g1h->is_in_reserved((void*)addr)) return "NOT-IN-HEAP";
  if (hr == nullptr) return "NO-HR";
  if (hr->is_evict_guarded()) return "GUARDED";
  if (hr->is_free()) return "FREE";
  if (!g1h->is_in((void*)addr)) return "OUTSIDE-LIVE";

  oop obj = cast_to_oop((HeapWord*)addr);
  Klass* k = obj->klass_or_null();
  if (k == nullptr) return "NULL-KLASS";
  if (G1CollectedHeap::is_obj_filler(obj)) return "FILLER";
  return "STALE";
}

static oopDesc* resolve_local_handle_addr(RemoteHandle* h, uintptr_t addr,
                                          RemoteHandle** redirect_out,
                                          const char* caller) {
  if (redirect_out != nullptr) {
    *redirect_out = nullptr;
  }
  if (!remote_resolve_enabled()) {
    return (oopDesc*)addr;
  }

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  HeapRegion* hr = nullptr;
  if (!local_handle_addr_is_stale(g1h, addr, &hr)) {
    return (oopDesc*)addr;
  }

  G1RemoteMemoryManager* rmm = g1h == nullptr ? nullptr : g1h->remote_memory_manager();
  RemoteHandle* alt = rmm == nullptr ? nullptr : rmm->handle_for_addr_any_state(addr);
  if (alt != nullptr && alt != h) {
    uintptr_t alt_sa = alt->load_state_and_addr_acquire();
    uintptr_t alt_state = alt_sa & REMOTE_HANDLE_STATE_MASK;
    if (rmm != nullptr) {
      rmm->record_resolve_fast_state(alt_state);
    }

    if (alt_state == REMOTE_HANDLE_LOCAL) {
      uintptr_t alt_addr = alt_sa & REMOTE_HANDLE_ADDR_MASK;
      HeapRegion* alt_hr = nullptr;
      if (!local_handle_addr_is_stale(g1h, alt_addr, &alt_hr)) {
        log_debug(gc)("%s: redirected stale LOCAL handle " PTR_FORMAT
                      " addr=" PTR_FORMAT " to duplicate LOCAL handle "
                      PTR_FORMAT " addr=" PTR_FORMAT,
                      caller, p2i(h), p2i((void*)addr),
                      p2i(alt), p2i((void*)alt_addr));
        return (oopDesc*)alt_addr;
      }
    } else if (alt_state == REMOTE_HANDLE_REMOTE ||
               alt_state == REMOTE_HANDLE_FETCHING) {
      if (redirect_out != nullptr) {
        *redirect_out = alt;
      }
      log_debug(gc)("%s: redirected stale LOCAL handle " PTR_FORMAT
                    " addr=" PTR_FORMAT " to duplicate remote handle "
                    PTR_FORMAT " state=0x%lx",
                    caller, p2i(h), p2i((void*)addr), p2i(alt),
                    (unsigned long)alt_state);
      return nullptr;
    }
  }

  log_warning(gc)("%s: stale LOCAL handle " PTR_FORMAT " addr=" PTR_FORMAT
                  " points into %s region %u; returning nullptr",
                  caller, p2i(h), p2i((void*)addr),
                  stale_local_addr_reason(g1h, addr, hr),
                  hr == nullptr ? 9999 : hr->hrm_index());
  return nullptr;
}

static void sample_touch_hotness(oopDesc* obj, G1RemoteMemoryManager* rmm) {
  if (obj == nullptr || rmm == nullptr) return;

  // Sampled hotness epoch update: stamp the object's mark word with the
  // current GC epoch. Sample 1 in 16 resolutions (cheap hash on addr).
  // Best-effort: skip if locked, skip on CAS failure.
  if (((uintptr_t)obj & 0x78) == 0) {
    markWord mw = obj->mark_acquire();
    if (mw.is_unlocked()) {
      uint32_t epoch = rmm->gc_epoch() & 0xF;
      if (mw.remote_epoch() != epoch) {
        markWord new_mw = mw.set_remote_epoch(epoch);
        obj->cas_set_mark(new_mw, mw);
      }
    }
  }
}

JRT_LEAF(oopDesc*, G1BarrierSetRuntime::resolve_tagged_oop(oopDesc* tagged))
  uintptr_t v = (uintptr_t)tagged;
  if ((v >> 63) == 0) {
    if (v == 0) return tagged;

    // Remote eviction can leave a clean pre-eviction oop in Java/native state
    // that is not itself tagged. If it points into a guarded/free region, turn
    // it back into a shared handle so the caller's slow path can fetch it.
    if (LocalMemoryRatio < 100 || G1TagRefSites ||
        G1SimulateRemoteEviction || G1RemoteEvictionThreshold > 0) {
      G1CollectedHeap* g1h = G1CollectedHeap::heap();
      if (g1h != nullptr && g1h->is_in_reserved((void*)v)) {
        HeapRegion* hr = g1h->heap_region_containing_or_null((void*)v);
        bool stale_raw = hr == nullptr || hr->is_free() || hr->is_evict_guarded() ||
                         !g1h->is_in((void*)v);
        if (!stale_raw) {
          oop obj = cast_to_oop((HeapWord*)v);
          Klass* k = obj->klass_or_null();
          stale_raw = k == nullptr || G1CollectedHeap::is_obj_filler(obj);
        }
        if (stale_raw) {
          G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
          RemoteHandle* h = rmm == nullptr ? nullptr : rmm->handle_for_addr_any_state(v);
          if (h != nullptr) {
            uintptr_t sa = h->load_state_and_addr_acquire();
            uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
            if (rmm != nullptr) {
              rmm->record_resolve_fast_state(state);
            }
            if (state != REMOTE_HANDLE_DEAD) {
              return (oopDesc*)(G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h);
            }
          }
          return nullptr;
        }
      }
    }
    return tagged;
  }

  if (v & G1_OOP_INDIRECT_BIT) {
    RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    G1RemoteMemoryManager* rmm = G1CollectedHeap::heap()->remote_memory_manager();
    if (rmm != nullptr && state != REMOTE_HANDLE_LOCAL) {
      rmm->record_resolve_fast_state(state);
    }
    if (state == REMOTE_HANDLE_LOCAL) {
      uintptr_t resolved_addr = sa & REMOTE_HANDLE_ADDR_MASK;
      RemoteHandle* redirect = nullptr;
      oopDesc* resolved = resolve_local_handle_addr(h, resolved_addr, &redirect,
                                                    "resolve_tagged_oop");
      if (redirect != nullptr) {
        return (oopDesc*)(G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)redirect);
      }
      if (resolved == nullptr) {
        return nullptr;
      }

      sample_touch_hotness(resolved, rmm);

      return resolved;
    }
    // REMOTE or FETCHING: return tagged oop unchanged for slow path.
    // This is normal when remote eviction/tagged refs are active.
    // The slow path (resolve_tagged_oop_slow) handles REMOTE fetch.
    return tagged;
  }

  // Unique/Direct: strip tags
  oopDesc* resolved = (oopDesc*)(v & G1_OOP_ADDR_MASK);
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h == nullptr ? nullptr : g1h->remote_memory_manager();
  sample_touch_hotness(resolved, rmm);
  return resolved;
JRT_END

JRT_LEAF(oopDesc*, G1BarrierSetRuntime::resolve_tagged_oop_with_hint(oopDesc* tagged,
                                                                      uint32_t access_hint))
  (void)access_hint;
  return G1BarrierSetRuntime::resolve_tagged_oop(tagged);
JRT_END

// ============================================================
// Shared fetch helpers — used by both safepoint-safe and
// non-safepointing slow paths to avoid logic duplication.
// ============================================================

class PostFetchValidateClosure : public BasicOopIterateClosure {
  G1CollectedHeap* _g1h;
  G1RemoteMemoryManager* _rmm;
  oop _obj;
  int _bad;
  int _stale_patched;
  int _stale_nulled;

  const char* obj_klass_name() const {
    Klass* k = _obj->klass_or_null();
    return k == nullptr ? "<null-klass>" : k->external_name();
  }

  void log_outside_heap(oop* p, uintptr_t raw) {
    uint32_t off = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_obj));
    uintptr_t mw_lock = raw & 0x3;
    uintptr_t mw_age = (raw >> 3) & 0xF;
    const char* mw_hint = (mw_lock == 0x1 && raw > 0xFF)
                          ? " LOOKS-LIKE-MARKWORD" : "";
    log_warning(gc)("POST-FETCH CORRUPT: obj=" PTR_FORMAT " klass=%s offset=%u "
                    "field_val=0x%lx not in heap (lock=%lu age=%u)%s",
                    p2i((void*)_obj), obj_klass_name(), off,
                    (unsigned long)raw, (unsigned long)mw_lock,
                    (unsigned)mw_age, mw_hint);
  }

  void repair_stale(oop* p, uintptr_t raw, HeapRegion* hr, const char* reason = nullptr) {
    uint32_t off = (uint32_t)((uintptr_t)p - cast_from_oop<uintptr_t>(_obj));
    const char* target_state = reason != nullptr ? reason : stale_local_addr_reason(_g1h, raw, hr);
    RemoteHandle* h = _rmm == nullptr ? nullptr : _rmm->handle_for_addr_any_state(raw);
    if (h != nullptr) {
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      if (state != REMOTE_HANDLE_DEAD) {
        *(uintptr_t*)p = G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT | (uintptr_t)h;
        _stale_patched++;
        if (_stale_patched <= 20) {
          log_warning(gc)("POST-FETCH STALE-FIELD: obj=" PTR_FORMAT " klass=%s offset=%u "
                          "raw=" PTR_FORMAT " -> shared handle=" PTR_FORMAT
                          " state=0x%lx target=%s region=%u",
                          p2i((void*)_obj), obj_klass_name(), off,
                          p2i((void*)raw), p2i(h), (unsigned long)state,
                          target_state,
                          hr == nullptr ? 9999 : hr->hrm_index());
        }
        return;
      }
    }

    *(uintptr_t*)p = 0;
    _stale_nulled++;
    _bad++;
    if (_stale_nulled <= 20) {
      log_warning(gc)("POST-FETCH STALE-FIELD: obj=" PTR_FORMAT " klass=%s offset=%u "
                      "raw=" PTR_FORMAT " in %s region %u has no live handle; nulled",
                      p2i((void*)_obj), obj_klass_name(), off,
                      p2i((void*)raw),
                      target_state,
                      hr == nullptr ? 9999 : hr->hrm_index());
    }
  }

public:
  PostFetchValidateClosure(G1CollectedHeap* g1h, oop obj)
    : _g1h(g1h), _rmm(g1h->remote_memory_manager()), _obj(obj),
      _bad(0), _stale_patched(0), _stale_nulled(0) {}

  virtual void do_oop(oop* p) {
    uintptr_t raw = *(uintptr_t*)p;
    if (raw == 0) return;
    if ((raw >> 63) != 0) return;

    if (!_g1h->is_in_reserved((void*)raw)) {
      log_outside_heap(p, raw);
      *(uintptr_t*)p = 0;
      _bad++;
      return;
    }

    HeapRegion* hr = _g1h->heap_region_containing_or_null((void*)raw);
    if (hr == nullptr || hr->is_free() || hr->is_evict_guarded()) {
      repair_stale(p, raw, hr);
      return;
    }

    if (!_g1h->is_in((void*)raw)) {
      log_outside_heap(p, raw);
      *(uintptr_t*)p = 0;
      _bad++;
      return;
    }

    oop target = cast_to_oop(raw);
    Klass* tk = target->klass_or_null();
    if (tk == nullptr) {
      repair_stale(p, raw, hr, "NULL-KLASS");
      return;
    }
    if (G1CollectedHeap::is_obj_filler(target)) {
      repair_stale(p, raw, hr, "FILLER");
    }
  }
  virtual void do_oop(narrowOop* p) {}
  int bad() const { return _bad; }
  int stale_patched() const { return _stale_patched; }
  int stale_nulled() const { return _stale_nulled; }
};

static void dirty_fetched_object_cards(G1CollectedHeap* g1h, HeapWord* start, size_t word_size) {
  if (word_size == 0) return;
  G1CardTable* ct = g1h->card_table();
  G1DirtyCardQueueSet& qset = G1BarrierSet::dirty_card_queue_set();
  Thread* thr = Thread::current();
  G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thr);
  CardTable::CardValue* first = ct->byte_for(start);
  CardTable::CardValue* last = ct->byte_for(start + word_size - 1);
  for (CardTable::CardValue* card = first; card <= last; card++) {
    if (*card != G1CardTable::g1_young_card_val()) {
      *card = G1CardTable::dirty_card_val();
      qset.enqueue(queue, card);
    }
  }
}

static oopDesc* finish_fetched_object(G1CollectedHeap* g1h,
                                      G1RemoteMemoryManager* rmm,
                                      RemoteHandle* h,
                                      HeapWord* dest,
                                      Klass* fetched_klass,
                                      size_t word_size) {
  uintptr_t raw_mark_after_fetch = *(uintptr_t*)dest;
  uintptr_t raw_klass_after_fetch = *((uintptr_t*)dest + 1);

  {
    markWord fetched_mw = cast_to_oop(dest)->mark();
    if (!fetched_mw.is_unlocked()) {
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
    if (vcl.stale_patched() > 0) {
      dirty_fetched_object_cards(g1h, dest, word_size);
    }
    if (vcl.bad() > 0 || vcl.stale_patched() > 0) {
      log_warning(gc)("POST-FETCH: %d corrupt/stale fields nulled, %d stale fields patched "
                      "in obj=" PTR_FORMAT " klass=%s (fetched_mark=0x%lx "
                      "fetched_klass=0x%lx ws=%zu)",
                      vcl.bad(), vcl.stale_patched(), p2i(dest),
                      fetched->klass()->external_name(),
                      (unsigned long)raw_mark_after_fetch,
                      (unsigned long)raw_klass_after_fetch, word_size);
    }
  }

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
  return (oopDesc*)dest;
}

static const uint G1RemoteFetchBatchHardCap = 256;
static volatile int g1_remote_fetch_batch_disabled = 0;

struct RemotePrefetchCacheEntry {
  RemoteHandle* handle;
  size_t slot_id;
  Klass* klass;
  size_t word_size;
  uint8_t* bytes;
  size_t byte_size;
  uint64_t stamp;
};

// Spark and small-object workloads can prefetch many tiny objects: the byte
// cap can still be almost empty while a small entry table churns. Keep enough
// slots so the byte cap, not hash-table occupancy, is usually the limiter.
static const uint RemotePrefetchCacheSlots = 131072;
static const uint RemotePrefetchCacheProbeLimit = 64;
static const size_t RemotePrefetchCacheMaxBytes = 64 * 1024 * 1024;
static const size_t RemotePrefetchCacheMaxObjectBytes = 16 * 1024;
static volatile int g1_remote_prefetch_cache_lock = 0;
static RemotePrefetchCacheEntry* g1_remote_prefetch_cache = nullptr;
static size_t g1_remote_prefetch_cache_bytes = 0;
static uint64_t g1_remote_prefetch_cache_hits = 0;
static uint64_t g1_remote_prefetch_cache_stores = 0;
static uint64_t g1_remote_prefetch_cache_evictions = 0;
static uint64_t g1_remote_prefetch_cache_drops = 0;
static uint g1_remote_prefetch_cache_evict_cursor = 0;
static uint g1_remote_fetch_batch_effective_logged = 0;
static uint64_t g1_remote_fetch_batch_policy_log_next_stores = 0;
static volatile uint64_t g1_remote_prefetch_pressure_checks = 0;
static volatile int g1_remote_prefetch_pressure_lock = 0;
static volatile int g1_remote_prefetch_pressure_has_sample = 0;
static volatile int g1_remote_prefetch_pressure_level = 0;
static volatile int g1_remote_prefetch_pressure_logged_level = -1;
static volatile uint64_t g1_remote_prefetch_eager_suppressed = 0;
static volatile uint64_t g1_remote_prefetch_batch_suppressed = 0;
static volatile uint64_t g1_remote_prefetch_cache_suppressed = 0;

enum RemotePrefetchPressureLevel {
  RemotePrefetchPressureOk = 0,
  RemotePrefetchPressureOverTarget = 1,
  RemotePrefetchPressureOverTier2 = 2,
  RemotePrefetchPressureOverTier3 = 3
};

static const uint64_t RemotePrefetchPressureSamplePeriod = 1024;

static const char* remote_prefetch_pressure_level_name(int level) {
  switch (level) {
    case RemotePrefetchPressureOverTier3:
      return "over-tier3";
    case RemotePrefetchPressureOverTier2:
      return "over-tier2";
    case RemotePrefetchPressureOverTarget:
      return "over-target";
    default:
      return "ok";
  }
}

static const char* remote_prefetch_pressure_action(int level) {
  if (level >= RemotePrefetchPressureOverTier2) {
    return "demand-only";
  }
  if (level >= RemotePrefetchPressureOverTarget) {
    return "cache-only";
  }
  return "eager-allowed";
}

static bool remote_prefetch_read_cgroup_pressure(size_t* usage,
                                                 size_t* capacity,
                                                 size_t* total_usage,
                                                 size_t* cache_usage) {
  if (!G1RemoteUseCgroupPressure || LocalMemoryRatio >= 100) {
    return false;
  }

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  if (g1h == nullptr || LocalMemoryRatio == 0) {
    return false;
  }

  size_t local_capacity = (g1h->max_capacity() * (size_t)LocalMemoryRatio) / 100;
  if (local_capacity == 0) {
    return false;
  }

  return g1_remote_read_cgroup_pressure(local_capacity, usage, capacity,
                                        total_usage, cache_usage);
}

static int remote_prefetch_sample_pressure_level() {
  if (!G1RemoteUseCgroupPressure || LocalMemoryRatio >= 100) {
    return RemotePrefetchPressureOk;
  }

  uint64_t check = Atomic::add(&g1_remote_prefetch_pressure_checks,
                               (uint64_t)1);
  if (Atomic::load(&g1_remote_prefetch_pressure_has_sample) != 0 &&
      (check % RemotePrefetchPressureSamplePeriod) != 1) {
    return Atomic::load(&g1_remote_prefetch_pressure_level);
  }

  if (Atomic::cmpxchg(&g1_remote_prefetch_pressure_lock, 0, 1) != 0) {
    return Atomic::load(&g1_remote_prefetch_pressure_level);
  }

  size_t usage = 0;
  size_t capacity = 0;
  size_t total_usage = 0;
  size_t cache_usage = 0;
  bool has_pressure =
    remote_prefetch_read_cgroup_pressure(&usage, &capacity,
                                         &total_usage, &cache_usage);
  if (!has_pressure) {
    Atomic::release_store(&g1_remote_prefetch_pressure_has_sample, 1);
    Atomic::release_store(&g1_remote_prefetch_pressure_lock, 0);
    return Atomic::load(&g1_remote_prefetch_pressure_level);
  }

  int level = RemotePrefetchPressureOk;
  if (capacity > 0) {
    size_t pct = (usage * 100) / capacity;
    if (pct >= (size_t)G1RemoteTier3Percent) {
      level = RemotePrefetchPressureOverTier3;
    } else if (pct >= (size_t)G1RemoteTier2Percent) {
      level = RemotePrefetchPressureOverTier2;
    } else if (pct >= (size_t)G1RemoteTier2TargetPercent) {
      level = RemotePrefetchPressureOverTarget;
    }
  }

  Atomic::release_store(&g1_remote_prefetch_pressure_level, level);
  Atomic::release_store(&g1_remote_prefetch_pressure_has_sample, 1);

  if (Atomic::load(&g1_remote_prefetch_pressure_logged_level) != level) {
    Atomic::release_store(&g1_remote_prefetch_pressure_logged_level, level);
    log_info(gc)("Remote prefetch pressure policy: level=%s action=%s "
                 "cgroup_anon=" SIZE_FORMAT "MB/" SIZE_FORMAT
                 "MB %.1f%% total=" SIZE_FORMAT "MB cache=" SIZE_FORMAT
                 "MB target=%u%% T2=%u%% T3=%u%%",
                 remote_prefetch_pressure_level_name(level),
                 remote_prefetch_pressure_action(level),
                 usage / M,
                 capacity / M,
                 capacity == 0 ? 0.0 :
                   ((double)usage * 100.0) / (double)capacity,
                 total_usage / M,
                 cache_usage / M,
                 G1RemoteTier2TargetPercent,
                 G1RemoteTier2Percent,
                 G1RemoteTier3Percent);
  }

  Atomic::release_store(&g1_remote_prefetch_pressure_lock, 0);
  return level;
}

static bool remote_prefetch_budget_allows_eager_install() {
  return remote_prefetch_sample_pressure_level() <
         RemotePrefetchPressureOverTarget;
}

static bool remote_prefetch_budget_allows_cache_store() {
  return remote_prefetch_sample_pressure_level() <
         RemotePrefetchPressureOverTier2;
}

static void remote_prefetch_cache_lock() {
  while (Atomic::cmpxchg(&g1_remote_prefetch_cache_lock, 0, 1) != 0) {
    SpinPause();
  }
}

static void remote_prefetch_cache_unlock() {
  Atomic::release_store(&g1_remote_prefetch_cache_lock, 0);
}

static void remote_prefetch_cache_free_entry_locked(uint idx) {
  RemotePrefetchCacheEntry& e = g1_remote_prefetch_cache[idx];
  if (e.bytes != nullptr) {
    os::free(e.bytes);
    e.bytes = nullptr;
  }
  if (g1_remote_prefetch_cache_bytes >= e.byte_size) {
    g1_remote_prefetch_cache_bytes -= e.byte_size;
  } else {
    g1_remote_prefetch_cache_bytes = 0;
  }
  e.handle = nullptr;
  e.slot_id = 0;
  e.klass = nullptr;
  e.word_size = 0;
  e.byte_size = 0;
  e.stamp = 0;
}

static void remote_prefetch_cache_clear_locked() {
  if (g1_remote_prefetch_cache == nullptr) {
    return;
  }
  for (uint i = 0; i < RemotePrefetchCacheSlots; i++) {
    if (g1_remote_prefetch_cache[i].handle != nullptr) {
      remote_prefetch_cache_free_entry_locked(i);
    }
  }
  g1_remote_prefetch_cache_bytes = 0;
  g1_remote_prefetch_cache_evict_cursor = 0;
}

static bool remote_prefetch_cache_ensure_locked() {
  if (g1_remote_prefetch_cache != nullptr) {
    return true;
  }
  size_t bytes = sizeof(RemotePrefetchCacheEntry) * RemotePrefetchCacheSlots;
  RemotePrefetchCacheEntry* entries =
      (RemotePrefetchCacheEntry*)os::malloc(bytes, mtGC);
  if (entries == nullptr) {
    return false;
  }
  memset(entries, 0, bytes);
  g1_remote_prefetch_cache = entries;
  return true;
}

static bool remote_prefetch_cache_evict_one_locked() {
  if (g1_remote_prefetch_cache == nullptr) {
    return false;
  }
  for (uint i = 0; i < RemotePrefetchCacheSlots; i++) {
    uint idx = (g1_remote_prefetch_cache_evict_cursor + i) % RemotePrefetchCacheSlots;
    if (g1_remote_prefetch_cache[idx].handle != nullptr) {
      remote_prefetch_cache_free_entry_locked(idx);
      g1_remote_prefetch_cache_evictions++;
      g1_remote_prefetch_cache_evict_cursor = (idx + 1) % RemotePrefetchCacheSlots;
      return true;
    }
  }
  return false;
}

static uint remote_prefetch_cache_hash(RemoteHandle* h, size_t slot_id) {
  uintptr_t x = (uintptr_t)h ^ (slot_id * 11400714819323198485ull);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  return (uint)(x % RemotePrefetchCacheSlots);
}

static uint remote_fetch_effective_batch_objects(uint configured) {
  if (configured <= 1) {
    return configured;
  }

  int pressure_level = remote_prefetch_sample_pressure_level();
  uint bounded = MIN2(configured, G1RemoteFetchBatchHardCap);
  uint effective = bounded;
  uint64_t hits = 0;
  uint64_t stores = 0;
  uint64_t evictions = 0;
  uint64_t drops = 0;
  uint64_t useful_per_mille = 0;
  uint64_t evict_per_mille = 0;
  bool log_change = false;
  bool disabled_by_policy = false;
  bool pressure_limited = false;
  uint64_t pressure_suppressed = 0;

  remote_prefetch_cache_lock();
  hits = g1_remote_prefetch_cache_hits;
  stores = g1_remote_prefetch_cache_stores;
  evictions = g1_remote_prefetch_cache_evictions;
  drops = g1_remote_prefetch_cache_drops;
  useful_per_mille = stores == 0 ? 0 : (hits * 1000) / stores;
  evict_per_mille = stores == 0 ? 0 : (evictions * 1000) / stores;

  if (stores < 4096) {
    effective = MIN2(bounded, 64u);
  } else if (stores < 64 * 1024) {
    if (useful_per_mille < 50) {
      effective = MIN2(bounded, 8u);
    } else if (useful_per_mille < 150) {
      effective = MIN2(bounded, 16u);
    } else if (useful_per_mille < 300) {
      effective = MIN2(bounded, 32u);
    } else {
      effective = MIN2(bounded, 64u);
    }
  } else {
    if (useful_per_mille < 100 ||
        (useful_per_mille < 200 && evict_per_mille > 750)) {
      effective = 1;
    } else if (useful_per_mille < 200) {
      effective = MIN2(bounded, 4u);
    } else if (useful_per_mille < 350) {
      effective = MIN2(bounded, 8u);
    } else if (useful_per_mille < 500) {
      effective = MIN2(bounded, 16u);
    } else if (useful_per_mille < 700) {
      effective = MIN2(bounded, 64u);
    }
  }

  if (pressure_level >= RemotePrefetchPressureOverTier2) {
    if (effective > 1) {
      pressure_suppressed =
        Atomic::add(&g1_remote_prefetch_batch_suppressed, (uint64_t)1);
      pressure_limited = true;
    }
    effective = 1;
    if (g1_remote_prefetch_cache_bytes > 0) {
      remote_prefetch_cache_clear_locked();
    }
  }

  if (g1_remote_fetch_batch_effective_logged == 0 ||
      stores >= g1_remote_fetch_batch_policy_log_next_stores ||
      (pressure_limited &&
       (pressure_suppressed == 1 ||
        (pressure_suppressed & (pressure_suppressed - 1)) == 0))) {
    g1_remote_fetch_batch_effective_logged = effective;
    g1_remote_fetch_batch_policy_log_next_stores = stores + 64 * 1024;
    log_change = true;
  }
  if (!pressure_limited && effective == 1 && stores >= 64 * 1024 &&
      Atomic::cmpxchg(&g1_remote_fetch_batch_disabled, 0, 1) == 0) {
    remote_prefetch_cache_clear_locked();
    disabled_by_policy = true;
  }
  remote_prefetch_cache_unlock();

  if (log_change) {
    log_info(gc)("Remote batch fetch policy: configured=%u effective=%u "
                 "cache_hits=" UINT64_FORMAT " stores=" UINT64_FORMAT
                 " evictions=" UINT64_FORMAT " drops=" UINT64_FORMAT
                 " useful_per_mille=" UINT64_FORMAT
                 " evict_per_mille=" UINT64_FORMAT
                 " pressure=%s action=%s suppress(batch=" UINT64_FORMAT
                 " eager=" UINT64_FORMAT " cache=" UINT64_FORMAT ")",
                 configured, effective, hits, stores, evictions, drops,
                 useful_per_mille, evict_per_mille,
                 remote_prefetch_pressure_level_name(pressure_level),
                 remote_prefetch_pressure_action(pressure_level),
                 Atomic::load(&g1_remote_prefetch_batch_suppressed),
                 Atomic::load(&g1_remote_prefetch_eager_suppressed),
                 Atomic::load(&g1_remote_prefetch_cache_suppressed));
  }
  if (disabled_by_policy) {
    log_info(gc)("Remote batch fetch disabled by storm breaker: "
                 "cache_hits=" UINT64_FORMAT " stores=" UINT64_FORMAT
                 " evictions=" UINT64_FORMAT " drops=" UINT64_FORMAT
                 " useful_per_mille=" UINT64_FORMAT
                 " evict_per_mille=" UINT64_FORMAT,
                 hits, stores, evictions, drops,
                 useful_per_mille, evict_per_mille);
  }
  return effective;
}

static bool remote_prefetch_cache_store(RemoteHandle* h, size_t slot_id,
                                        Klass* klass, size_t word_size,
                                        const void* obj_bytes) {
  if (Atomic::load(&g1_remote_fetch_batch_disabled) != 0) {
    return false;
  }
  if (!remote_prefetch_budget_allows_cache_store()) {
    Atomic::inc(&g1_remote_prefetch_cache_suppressed);
    return false;
  }
  if (h == nullptr || klass == nullptr || obj_bytes == nullptr || word_size == 0) {
    return false;
  }
  if (word_size > SIZE_MAX / HeapWordSize) {
    return false;
  }
  size_t byte_size = word_size * HeapWordSize;
  if (byte_size > RemotePrefetchCacheMaxObjectBytes ||
      byte_size > RemotePrefetchCacheMaxBytes) {
    return false;
  }

  uintptr_t sa = h->load_state_and_addr_acquire();
  if ((sa & REMOTE_HANDLE_STATE_MASK) != REMOTE_HANDLE_REMOTE ||
      (size_t)(sa & REMOTE_HANDLE_ADDR_MASK) != slot_id ||
      word_size != h->eviction_word_size()) {
    return false;
  }

  uint8_t* bytes = (uint8_t*)os::malloc(byte_size, mtGC);
  if (bytes == nullptr) {
    return false;
  }
  memcpy(bytes, obj_bytes, byte_size);

  remote_prefetch_cache_lock();
  if (!remote_prefetch_cache_ensure_locked()) {
    remote_prefetch_cache_unlock();
    os::free(bytes);
    return false;
  }

  sa = h->load_state_and_addr_acquire();
  if ((sa & REMOTE_HANDLE_STATE_MASK) != REMOTE_HANDLE_REMOTE ||
      (size_t)(sa & REMOTE_HANDLE_ADDR_MASK) != slot_id ||
      word_size != h->eviction_word_size()) {
    remote_prefetch_cache_unlock();
    os::free(bytes);
    return false;
  }

  while (g1_remote_prefetch_cache_bytes + byte_size > RemotePrefetchCacheMaxBytes) {
    if (!remote_prefetch_cache_evict_one_locked()) {
      remote_prefetch_cache_unlock();
      os::free(bytes);
      return false;
    }
  }

  uint start = remote_prefetch_cache_hash(h, slot_id);
  uint target = UINT_MAX;
  uint oldest = UINT_MAX;
  uint64_t oldest_stamp = UINT64_MAX;
  for (uint probe = 0; probe < RemotePrefetchCacheProbeLimit; probe++) {
    uint idx = (start + probe) % RemotePrefetchCacheSlots;
    RemotePrefetchCacheEntry& e = g1_remote_prefetch_cache[idx];
    if (e.handle == h && e.slot_id == slot_id) {
      target = idx;
      break;
    }
    if (e.handle == nullptr) {
      target = idx;
      break;
    }
    if (e.stamp < oldest_stamp) {
      oldest_stamp = e.stamp;
      oldest = idx;
    }
  }
  if (target == UINT_MAX) {
    target = oldest;
  }
  if (target == UINT_MAX) {
    remote_prefetch_cache_unlock();
    os::free(bytes);
    return false;
  }

  if (g1_remote_prefetch_cache[target].handle != nullptr) {
    remote_prefetch_cache_free_entry_locked(target);
    g1_remote_prefetch_cache_evictions++;
  }

  uint64_t stamp = ++g1_remote_prefetch_cache_stores;
  RemotePrefetchCacheEntry& e = g1_remote_prefetch_cache[target];
  e.handle = h;
  e.slot_id = slot_id;
  e.klass = klass;
  e.word_size = word_size;
  e.bytes = bytes;
  e.byte_size = byte_size;
  e.stamp = stamp;
  g1_remote_prefetch_cache_bytes += byte_size;

  if (stamp == 1 || (stamp & (stamp - 1)) == 0) {
    log_info(gc)("Remote prefetch cache: hits=" UINT64_FORMAT
                 " stores=" UINT64_FORMAT " evictions=" UINT64_FORMAT
                 " drops=" UINT64_FORMAT " bytes=" SIZE_FORMAT,
                 g1_remote_prefetch_cache_hits, stamp,
                 g1_remote_prefetch_cache_evictions,
                 g1_remote_prefetch_cache_drops,
                 g1_remote_prefetch_cache_bytes);
  }

  remote_prefetch_cache_unlock();
  return true;
}

static bool remote_prefetch_cache_take(RemoteHandle* h, size_t slot_id,
                                       Klass** klass_out, size_t* word_size_out,
                                       uint8_t** bytes_out) {
  *klass_out = nullptr;
  *word_size_out = 0;
  *bytes_out = nullptr;
  if (h == nullptr || g1_remote_prefetch_cache == nullptr ||
      g1_remote_prefetch_cache_bytes == 0) {
    return false;
  }

  remote_prefetch_cache_lock();
  if (g1_remote_prefetch_cache == nullptr ||
      g1_remote_prefetch_cache_bytes == 0) {
    remote_prefetch_cache_unlock();
    return false;
  }

  uint start = remote_prefetch_cache_hash(h, slot_id);
  for (uint probe = 0; probe < RemotePrefetchCacheProbeLimit; probe++) {
    uint idx = (start + probe) % RemotePrefetchCacheSlots;
    RemotePrefetchCacheEntry& e = g1_remote_prefetch_cache[idx];
    if (e.handle == nullptr) {
      continue;
    }
    if (e.handle != h) {
      continue;
    }

    bool match = e.slot_id == slot_id && e.word_size == h->eviction_word_size();
    if (match) {
      *klass_out = e.klass;
      *word_size_out = e.word_size;
      *bytes_out = e.bytes;
      e.bytes = nullptr;
      e.handle = nullptr;
      e.slot_id = 0;
      e.klass = nullptr;
      e.word_size = 0;
      if (g1_remote_prefetch_cache_bytes >= e.byte_size) {
        g1_remote_prefetch_cache_bytes -= e.byte_size;
      } else {
        g1_remote_prefetch_cache_bytes = 0;
      }
      e.byte_size = 0;
      e.stamp = 0;
      g1_remote_prefetch_cache_hits++;
      uint64_t hits = g1_remote_prefetch_cache_hits;
      if (hits == 1 || (hits & (hits - 1)) == 0) {
        log_info(gc)("Remote prefetch cache: hits=" UINT64_FORMAT
                     " stores=" UINT64_FORMAT " evictions=" UINT64_FORMAT
                     " drops=" UINT64_FORMAT " bytes=" SIZE_FORMAT,
                     hits, g1_remote_prefetch_cache_stores,
                     g1_remote_prefetch_cache_evictions,
                     g1_remote_prefetch_cache_drops,
                     g1_remote_prefetch_cache_bytes);
      }
      remote_prefetch_cache_unlock();
      return true;
    }

    remote_prefetch_cache_free_entry_locked(idx);
    g1_remote_prefetch_cache_drops++;
    remote_prefetch_cache_unlock();
    return false;
  }

  remote_prefetch_cache_unlock();
  return false;
}

static void remote_prefetch_cache_drop(RemoteHandle* h, size_t slot_id) {
  if (h == nullptr || g1_remote_prefetch_cache == nullptr ||
      g1_remote_prefetch_cache_bytes == 0) {
    return;
  }

  remote_prefetch_cache_lock();
  if (g1_remote_prefetch_cache != nullptr) {
    uint start = remote_prefetch_cache_hash(h, slot_id);
    for (uint probe = 0; probe < RemotePrefetchCacheProbeLimit; probe++) {
      uint idx = (start + probe) % RemotePrefetchCacheSlots;
      if (g1_remote_prefetch_cache[idx].handle == h &&
          g1_remote_prefetch_cache[idx].slot_id == slot_id) {
        remote_prefetch_cache_free_entry_locked(idx);
        g1_remote_prefetch_cache_drops++;
        break;
      }
    }
  }
  remote_prefetch_cache_unlock();
}

class BatchFetchInstallClosure : public G1RemoteBackend::FetchBatchClosure {
  G1CollectedHeap* _g1h;
  G1RemoteMemoryManager* _rmm;
  RemoteHandle* _primary;
  oopDesc* _primary_result;
  size_t _primary_slot;
  size_t _returned;
  size_t _installed;
  size_t _prefetched;
  size_t _raced;
  size_t _failed;
  size_t _prefetch_words;
  bool _primary_alloc_failed;
  RemoteHandle* _publish_handles[G1RemoteFetchBatchHardCap];
  HeapWord* _publish_dests[G1RemoteFetchBatchHardCap];
  uintptr_t _publish_ids[G1RemoteFetchBatchHardCap];
  size_t _publish_slots[G1RemoteFetchBatchHardCap];
  uint _publish_count;
  bool _eager_prefetch;

  bool try_eager_install_prefetch(RemoteHandle* h, size_t slot_id, Klass* klass,
                                  size_t word_size, const void* obj_bytes) {
    if (!_eager_prefetch || _publish_count >= G1RemoteFetchBatchHardCap) {
      return false;
    }
    if (!remote_prefetch_budget_allows_eager_install()) {
      Atomic::inc(&g1_remote_prefetch_eager_suppressed);
      return false;
    }
    if (!h->try_remote_to_fetching(slot_id)) {
      return false;
    }

    HeapWord* dest = _rmm->allocate_in_fcr(word_size);
    if (dest == nullptr) {
      h->cas_fetching_to_remote();
      return false;
    }

    memset(dest, 0, word_size * HeapWordSize);
    memcpy(dest, obj_bytes, word_size * HeapWordSize);
    finish_fetched_object(_g1h, _rmm, h, dest, klass, word_size);

    _publish_handles[_publish_count] = h;
    _publish_dests[_publish_count] = dest;
    _publish_ids[_publish_count] = (uintptr_t)h;
    _publish_slots[_publish_count] = slot_id;
    _publish_count++;

    _installed++;
    _prefetched++;
    _prefetch_words += word_size;
    return true;
  }

public:
  BatchFetchInstallClosure(G1CollectedHeap* g1h, G1RemoteMemoryManager* rmm,
                           RemoteHandle* primary, size_t primary_slot,
                           bool eager_prefetch)
    : _g1h(g1h), _rmm(rmm), _primary(primary), _primary_result(nullptr),
      _primary_slot(primary_slot), _returned(0), _installed(0), _prefetched(0),
      _raced(0), _failed(0), _prefetch_words(0), _primary_alloc_failed(false),
      _publish_count(0), _eager_prefetch(eager_prefetch) {}

  void do_object(uintptr_t handle_id, size_t slot_id, Klass* klass,
                 size_t word_size, const void* obj_bytes) override {
    _returned++;
    RemoteHandle* h = (RemoteHandle*)handle_id;
    bool is_primary = (h == _primary);

    if (h == nullptr || klass == nullptr || word_size == 0 || obj_bytes == nullptr) {
      _failed++;
      return;
    }

    if (word_size != h->eviction_word_size()) {
      _failed++;
      log_warning(gc)("Batch fetch size MISMATCH: handle=" PTR_FORMAT
                      " slot=" SIZE_FORMAT " expected=" SIZE_FORMAT
                      "w got=" SIZE_FORMAT "w",
                      p2i(h), slot_id, h->eviction_word_size(), word_size);
      return;
    }

    if (is_primary) {
      if (_publish_count >= G1RemoteFetchBatchHardCap) {
        _failed++;
        return;
      }
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      size_t current_slot = (size_t)(sa & REMOTE_HANDLE_ADDR_MASK);
      if (state != REMOTE_HANDLE_FETCHING || current_slot != slot_id ||
          slot_id != _primary_slot) {
        _failed++;
        return;
      }
    } else {
      if (try_eager_install_prefetch(h, slot_id, klass, word_size, obj_bytes)) {
        return;
      }
      if (remote_prefetch_cache_store(h, slot_id, klass, word_size, obj_bytes)) {
        _prefetched++;
        _prefetch_words += word_size;
      } else {
        _raced++;
      }
      return;
    }

    HeapWord* dest = _rmm->allocate_in_fcr(word_size);
    if (dest == nullptr) {
      if (is_primary) {
        h->cas_fetching_to_remote();
        _primary_alloc_failed = true;
        _failed++;
        return;
      } else {
        h->cas_fetching_to_remote();
        _failed++;
        return;
      }
    }

    memset(dest, 0, word_size * HeapWordSize);
    memcpy(dest, obj_bytes, word_size * HeapWordSize);
    oopDesc* installed = finish_fetched_object(_g1h, _rmm, h, dest, klass, word_size);

    _publish_handles[_publish_count] = h;
    _publish_dests[_publish_count] = dest;
    _publish_ids[_publish_count] = (uintptr_t)h;
    _publish_slots[_publish_count] = slot_id;
    _publish_count++;

    _installed++;
    if (is_primary) {
      _primary_result = installed;
    }
  }

  void publish(G1RemoteBackend* backend) {
    if (_publish_count == 0) return;
    for (uint i = 0; i < _publish_count; i++) {
      remote_prefetch_cache_drop(_publish_handles[i], _publish_slots[i]);
    }
    backend->localize_batch(_publish_ids, _publish_count);
    _rmm->publish_local_handles(_publish_handles, _publish_dests, _publish_count);
  }

  oopDesc* primary_result() const { return _primary_result; }
  bool primary_alloc_failed() const { return _primary_alloc_failed; }
  size_t returned() const { return _returned; }
  size_t installed() const { return _installed; }
  size_t prefetched() const { return _prefetched; }
  size_t raced() const { return _raced; }
  size_t failed() const { return _failed; }
  size_t prefetch_words() const { return _prefetch_words; }
};

struct RemoteExactFetchRequest {
  RemoteHandle* handle;
  size_t slot_id;
  size_t word_size;
  volatile int done;
  bool skipped;
  bool retry;
  bool installed;
  oopDesc* result;
};

static const uint G1RemoteExactFetchCombineSpin = 4096;
static volatile int g1_remote_exact_fetch_combine_lock = 0;
static volatile int g1_remote_exact_fetch_combine_active = 0;
static RemoteExactFetchRequest*
    g1_remote_exact_fetch_combine_queue[G1RemoteFetchBatchHardCap];
static uint g1_remote_exact_fetch_combine_count = 0;
static volatile uint64_t g1_remote_exact_fetch_batches = 0;
static volatile uint64_t g1_remote_exact_fetch_requests = 0;
static volatile uint64_t g1_remote_exact_fetch_solo = 0;
static volatile uint64_t g1_remote_exact_fetch_installed = 0;
static volatile uint64_t g1_remote_exact_fetch_retries = 0;

static uint remote_exact_fetch_limit() {
  if (G1RemoteFetchBatchObjects <= 1) {
    return 0;
  }
  return MIN2((uint)G1RemoteFetchBatchObjects, G1RemoteFetchBatchHardCap);
}

static size_t remote_fetch_max_response_bytes() {
  return G1RemoteFetchBatchBytes == 0 ?
      (size_t)RDMAMsgBufSize : MIN2((size_t)G1RemoteFetchBatchBytes,
                                    (size_t)RDMAMsgBufSize);
}

static bool remote_fetch_hint_prefers_around(uint32_t access_hint) {
  if (!G1RemoteUseCompilerFetchHints) {
    return false;
  }
  return access_hint == G1RemoteAccessHintField ||
         access_hint == G1RemoteAccessHintArray ||
         access_hint == G1RemoteAccessHintInterpreter;
}

static bool remote_fetch_hint_eager_installs_prefetch(uint32_t access_hint) {
  if (!G1RemoteEagerInstallPrefetch ||
      !remote_fetch_hint_prefers_around(access_hint)) {
    return false;
  }
  if (!remote_prefetch_budget_allows_eager_install()) {
    Atomic::inc(&g1_remote_prefetch_eager_suppressed);
    return false;
  }
  return true;
}

static void remote_apply_compiler_fetch_hint(uint32_t access_hint,
                                             uint* max_objects,
                                             uint* slot_window,
                                             size_t* max_response_bytes) {
  if (!G1RemoteUseCompilerFetchHints || max_objects == nullptr ||
      slot_window == nullptr || max_response_bytes == nullptr ||
      *max_objects <= 1) {
    return;
  }

  switch (access_hint) {
    case G1RemoteAccessHintArray:
      *max_objects = MAX2(*max_objects, MIN2((uint)64, G1RemoteFetchBatchHardCap));
      *slot_window = MAX2(*slot_window, (uint)1024);
      break;
    case G1RemoteAccessHintField:
      *max_objects = MAX2(*max_objects, MIN2((uint)32, G1RemoteFetchBatchHardCap));
      break;
    case G1RemoteAccessHintInterpreter:
      *max_objects = MAX2(*max_objects, MIN2((uint)16, G1RemoteFetchBatchHardCap));
      break;
    case G1RemoteAccessHintUnsafe:
    case G1RemoteAccessHintAtomic:
    case G1RemoteAccessHintUnknown:
    default:
      return;
  }

  *max_objects = MIN2(*max_objects, G1RemoteFetchBatchHardCap);
  if (*max_response_bytes == 0) {
    *max_response_bytes = remote_fetch_max_response_bytes();
  }
}

static void remote_exact_fetch_combine_lock() {
  while (Atomic::cmpxchg(&g1_remote_exact_fetch_combine_lock, 0, 1) != 0) {
    SpinPause();
  }
}

static void remote_exact_fetch_combine_unlock() {
  Atomic::release_store(&g1_remote_exact_fetch_combine_lock, 0);
}

static void remote_exact_fetch_complete(RemoteExactFetchRequest* req) {
  Atomic::release_store(&req->done, 1);
}

static bool remote_exact_fetch_enqueue(RemoteExactFetchRequest* req,
                                       bool* became_leader) {
  *became_leader = false;
  uint limit = remote_exact_fetch_limit();
  if (limit < 2) {
    return false;
  }

  remote_exact_fetch_combine_lock();
  if (g1_remote_exact_fetch_combine_count >= limit) {
    remote_exact_fetch_combine_unlock();
    return false;
  }

  g1_remote_exact_fetch_combine_queue[g1_remote_exact_fetch_combine_count++] = req;
  if (Atomic::load(&g1_remote_exact_fetch_combine_active) == 0) {
    Atomic::release_store(&g1_remote_exact_fetch_combine_active, 1);
    *became_leader = true;
  }
  remote_exact_fetch_combine_unlock();
  return true;
}

class ExactFetchInstallClosure : public G1RemoteBackend::FetchBatchClosure {
  G1CollectedHeap* _g1h;
  G1RemoteMemoryManager* _rmm;
  RemoteExactFetchRequest** _requests;
  uint _count;
  size_t _installed;
  size_t _failed;
  RemoteHandle* _publish_handles[G1RemoteFetchBatchHardCap];
  HeapWord* _publish_dests[G1RemoteFetchBatchHardCap];
  uintptr_t _publish_ids[G1RemoteFetchBatchHardCap];
  size_t _publish_slots[G1RemoteFetchBatchHardCap];
  uint _publish_count;

  RemoteExactFetchRequest* find_request(RemoteHandle* h, size_t slot_id) {
    for (uint i = 0; i < _count; i++) {
      RemoteExactFetchRequest* req = _requests[i];
      if (req->handle == h && req->slot_id == slot_id) {
        return req;
      }
    }
    return nullptr;
  }

public:
  ExactFetchInstallClosure(G1CollectedHeap* g1h, G1RemoteMemoryManager* rmm,
                           RemoteExactFetchRequest** requests, uint count)
    : _g1h(g1h), _rmm(rmm), _requests(requests), _count(count),
      _installed(0), _failed(0), _publish_count(0) {}

  void do_object(uintptr_t handle_id, size_t slot_id, Klass* klass,
                 size_t word_size, const void* obj_bytes) override {
    RemoteHandle* h = (RemoteHandle*)handle_id;
    RemoteExactFetchRequest* req = find_request(h, slot_id);
    if (req == nullptr || req->installed || h == nullptr ||
        klass == nullptr || obj_bytes == nullptr || word_size == 0) {
      _failed++;
      return;
    }

    if (word_size != req->word_size || word_size != h->eviction_word_size()) {
      req->retry = true;
      _failed++;
      log_warning(gc)("Exact fetch size MISMATCH: handle=" PTR_FORMAT
                      " slot=" SIZE_FORMAT " expected=" SIZE_FORMAT
                      "w got=" SIZE_FORMAT "w",
                      p2i(h), slot_id, req->word_size, word_size);
      return;
    }

    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    size_t current_slot = (size_t)(sa & REMOTE_HANDLE_ADDR_MASK);
    if (state != REMOTE_HANDLE_FETCHING || current_slot != slot_id) {
      req->retry = true;
      _failed++;
      return;
    }

    if (_publish_count >= G1RemoteFetchBatchHardCap) {
      req->retry = true;
      _failed++;
      return;
    }

    HeapWord* dest = _rmm->allocate_in_fcr(word_size);
    if (dest == nullptr) {
      h->cas_fetching_to_remote();
      req->retry = true;
      _failed++;
      return;
    }

    memset(dest, 0, word_size * HeapWordSize);
    memcpy(dest, obj_bytes, word_size * HeapWordSize);
    oopDesc* installed = finish_fetched_object(_g1h, _rmm, h, dest, klass, word_size);

    _publish_handles[_publish_count] = h;
    _publish_dests[_publish_count] = dest;
    _publish_ids[_publish_count] = (uintptr_t)h;
    _publish_slots[_publish_count] = slot_id;
    _publish_count++;

    req->result = installed;
    req->installed = true;
    _installed++;
  }

  void publish(G1RemoteBackend* backend) {
    if (_publish_count == 0) return;
    for (uint i = 0; i < _publish_count; i++) {
      remote_prefetch_cache_drop(_publish_handles[i], _publish_slots[i]);
    }
    backend->localize_batch(_publish_ids, _publish_count);
    _rmm->publish_local_handles(_publish_handles, _publish_dests, _publish_count);
  }

  size_t installed() const { return _installed; }
  size_t failed() const { return _failed; }
};

static void remote_exact_fetch_log_progress() {
  uint64_t batches = Atomic::load(&g1_remote_exact_fetch_batches);
  if (batches == 1 || (batches & (batches - 1)) == 0) {
    log_info(gc)("Remote exact fetch combiner: batches=" UINT64_FORMAT
                 " requests=" UINT64_FORMAT " installed=" UINT64_FORMAT
                 " retries=" UINT64_FORMAT " solo=" UINT64_FORMAT,
                 batches,
                 Atomic::load(&g1_remote_exact_fetch_requests),
                 Atomic::load(&g1_remote_exact_fetch_installed),
                 Atomic::load(&g1_remote_exact_fetch_retries),
                 Atomic::load(&g1_remote_exact_fetch_solo));
  }
}

static void remote_exact_fetch_run_batch(RemoteExactFetchRequest** requests,
                                         uint count,
                                         G1CollectedHeap* g1h,
                                         G1RemoteMemoryManager* rmm,
                                         G1RemoteBackend* backend) {
  uintptr_t handle_ids[G1RemoteFetchBatchHardCap];
  size_t slot_ids[G1RemoteFetchBatchHardCap];
  for (uint i = 0; i < count; i++) {
    handle_ids[i] = (uintptr_t)requests[i]->handle;
    slot_ids[i] = requests[i]->slot_id;
  }

  ExactFetchInstallClosure installer(g1h, rmm, requests, count);
  jlong fetch_start = os::elapsed_counter();
  size_t returned = backend->fetch_batch_exact(handle_ids, slot_ids, count,
                                               remote_fetch_max_response_bytes(),
                                               &installer);
  jlong fetch_elapsed = os::elapsed_counter() - fetch_start;

  installer.publish(backend);
  rmm->record_fetch_batch_result(count, returned, installer.installed(),
                                 0, 0, installer.failed(), 0, fetch_elapsed);

  size_t installed = 0;
  size_t retries = 0;
  for (uint i = 0; i < count; i++) {
    RemoteExactFetchRequest* req = requests[i];
    if (req->installed && req->result != nullptr) {
      installed++;
      rmm->record_fetch_result(req->word_size, fetch_elapsed, true);
    } else {
      req->retry = true;
      req->handle->cas_fetching_to_remote();
      retries++;
    }
    remote_exact_fetch_complete(req);
  }

  Atomic::add(&g1_remote_exact_fetch_batches, (uint64_t)1);
  Atomic::add(&g1_remote_exact_fetch_requests, (uint64_t)count);
  Atomic::add(&g1_remote_exact_fetch_installed, (uint64_t)installed);
  Atomic::add(&g1_remote_exact_fetch_retries, (uint64_t)retries);
  remote_exact_fetch_log_progress();
}

static void remote_exact_fetch_drain_leader(G1CollectedHeap* g1h,
                                            G1RemoteMemoryManager* rmm,
                                            G1RemoteBackend* backend) {
  while (true) {
    for (uint spin = 0; spin < G1RemoteExactFetchCombineSpin; spin++) {
      SpinPause();
    }

    RemoteExactFetchRequest* requests[G1RemoteFetchBatchHardCap];
    uint count = 0;
    uint limit = remote_exact_fetch_limit();

    remote_exact_fetch_combine_lock();
    if (g1_remote_exact_fetch_combine_count == 0 || limit < 2) {
      Atomic::release_store(&g1_remote_exact_fetch_combine_active, 0);
      remote_exact_fetch_combine_unlock();
      return;
    }

    count = MIN2(g1_remote_exact_fetch_combine_count, limit);
    for (uint i = 0; i < count; i++) {
      requests[i] = g1_remote_exact_fetch_combine_queue[i];
    }
    for (uint i = count; i < g1_remote_exact_fetch_combine_count; i++) {
      g1_remote_exact_fetch_combine_queue[i - count] =
          g1_remote_exact_fetch_combine_queue[i];
    }
    g1_remote_exact_fetch_combine_count -= count;
    remote_exact_fetch_combine_unlock();

    if (count <= 1) {
      requests[0]->skipped = true;
      remote_exact_fetch_complete(requests[0]);
      Atomic::inc(&g1_remote_exact_fetch_solo);
      continue;
    }

    remote_exact_fetch_run_batch(requests, count, g1h, rmm, backend);
  }
}

static bool fetch_and_install_exact_combined(RemoteHandle* h,
                                             bool* out_retry,
                                             oopDesc** out_result) {
  *out_retry = false;
  *out_result = nullptr;

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
  G1RemoteBackend* backend = rmm->backend();
  if (remote_exact_fetch_limit() < 2 || !backend->supports_exact_batch_fetch()) {
    return false;
  }

  uintptr_t sa = h->load_state_and_addr_acquire();
  if ((sa & REMOTE_HANDLE_STATE_MASK) != REMOTE_HANDLE_FETCHING) {
    return false;
  }

  RemoteExactFetchRequest req;
  req.handle = h;
  req.slot_id = (size_t)(sa & REMOTE_HANDLE_ADDR_MASK);
  req.word_size = h->eviction_word_size();
  req.done = 0;
  req.skipped = false;
  req.retry = false;
  req.installed = false;
  req.result = nullptr;

  bool became_leader = false;
  if (!remote_exact_fetch_enqueue(&req, &became_leader)) {
    return false;
  }

  if (became_leader) {
    remote_exact_fetch_drain_leader(g1h, rmm, backend);
  }

  uint64_t spins = 0;
  while (Atomic::load_acquire(&req.done) == 0) {
    if (++spins == (1ULL << 24)) {
      log_warning(gc)("Exact fetch combiner wait exceeded %llu spins for handle "
                      PTR_FORMAT " slot=" SIZE_FORMAT,
                      (unsigned long long)spins, p2i(h), req.slot_id);
    }
    SpinPause();
  }

  if (req.skipped) {
    return false;
  }

  if (req.retry || req.result == nullptr) {
    // Keep exact batching out of the correctness decision.  If the batch
    // response omitted this request or allocation failed, try to reclaim the
    // FETCHING ownership and let the existing single-object path handle the
    // fetch/retry/dead-marking policy.
    if (h->try_remote_to_fetching(req.slot_id)) {
      return false;
    }
    rmm->record_fetch_retry();
    *out_retry = true;
    return true;
  }

  *out_result = req.result;
  return true;
}

static oopDesc* fetch_and_install_batch(RemoteHandle* h, int& fetch_attempts,
                                        bool* out_retry, uint max_objects,
                                        uint slot_window,
                                        size_t max_response_bytes,
                                        bool eager_prefetch) {
  *out_retry = false;
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
  G1RemoteBackend* backend = rmm->backend();

  uintptr_t sa = h->load_state_and_addr_acquire();
  size_t slot_id = (size_t)(sa & REMOTE_HANDLE_ADDR_MASK);
  size_t word_size = h->eviction_word_size();
  BatchFetchInstallClosure installer(g1h, rmm, h, slot_id, eager_prefetch);
  jlong fetch_start = os::elapsed_counter();
  size_t returned = backend->fetch_batch_around((uintptr_t)h, slot_id, max_objects,
                                                slot_window, max_response_bytes,
                                                &installer);
  jlong fetch_elapsed = os::elapsed_counter() - fetch_start;

  installer.publish(backend);
  bool primary_ok = installer.primary_result() != nullptr;
  rmm->record_fetch_batch_result(max_objects, returned, installer.installed(),
                                 installer.prefetched(), installer.raced(),
                                 installer.failed(), installer.prefetch_words(),
                                 fetch_elapsed);
  rmm->record_fetch_result(word_size, fetch_elapsed, primary_ok);

  if (primary_ok) {
    return installer.primary_result();
  }

  if (installer.primary_alloc_failed()) {
    h->cas_fetching_to_remote();
    rmm->record_fetch_retry();
    *out_retry = true;
    return nullptr;
  }

  Atomic::release_store(&g1_remote_fetch_batch_disabled, 1);
  log_warning(gc)("Batch fetch did not install primary object for handle " PTR_FORMAT
                  " slot=" SIZE_FORMAT " (returned=" SIZE_FORMAT
                  "); disabling batch fetch for this JVM",
                  p2i(h), slot_id, returned);

  fetch_attempts++;
  rmm->record_fetch_retry();
  if (fetch_attempts >= 3) {
    rmm->mark_handle_dead(h);
    log_warning(gc)("Batch fetch failed %d times for handle " PTR_FORMAT
                    " — marking DEAD", fetch_attempts, p2i(h));
    return nullptr;
  }

  h->cas_fetching_to_remote();
  *out_retry = true;
  return nullptr;
}

// Fetch a REMOTE object, install it locally, and publish the Handle as LOCAL.
// Caller must have already CAS'd the Handle to FETCHING.
// Returns the local oop on success, nullptr on failure (Handle set to DEAD
// after max retries, or set back to REMOTE for caller to retry).
// *out_retry is set to true if the caller should re-enter the state machine.
static oopDesc* fetch_and_install(RemoteHandle* h, int& fetch_attempts,
                                  bool* out_retry, uint32_t access_hint) {
  *out_retry = false;
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
  size_t word_size = h->eviction_word_size();
  uintptr_t fetch_sa = h->load_state_and_addr_acquire();
  size_t slot_id = (size_t)(fetch_sa & REMOTE_HANDLE_ADDR_MASK);

  Klass* cached_klass = nullptr;
  size_t cached_word_size = 0;
  uint8_t* cached_bytes = nullptr;
  if (remote_prefetch_cache_take(h, slot_id, &cached_klass,
                                 &cached_word_size, &cached_bytes)) {
    jlong fetch_start = os::elapsed_counter();
    HeapWord* dest = rmm->allocate_in_fcr(cached_word_size);
    if (dest == nullptr) {
      os::free(cached_bytes);
      h->cas_fetching_to_remote();
      rmm->record_fetch_retry();
      *out_retry = true;
      return nullptr;
    }

    memcpy(dest, cached_bytes, cached_word_size * HeapWordSize);
    os::free(cached_bytes);
    oopDesc* result = finish_fetched_object(g1h, rmm, h, dest,
                                            cached_klass, cached_word_size);
    uintptr_t handle_id = (uintptr_t)h;
    remote_prefetch_cache_drop(h, slot_id);
    rmm->backend()->localize_batch(&handle_id, 1);
    rmm->publish_local_handle(h, dest);

    jlong fetch_elapsed = os::elapsed_counter() - fetch_start;
    rmm->record_fetch_result(cached_word_size, fetch_elapsed, true);
    return result;
  }

  bool prefer_around = remote_fetch_hint_prefers_around(access_hint);

  oopDesc* combined_result = nullptr;
  bool combined_retry = false;
  if (!prefer_around &&
      fetch_and_install_exact_combined(h, &combined_retry, &combined_result)) {
    if (combined_retry) {
      *out_retry = true;
      return nullptr;
    }
    return combined_result;
  }

  if (G1RemoteFetchBatchObjects > 1 &&
      Atomic::load(&g1_remote_fetch_batch_disabled) == 0 &&
      rmm->backend()->supports_batch_fetch()) {
    uint max_objects = MIN2((uint)G1RemoteFetchBatchObjects,
                            G1RemoteFetchBatchHardCap);
    uint slot_window = G1RemoteFetchBatchSlotWindow;
    size_t max_response_bytes = remote_fetch_max_response_bytes();
    remote_apply_compiler_fetch_hint(access_hint, &max_objects,
                                     &slot_window, &max_response_bytes);
    max_objects = remote_fetch_effective_batch_objects(max_objects);
    if (max_objects > 1) {
      bool eager_prefetch = remote_fetch_hint_eager_installs_prefetch(access_hint);
      return fetch_and_install_batch(h, fetch_attempts, out_retry, max_objects,
                                     slot_window, max_response_bytes,
                                     eager_prefetch);
    }
  }

  if (prefer_around &&
      fetch_and_install_exact_combined(h, &combined_retry, &combined_result)) {
    if (combined_retry) {
      *out_retry = true;
      return nullptr;
    }
    return combined_result;
  }

  HeapWord* dest = rmm->allocate_in_fcr(word_size);
  if (dest == nullptr) {
    h->cas_fetching_to_remote();
    rmm->record_fetch_retry();
    *out_retry = true;
    return nullptr;
  }

  // Zero-fill before fetch so any partial/wrong copy is detectable
  memset(dest, 0, word_size * HeapWordSize);

  jlong fetch_start = os::elapsed_counter();
  Klass* fetched_klass = rmm->fetch_remote_object(h, dest);
  jlong fetch_elapsed = os::elapsed_counter() - fetch_start;
  rmm->record_fetch_result(word_size, fetch_elapsed, fetched_klass != nullptr);

  if (fetched_klass == nullptr) {
    fetch_attempts++;
    rmm->record_fetch_retry();
    if (fetch_attempts >= 3) {
      rmm->mark_handle_dead(h);
      log_warning(gc)("Fetch failed %d times for handle " PTR_FORMAT " — marking DEAD",
                      fetch_attempts, p2i(h));
      return nullptr;
    }
    h->cas_fetching_to_remote();
    log_warning(gc)("Fetch failed for handle " PTR_FORMAT " — retry %d/3",
                    p2i(h), fetch_attempts);
    *out_retry = true;
    return nullptr;
  }

  oopDesc* result = finish_fetched_object(g1h, rmm, h, dest, fetched_klass, word_size);

  uintptr_t handle_id = (uintptr_t)h;
  remote_prefetch_cache_drop(h, slot_id);
  rmm->backend()->localize_batch(&handle_id, 1);

  rmm->publish_local_handle(h, dest);
  return result;
}

// Shared fast-path checks for both slow-path variants.
// Returns non-null oop if resolved without needing the state machine.
// Returns nullptr if the caller must enter the state machine.
// Sets *handle_out to the RemoteHandle* if state machine is needed.
static oopDesc* resolve_fast_checks(oopDesc* tagged, RemoteHandle** handle_out) {
  uintptr_t v = (uintptr_t)tagged;
  if ((v >> 63) == 0) {
    if (v == 0) return tagged;

    // A clean oop can still be stale if it was live only in compiled state
    // while its region was evicted.  Heap field scans tag clean refs before
    // eviction, but compiled registers are not a heap slot and may retain the
    // pre-eviction address.  If that address now belongs to a guarded/free
    // region, recover the RemoteHandle by its old eviction address and let the
    // normal REMOTE/FETCHING/LOCAL state machine produce a current oop.
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    if (g1h != nullptr && g1h->is_in_reserved((void*)v)) {
      HeapRegion* hr = g1h->heap_region_containing_or_null((void*)v);
      bool stale_clean = hr == nullptr || hr->is_free() || hr->is_evict_guarded() ||
                         !g1h->is_in((void*)v);
      const char* stale_kind =
          hr == nullptr ? "NO-HR" : (hr->is_evict_guarded() ? "GUARDED" :
          (hr->is_free() ? "FREE" : "STALE"));
      if (!stale_clean) {
        oop obj = cast_to_oop((HeapWord*)v);
        Klass* k = obj->klass_or_null();
        if (k == nullptr) {
          stale_clean = true;
          stale_kind = "NULL-KLASS";
        } else if (G1CollectedHeap::is_obj_filler(obj)) {
          stale_clean = true;
          stale_kind = "FILLER";
        }
      }
      if (stale_clean) {
        G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
        RemoteHandle* h = rmm == nullptr ? nullptr : rmm->handle_for_addr_any_state(v);
        if (h != nullptr) {
          uintptr_t sa = h->load_state_and_addr_acquire();
          uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
          if (rmm != nullptr) {
            rmm->record_resolve_fast_state(state);
          }
          if (state != REMOTE_HANDLE_DEAD) {
            *handle_out = h;
            log_debug(gc)("Resolved clean stale oop " PTR_FORMAT
                          " from %s region %u via handle " PTR_FORMAT
                          " state=0x%lx",
                          p2i((void*)v),
                          stale_kind,
                          hr == nullptr ? 9999 : hr->hrm_index(),
                          p2i(h), (unsigned long)state);
            return nullptr;
          }
        }
        log_warning(gc)("Clean oop " PTR_FORMAT
                        " points into %s region %u but has no live remote handle",
                        p2i((void*)v),
                        stale_kind,
                        hr == nullptr ? 9999 : hr->hrm_index());
        return nullptr;
      }
    }
    return tagged;
  }
  if (!(v & G1_OOP_INDIRECT_BIT)) {
    oopDesc* resolved = (oopDesc*)(v & G1_OOP_ADDR_MASK);
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    G1RemoteMemoryManager* rmm = g1h == nullptr ? nullptr : g1h->remote_memory_manager();
    sample_touch_hotness(resolved, rmm);
    return resolved;
  }

  RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
  uintptr_t sa = h->load_state_and_addr_acquire();
  uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
  if (state == REMOTE_HANDLE_LOCAL) {
    RemoteHandle* redirect = nullptr;
    oopDesc* resolved = resolve_local_handle_addr(h, sa & REMOTE_HANDLE_ADDR_MASK,
                                                  &redirect, "resolve_fast_checks");
    if (redirect != nullptr) {
      *handle_out = redirect;
      return nullptr;
    }
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    G1RemoteMemoryManager* rmm = g1h == nullptr ? nullptr : g1h->remote_memory_manager();
    sample_touch_hotness(resolved, rmm);
    return resolved;
  }
  if (state == REMOTE_HANDLE_DEAD) {
    log_warning(gc)("resolve_fast_checks: DEAD handle " PTR_FORMAT
                    " reached by mutator (slot=%lu) — returning nullptr",
                    p2i(h), (unsigned long)(sa & REMOTE_HANDLE_ADDR_MASK));
    return nullptr;
  }

  *handle_out = h;
  return nullptr;
}

// ============================================================
// Runtime slow path for interpreter/shared callers.
//
// Remote fetch allocates into an FCR and publishes the Handle as LOCAL after
// the RDMA copy and field patching complete.  This publication must not race
// with STW remote eviction: otherwise eviction can inspect the Handle while it
// is FETCHING/REMOTE, free the destination region, and then the fetcher can
// publish a LOCAL address into a guarded region.  Use the no-safepoint state
// machine here as well, so a safepoint waits for the short fetch window instead
// of evicting concurrently with it.
// ============================================================
oopDesc* G1BarrierSetRuntime::resolve_tagged_oop_slow(oopDesc* tagged) {
  RemoteHandle* h = nullptr;
  oopDesc* fast = resolve_fast_checks(tagged, &h);
  if (h == nullptr) return fast;
  G1RemoteMemoryManager* rmm = G1CollectedHeap::heap()->remote_memory_manager();
  if (rmm != nullptr) {
    rmm->record_resolve_slow_entry();
  }
  return resolve_tagged_oop_no_safepoint(tagged);
}

// ============================================================
// Non-safepointing slow path: REMOTE fetch WITHOUT thread transitions.
// Called from C1/C2/assembler barrier stubs where the OopMap does not
// describe all live oop registers. By staying in _thread_in_Java
// and not allowing safepoints, the empty OopMap is harmless.
// Blocking I/O (RDMA/TCP) adds at most ~50us to safepoint initiation.
// ============================================================
oopDesc* G1BarrierSetRuntime::resolve_tagged_oop_no_safepoint(oopDesc* tagged) {
  return resolve_tagged_oop_no_safepoint_with_hint(tagged, G1RemoteAccessHintUnknown);
}

oopDesc* G1BarrierSetRuntime::resolve_tagged_oop_no_safepoint_with_hint(oopDesc* tagged,
                                                                         uint32_t access_hint) {
  RemoteHandle* h = nullptr;
  oopDesc* fast = resolve_fast_checks(tagged, &h);
  if (h == nullptr) return fast;
  G1RemoteMemoryManager* rmm = G1CollectedHeap::heap()->remote_memory_manager();
  if (rmm != nullptr) {
    rmm->record_resolve_no_safepoint_entry();
  }

  int fetch_attempts = 0;
  while (true) {
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;

    if (state == REMOTE_HANDLE_LOCAL) {
      RemoteHandle* redirect = nullptr;
      oopDesc* resolved = resolve_local_handle_addr(h, sa & REMOTE_HANDLE_ADDR_MASK,
                                                    &redirect,
                                                    "resolve_tagged_oop_no_safepoint");
      if (redirect != nullptr) {
        h = redirect;
        continue;
      }
      return resolved;
    }
    if (state == REMOTE_HANDLE_DEAD)  {
      // Same diagnostic as the safepointing variant. C2 callers have elided
      // implicit null checks on the result, so a nullptr return manifests as
      // SIGSEGV at offset N from null in JIT'd code. Log loudly.
      log_warning(gc)("resolve_tagged_oop_no_safepoint: DEAD handle " PTR_FORMAT
                      " reached by mutator (slot=%lu) — returning nullptr",
                      p2i(h), (unsigned long)(sa & REMOTE_HANDLE_ADDR_MASK));
      return nullptr;
    }

    if (state == REMOTE_HANDLE_REMOTE) {
      if (h->cas_remote_to_fetching()) {
        bool retry = false;
        oopDesc* result = fetch_and_install(h, fetch_attempts, &retry, access_hint);
        if (retry) continue;
        return result;
      }
      continue;
    }

    if (state == REMOTE_HANDLE_FETCHING) {
      const uint64_t SpinThreshold = 1ULL << 24;       // ~16M spins
      const uint64_t HardLimit     = 1ULL << 30;       // ~1B spins (~10s)
      uint64_t spins = 0;
      bool hard_wait = false;
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
          if (h->cas_fetching_to_remote()) {
            log_warning(gc)("FETCHING wait HARD LIMIT (%llu spins) for handle " PTR_FORMAT
                            " — reverted to REMOTE, will retry",
                            (unsigned long long)spins, p2i(h));
          }
          hard_wait = true;
          break;  // re-enter outer loop, retry with current state
        }
        SpinPause();
      }
      if (rmm != nullptr && spins > 0) {
        rmm->record_fetch_wait(true, hard_wait, spins);
      }
      continue;
    }

    SpinPause();
  }
}

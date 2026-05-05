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
#include "gc/g1/heapRegion.hpp"
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
  uint _publish_count;

public:
  BatchFetchInstallClosure(G1CollectedHeap* g1h, G1RemoteMemoryManager* rmm,
                           RemoteHandle* primary, size_t primary_slot)
    : _g1h(g1h), _rmm(rmm), _primary(primary), _primary_result(nullptr),
      _primary_slot(primary_slot), _returned(0), _installed(0), _prefetched(0),
      _raced(0), _failed(0), _prefetch_words(0), _primary_alloc_failed(false),
      _publish_count(0) {}

  void do_object(uintptr_t handle_id, size_t slot_id, Klass* klass,
                 size_t word_size, const void* obj_bytes) override {
    _returned++;
    RemoteHandle* h = (RemoteHandle*)handle_id;
    bool is_primary = (h == _primary);

    if (h == nullptr || klass == nullptr || word_size == 0 || obj_bytes == nullptr) {
      _failed++;
      return;
    }

    // Speculative prefetch installs objects into FCR without a Java access
    // that naturally roots them. Full GC can then trim the FCR region and leave
    // a LOCAL handle pointing outside top. Until prefetch uses a non-heap byte
    // cache or has precise lifetime tracking, only install the demanded object.
    if (!is_primary) {
      return;
    }

    if (_publish_count >= G1RemoteFetchBatchHardCap) {
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
      uintptr_t sa = h->load_state_and_addr_acquire();
      uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
      size_t current_slot = (size_t)(sa & REMOTE_HANDLE_ADDR_MASK);
      if (state != REMOTE_HANDLE_FETCHING || current_slot != slot_id ||
          slot_id != _primary_slot) {
        _failed++;
        return;
      }
    } else {
      if (!h->try_remote_to_fetching(slot_id)) {
        _raced++;
        return;
      }
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
    _publish_count++;

    _installed++;
    if (is_primary) {
      _primary_result = installed;
    } else {
      _prefetched++;
      _prefetch_words += word_size;
    }
  }

  void publish(G1RemoteBackend* backend) {
    if (_publish_count == 0) return;
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

static oopDesc* fetch_and_install_batch(RemoteHandle* h, int& fetch_attempts,
                                        bool* out_retry) {
  *out_retry = false;
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
  G1RemoteBackend* backend = rmm->backend();

  uintptr_t sa = h->load_state_and_addr_acquire();
  size_t slot_id = (size_t)(sa & REMOTE_HANDLE_ADDR_MASK);
  size_t word_size = h->eviction_word_size();
  uint max_objects = 1;
  size_t max_response_bytes = G1RemoteFetchBatchBytes == 0 ?
      (size_t)RDMAMsgBufSize : MIN2((size_t)G1RemoteFetchBatchBytes, (size_t)RDMAMsgBufSize);

  BatchFetchInstallClosure installer(g1h, rmm, h, slot_id);
  jlong fetch_start = os::elapsed_counter();
  size_t returned = backend->fetch_batch_around((uintptr_t)h, slot_id, max_objects,
                                                G1RemoteFetchBatchSlotWindow,
                                                max_response_bytes, &installer);
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
static oopDesc* fetch_and_install(RemoteHandle* h, int& fetch_attempts, bool* out_retry) {
  *out_retry = false;
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1RemoteMemoryManager* rmm = g1h->remote_memory_manager();
  size_t word_size = h->eviction_word_size();

  if (G1RemoteFetchBatchObjects > 1 &&
      Atomic::load(&g1_remote_fetch_batch_disabled) == 0 &&
      rmm->backend()->supports_batch_fetch()) {
    return fetch_and_install_batch(h, fetch_attempts, out_retry);
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
      if (hr == nullptr || hr->is_free() || hr->is_evict_guarded()) {
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
                          hr == nullptr ? "NO-HR" : (hr->is_evict_guarded() ? "GUARDED" : "FREE"),
                          hr == nullptr ? 9999 : hr->hrm_index(),
                          p2i(h), (unsigned long)state);
            return nullptr;
          }
        }
        log_warning(gc)("Clean oop " PTR_FORMAT
                        " points into %s region %u but has no live remote handle",
                        p2i((void*)v),
                        hr == nullptr ? "NO-HR" : (hr->is_evict_guarded() ? "GUARDED" : "FREE"),
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
        oopDesc* result = fetch_and_install(h, fetch_attempts, &retry);
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

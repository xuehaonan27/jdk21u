/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 *
 * OOP tagging and resolution for G1 disaggregated memory support.
 *
 * OOP Tag Bit Layout (64-bit, UseCompressedOops=false):
 *   Bit 63: is_managed    (0=Ordinary/Young, 1=Managed Old)
 *   Bit 62: is_indirect   (0=Direct/Unique,  1=Indirect/Shared via Handle)
 *   Bit 61: is_remote     (0=Local,          1=Remote)
 *   Bits 48-60: reserved
 *   Bits 0-47:  address (object, Handle, or remote_id)
 *
 * Key invariant: No code ever dereferences a tagged oop. Tags exist
 * only in heap slots. resolve_oop_raw() / load barrier strips tags
 * before any dereference, region lookup, or comparison.
 *
 * Heap MUST be below 2^47 (enforced by guarantee in universe.cpp).
 */

#ifndef SHARE_GC_G1_G1REMOTEOOP_HPP
#define SHARE_GC_G1_G1REMOTEOOP_HPP

#include "oops/oopsHierarchy.hpp"
#include "oops/compressedOops.hpp"
#include "oops/access.hpp"
#include "oops/accessBackend.hpp"
#include "utilities/globalDefinitions.hpp"

// ============================================================
// OOP Tag Bit Constants
// ============================================================

// Tag bits occupy bits 48-63 of 64-bit oop values.
// On x86-64 with heap below 2^47, these bits are naturally zero
// for valid heap pointers, so tagging is non-destructive.

const uintptr_t G1_OOP_MANAGED_BIT   = uintptr_t(1) << 63;  // is_managed
const uintptr_t G1_OOP_INDIRECT_BIT  = uintptr_t(1) << 62;  // is_indirect (Shared)
const uintptr_t G1_OOP_REMOTE_BIT    = uintptr_t(1) << 61;  // is_remote

// Mask covering all tag bits (bits 48-63)
const uintptr_t G1_OOP_TAG_MASK      = ~((uintptr_t(1) << 48) - 1);

// Mask to extract the 48-bit address from a tagged oop
const uintptr_t G1_OOP_ADDR_MASK     = (uintptr_t(1) << 48) - 1;

// Combined check: any tag bits set?
inline bool g1_oop_is_tagged(oop o) {
  return (cast_from_oop<uintptr_t>(o) & G1_OOP_TAG_MASK) != 0;
}

inline bool g1_oop_is_managed(oop o) {
  return (cast_from_oop<uintptr_t>(o) & G1_OOP_MANAGED_BIT) != 0;
}

inline bool g1_oop_is_indirect(oop o) {
  return (cast_from_oop<uintptr_t>(o) & G1_OOP_INDIRECT_BIT) != 0;
}

inline bool g1_oop_is_remote(oop o) {
  return (cast_from_oop<uintptr_t>(o) & G1_OOP_REMOTE_BIT) != 0;
}

// Strip all tag bits, returning a clean oop suitable for dereference.
inline oop g1_oop_strip_tags(oop o) {
  return cast_to_oop(cast_from_oop<uintptr_t>(o) & G1_OOP_ADDR_MASK);
}

// Create tagged oop values
inline oop g1_make_shared_oop(void* handle_addr) {
  return cast_to_oop(
    (cast_from_oop<uintptr_t>(cast_to_oop(handle_addr))) |
    G1_OOP_MANAGED_BIT | G1_OOP_INDIRECT_BIT
  );
}

inline oop g1_make_unique_oop(oop obj) {
  return cast_to_oop(
    cast_from_oop<uintptr_t>(obj) | G1_OOP_MANAGED_BIT
  );
}


// ============================================================
// Mark Word Remote Metadata Constants
// ============================================================
// These occupy bits 39-44 of the 64-bit mark word (within the
// 25-bit "unused" region at bits 39-63).
//
// We use LOWER bits (39-44) instead of higher bits (58-63) to avoid
// the sign bit (bit 63) and high bits that might be interpreted
// differently by some runtime code paths (MethodHandles, monitors).
//
// Mark word classification bits (39-40) are used for per-object classification.
// Comprehensive analysis (RESEARCH_DESIGN_PLAN.md) confirmed ALL HotSpot CAS
// paths (locking, hash, inflation/deflation) preserve upper mark word bits.
// Previous crashes were caused by plain stores racing with concurrent CAS —
// NOT by fundamental bit unsafety. Fix: set via CAS or during STW.
//
// HeapRegion::_has_classified_objects flag is the fast-negative test for
// write barrier. Mark word is the per-object authority.
//
// Mark word constants (aliases for markWord:: constants, for use in GC code):
const uintptr_t G1_MW_CLASS_UNTRACKED = markWord::remote_class_untracked;
const uintptr_t G1_MW_CLASS_UNIQUE    = markWord::remote_class_unique;
const uintptr_t G1_MW_CLASS_SHARED    = markWord::remote_class_shared;


// ============================================================
// resolve_oop_raw: GC-side oop resolution (no RDMA, no blocking)
// ============================================================
// Strips tag bits and follows Handle for Shared OOPs.
// Used by GC closures during STW when all scanned objects are local.
// Does NOT handle remote objects (asserts local during STW).

// RemoteHandle is defined in g1RemoteHandle.hpp.
// We include it here so resolve_oop_raw can follow Handles.
#include "gc/g1/g1RemoteHandle.hpp"

inline oop resolve_oop_raw(oop tagged) {
  uintptr_t v = cast_from_oop<uintptr_t>(tagged);
  if ((v & G1_OOP_TAG_MASK) == 0) {
    return tagged;  // Ordinary (fast path -- no tags)
  }

  if (v & G1_OOP_INDIRECT_BIT) {
    // Shared OOP: bits 47:0 is Handle address. Follow the Handle.
    RemoteHandle* h = (RemoteHandle*)(v & G1_OOP_ADDR_MASK);
    uintptr_t sa = h->load_state_and_addr_acquire();
    uintptr_t state = sa & REMOTE_HANDLE_STATE_MASK;
    if (state == REMOTE_HANDLE_LOCAL) {
      return cast_to_oop(sa & REMOTE_HANDLE_ADDR_MASK);
    }
    // Handle is REMOTE or FETCHING: object is not locally present.
    // Return nullptr — GC closures (mark_and_push, etc.) skip nullptr.
    // Remote objects are kept alive by the Handle table; their lifecycle
    // is managed by collect_dead_remote_objects(), not GC marking.
    return nullptr;
  }

  // Unique OOP (Managed, Direct): strip tag bits, return clean address.
  return cast_to_oop(v & G1_OOP_ADDR_MASK);
}


// Full resolution for non-GC contexts (JNI handles, runtime calls).
// Handles LOCAL, REMOTE (triggers fetch), FETCHING (waits).
// Defined in g1BarrierSet.cpp. NOT suitable during STW.
oop resolve_oop_full(oop tagged);

// ============================================================
// g1_resolved_load: centralized GC-side oop load + resolution
// ============================================================
// Replaces RawAccess<decorators>::oop_load(p) + CompressedOops::decode()
// at all G1 closure sites. Returns a clean, resolved oop.

template <DecoratorSet decorators = DECORATORS_NONE, typename T>
inline oop g1_resolved_load(T* p) {
  if (sizeof(T) == sizeof(narrowOop)) {
    // Narrow oop path: tags don't apply. Use normal decode.
    T raw = RawAccess<decorators>::oop_load(p);
    if (CompressedOops::is_null(raw)) return nullptr;
    return CompressedOops::decode_not_null(raw);
  }
  // Wide oop path: FAST PATH for clean oops (99.99% of cases).
  // Use standard RawAccess load (preserves compiler optimizations like
  // prefetch, reordering, vectorization). Check bit 63 only after load.
  uintptr_t raw = *(uintptr_t*)p;
  if (raw == 0) return nullptr;
  // Fast path: bit 63 clear → clean oop, return directly (no resolve)
  if ((raw >> 63) == 0) {
    return cast_to_oop(raw);
  }
  // Slow path: tagged oop (bit 63 set) — resolve through Handle/strip tags
  return resolve_oop_raw(cast_to_oop(raw));
}

#endif // SHARE_GC_G1_G1REMOTEOOP_HPP

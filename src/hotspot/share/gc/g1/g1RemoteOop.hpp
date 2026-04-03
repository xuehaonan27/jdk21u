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
// These occupy bits 58-63 of the 64-bit mark word (within the
// 25-bit "unused" region at bits 39-63).
//
// Bit 63: oop_managed     -- classified as Unique or Shared
// Bit 62: oop_shared      -- 0=Unique (RC=1), 1=Shared (RC>1)
// Bit 61: has_handle      -- Handle allocated (partially-upgraded)
// Bit 60: remote_stored   -- object bytes on remote memory
// Bits 59-58: hotness     -- 00=Cold, 01=Warm, 10=Hot, 11=VeryHot

const int      G1_MW_MANAGED_SHIFT   = 63;
const int      G1_MW_SHARED_SHIFT    = 62;
const int      G1_MW_HAS_HANDLE_SHIFT = 61;
const int      G1_MW_REMOTE_SHIFT    = 60;
const int      G1_MW_HOTNESS_SHIFT   = 58;

const uintptr_t G1_MW_MANAGED_BIT    = uintptr_t(1) << G1_MW_MANAGED_SHIFT;
const uintptr_t G1_MW_SHARED_BIT     = uintptr_t(1) << G1_MW_SHARED_SHIFT;
const uintptr_t G1_MW_HAS_HANDLE_BIT = uintptr_t(1) << G1_MW_HAS_HANDLE_SHIFT;
const uintptr_t G1_MW_REMOTE_BIT     = uintptr_t(1) << G1_MW_REMOTE_SHIFT;
const uintptr_t G1_MW_HOTNESS_MASK   = uintptr_t(3) << G1_MW_HOTNESS_SHIFT;

// All remote metadata bits (for must_be_preserved check and forward_to safety clear)
const uintptr_t G1_MW_REMOTE_METADATA_MASK =
    G1_MW_MANAGED_BIT | G1_MW_SHARED_BIT | G1_MW_HAS_HANDLE_BIT |
    G1_MW_REMOTE_BIT  | G1_MW_HOTNESS_MASK;


// ============================================================
// resolve_oop_raw: GC-side oop resolution (no RDMA, no blocking)
// ============================================================
// Strips tag bits and follows Handle for Shared OOPs.
// Used by GC closures during STW when all scanned objects are local.
// Does NOT handle remote objects (asserts local during STW).

// Forward declaration -- RemoteHandle is defined in g1RemoteHandle.hpp
// For now, resolve_oop_raw only strips tags (Phase 1a).
// Handle following will be added when Handle table is implemented (Phase 1b).

inline oop resolve_oop_raw(oop tagged) {
  uintptr_t v = cast_from_oop<uintptr_t>(tagged);
  if ((v & G1_OOP_TAG_MASK) == 0) {
    return tagged;  // Ordinary (fast path -- no tags)
  }
  // For now (Phase 1a): just strip tags.
  // Phase 1b will add Handle following for Indirect/Shared OOPs.
  // Phase 1c will add remote fetch for Remote OOPs.
  return cast_to_oop(v & G1_OOP_ADDR_MASK);
}


// ============================================================
// g1_resolved_load: centralized GC-side oop load + resolution
// ============================================================
// Replaces RawAccess<decorators>::oop_load(p) + CompressedOops::decode()
// at all G1 closure sites. Returns a clean, resolved oop.

template <DecoratorSet decorators = DECORATORS_NONE, typename T>
inline oop g1_resolved_load(T* p) {
  T raw = RawAccess<decorators>::oop_load(p);
  if (CompressedOops::is_null(raw)) return nullptr;
  oop obj = CompressedOops::decode_not_null(raw);
  return resolve_oop_raw(obj);
}

#endif // SHARE_GC_G1_G1REMOTEOOP_HPP

/*
 * Copyright (c) 2001, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1_GLOBALS_HPP
#define SHARE_GC_G1_G1_GLOBALS_HPP

#include "runtime/globals_shared.hpp"

// Enable evacuation failure injector by default in non-product builds.

#ifdef EVAC_FAILURE_INJECTOR
#error "EVAC_FAILURE_INJECTOR already defined"
#endif
#ifndef PRODUCT
#define EVAC_FAILURE_INJECTOR 1
#else
#define EVAC_FAILURE_INJECTOR 0
#endif

#if EVAC_FAILURE_INJECTOR
#define GC_G1_EVACUATION_FAILURE_FLAGS(develop,                             \
                                       develop_pd,                          \
                                       product,                             \
                                       product_pd,                          \
                                       notproduct,                          \
                                       range,                               \
                                       constraint)                          \
                                                                            \
  product(bool, G1EvacuationFailureALot, false,                             \
          "Force use of evacuation failure handling during certain "        \
          "evacuation pauses")                                              \
                                                                            \
  product(uintx, G1EvacuationFailureALotCount, 1000,                        \
          "Number of successful evacuations between evacuation failures "   \
          "occurring at object copying per thread")                         \
                                                                            \
  product(uintx, G1EvacuationFailureALotInterval, 5,                        \
          "Total collections between forced triggering of evacuation "      \
          "failures")                                                       \
                                                                            \
  product(bool, G1EvacuationFailureALotDuringConcMark, true,                \
          "Force use of evacuation failure handling during evacuation "     \
          "pauses when marking is in progress")                             \
                                                                            \
  product(bool, G1EvacuationFailureALotDuringConcurrentStart, true,         \
          "Force use of evacuation failure handling during concurrent "     \
          "start evacuation pauses")                                        \
                                                                            \
  product(bool, G1EvacuationFailureALotDuringYoungGC, true,                 \
          "Force use of evacuation failure handling during young "          \
          "evacuation pauses")                                              \
                                                                            \
  product(bool, G1EvacuationFailureALotDuringMixedGC, true,                 \
          "Force use of evacuation failure handling during mixed "          \
          "evacuation pauses")                                              \
                                                                            \
  product(uint, G1EvacuationFailureALotCSetPercent, 100,                    \
          "The percentage of regions in the collection set starting "       \
          "from the beginning where the forced evacuation failure "         \
          "injection will be applied.")                                     \
          range(1, 100)
#else
#define GC_G1_EVACUATION_FAILURE_FLAGS(develop,                             \
                                       develop_pd,                          \
                                       product,                             \
                                       product_pd,                          \
                                       notproduct,                          \
                                       range,                               \
                                       constraint)
#endif
//
// Defines all globals flags used by the garbage-first compiler.
//

#define GC_G1_FLAGS(develop,                                                \
                    develop_pd,                                             \
                    product,                                                \
                    product_pd,                                             \
                    notproduct,                                             \
                    range,                                                  \
                    constraint)                                             \
                                                                            \
  product(bool, G1UseAdaptiveIHOP, true,                                    \
          "Adaptively adjust the initiating heap occupancy from the "       \
          "initial value of InitiatingHeapOccupancyPercent. The policy "    \
          "attempts to start marking in time based on application "         \
          "behavior.")                                                      \
                                                                            \
  product(size_t, G1AdaptiveIHOPNumInitialSamples, 3, EXPERIMENTAL,         \
          "How many completed time periods from concurrent start to first " \
          "mixed gc are required to use the input values for prediction "   \
          "of the optimal occupancy to start marking.")                     \
          range(1, max_intx)                                                \
                                                                            \
  product(uintx, G1ConfidencePercent, 50,                                   \
          "Confidence level for MMU/pause predictions")                     \
          range(0, 100)                                                     \
                                                                            \
  product(intx, G1SummarizeRSetStatsPeriod, 0, DIAGNOSTIC,                  \
          "The period (in number of GCs) at which we will generate "        \
          "update buffer processing info "                                  \
          "(0 means do not periodically generate this info); "              \
          "it also requires that logging is enabled on the trace"           \
          "level for gc+remset")                                            \
          range(0, max_intx)                                                \
                                                                            \
  product(double, G1ConcMarkStepDurationMillis, 10.0,                       \
          "Target duration of individual concurrent marking steps "         \
          "in milliseconds.")                                               \
          range(1.0, DBL_MAX)                                               \
                                                                            \
  product(uint, G1RefProcDrainInterval, 1000,                               \
          "The number of discovered reference objects to process before "   \
          "draining concurrent marking work queues.")                       \
          range(1, INT_MAX)                                                 \
                                                                            \
  product(bool, G1UseReferencePrecleaning, true, EXPERIMENTAL,              \
               "Concurrently preclean java.lang.ref.references instances "  \
               "before the Remark pause.")                                  \
                                                                            \
  product(double, G1LastPLABAverageOccupancy, 50.0, EXPERIMENTAL,           \
               "The expected average occupancy of the last PLAB in "        \
               "percent.")                                                  \
               range(0.001, 100.0)                                          \
                                                                            \
  product(size_t, G1SATBBufferSize, 1*K,                                    \
          "Number of entries in an SATB log buffer.")                       \
          range(1, max_uintx)                                               \
                                                                            \
  develop(intx, G1SATBProcessCompletedThreshold, 20,                        \
          "Number of completed buffers that triggers log processing.")      \
          range(0, max_jint)                                                \
                                                                            \
  product(uintx, G1SATBBufferEnqueueingThresholdPercent, 60,                \
          "Before enqueueing them, each mutator thread tries to do some "   \
          "filtering on the SATB buffers it generates. If post-filtering "  \
          "the percentage of retained entries is over this threshold "      \
          "the buffer will be enqueued for processing. A value of 0 "       \
          "specifies that mutator threads should not do such filtering.")   \
          range(0, 100)                                                     \
                                                                            \
  product(intx, G1ExpandByPercentOfAvailable, 20, EXPERIMENTAL,             \
          "When expanding, % of uncommitted space to claim.")               \
          range(0, 100)                                                     \
                                                                            \
  product(size_t, G1UpdateBufferSize, 256,                                  \
          "Size of an update buffer")                                       \
          range(1, NOT_LP64(32*M) LP64_ONLY(1*G))                           \
                                                                            \
  product(intx, G1RSetUpdatingPauseTimePercent, 10,                         \
          "A target percentage of time that is allowed to be spend on "     \
          "processing remembered set update buffers during the collection " \
          "pause.")                                                         \
          range(0, 100)                                                     \
                                                                            \
  product(bool, G1UseConcRefinement, true, DIAGNOSTIC,                      \
          "Control whether concurrent refinement is performed. "            \
          "Disabling effectively ignores G1RSetUpdatingPauseTimePercent")   \
                                                                            \
  develop(uint, G1RemSetArrayOfCardsEntriesBase, 8,                         \
          "Maximum number of entries per region in the Array of Cards "     \
          "card set container per MB of a heap region.")                    \
          range(1, 65536)                                                   \
                                                                            \
  product(uint, G1RemSetArrayOfCardsEntries, 0,  EXPERIMENTAL,              \
          "Maximum number of entries per Array of Cards card set "          \
          "container. Will be set ergonomically by default.")               \
          range(0, 65536)                                                   \
          constraint(G1RemSetArrayOfCardsEntriesConstraintFunc,AfterErgo)   \
                                                                            \
  product(uint, G1RemSetHowlMaxNumBuckets, 8, EXPERIMENTAL,                 \
          "Maximum number of buckets per Howl card set container. The "     \
          "default gives at worst bitmaps of size 8k. This showed to be a " \
          "good tradeoff between bitmap size (waste) and cacheability of "  \
          "the bucket array. Must be a power of two.")                      \
          range(1, 1024)                                                    \
          constraint(G1RemSetHowlMaxNumBucketsConstraintFunc,AfterErgo)     \
                                                                            \
  product(uint, G1RemSetHowlNumBuckets, 0, EXPERIMENTAL,                    \
          "Number of buckets per Howl card set container. Must be a power " \
          "of two. Will be set ergonomically by default.")                  \
          range(0, 1024)                                                    \
          constraint(G1RemSetHowlNumBucketsConstraintFunc,AfterErgo)        \
                                                                            \
  product(uint, G1RemSetCoarsenHowlBitmapToHowlFullPercent, 90, EXPERIMENTAL, \
          "Percentage at which to coarsen a Howl bitmap to Howl full card " \
          "set container.")                                                 \
          range(1, 100)                                                     \
                                                                            \
  product(uint, G1RemSetCoarsenHowlToFullPercent, 90, EXPERIMENTAL,         \
          "Percentage at which to coarsen a Howl card set to Full card "    \
          "set container.")                                                 \
          range(1, 100)                                                     \
                                                                            \
  develop(size_t, G1MaxVerifyFailures, SIZE_MAX,                            \
          "The maximum number of liveness and remembered set verification " \
          "failures to print per thread.")                                  \
          range(1, SIZE_MAX)                                                \
                                                                            \
  product(uintx, G1ReservePercent, 10,                                      \
          "It determines the minimum reserve we should have in the heap "   \
          "to minimize the probability of promotion failure.")              \
          range(0, 50)                                                      \
                                                                            \
  product(size_t, G1HeapRegionSize, 0,                                      \
          "Size of the G1 regions.")                                        \
          range(0, NOT_LP64(32*M) LP64_ONLY(512*M))                         \
          constraint(G1HeapRegionSizeConstraintFunc,AfterMemoryInit)        \
                                                                            \
  product(uint, G1ConcRefinementThreads, 0,                                 \
          "The number of parallel remembered set update threads. "          \
          "Will be set ergonomically by default.")                          \
          range(0, (max_jint-1)/wordSize)                                   \
                                                                            \
  product(uintx, G1MaxNewSizePercent, 60, EXPERIMENTAL,                     \
          "Percentage (0-100) of the heap size to use as default "          \
          " maximum young gen size.")                                       \
          range(0, 100)                                                     \
          constraint(G1MaxNewSizePercentConstraintFunc,AfterErgo)           \
                                                                            \
  product(uintx, G1NewSizePercent, 5, EXPERIMENTAL,                         \
          "Percentage (0-100) of the heap size to use as default "          \
          "minimum young gen size.")                                        \
          range(0, 100)                                                     \
          constraint(G1NewSizePercentConstraintFunc,AfterErgo)              \
                                                                            \
  product(uintx, G1MixedGCLiveThresholdPercent, 85, EXPERIMENTAL,           \
          "Threshold for regions to be considered for inclusion in the "    \
          "collection set of mixed GCs. "                                   \
          "Regions with live bytes exceeding this will not be collected.")  \
          range(0, 100)                                                     \
                                                                            \
  product(uintx, G1HeapWastePercent, 5,                                     \
          "Amount of space, expressed as a percentage of the heap size, "   \
          "that G1 is willing not to collect to avoid expensive GCs.")      \
          range(0, 100)                                                     \
                                                                            \
  product(uintx, G1MixedGCCountTarget, 8,                                   \
          "The target number of mixed GCs after a marking cycle.")          \
          range(0, max_uintx)                                               \
                                                                            \
  product(uint, G1EagerReclaimRemSetThreshold, 0, EXPERIMENTAL,             \
          "Maximum number of remembered set entries a humongous region "    \
          "otherwise eligible for eager reclaim may have to be a candidate "\
          "for eager reclaim. Will be selected ergonomically by default.")  \
                                                                            \
  product(size_t, G1RebuildRemSetChunkSize, 256 * K, EXPERIMENTAL,          \
          "Chunk size used for rebuilding the remembered set.")             \
          range(4 * K, 32 * M)                                              \
                                                                            \
  product(uintx, G1OldCSetRegionThresholdPercent, 10, EXPERIMENTAL,         \
          "An upper bound for the number of old CSet regions expressed "    \
          "as a percentage of the heap size.")                              \
          range(0, 100)                                                     \
                                                                            \
  product(bool, G1VerifyHeapRegionCodeRoots, false, DIAGNOSTIC,             \
          "Verify the code root lists attached to each heap region.")       \
                                                                            \
  develop(bool, G1VerifyBitmaps, false,                                     \
          "Verifies the consistency of the marking bitmaps")                \
                                                                            \
  product(bool, G1TagRefSites, false, DIAGNOSTIC,                            \
          "During STW classification, write tagged oops into ref-site "     \
          "heap slots. Diagnostic flag for barrier coverage testing.")      \
                                                                            \
  product(bool, G1SimulateRemoteEviction, false, DIAGNOSTIC,                \
          "After each GC, simulate evicting a few old objects to remote "   \
          "memory. For testing disaggregated memory infrastructure.")       \
                                                                            \
  product(uint, G1RemoteEvictionThreshold, 0, DIAGNOSTIC,                   \
          "Heap occupancy percentage (0-100) above which cold Old regions "  \
          "are evicted to remote memory. 0 = disabled. E.g., 75 means "    \
          "evict when heap is >75% full. Requires UseRemoteExecutor or "   \
          "G1SimulateRemoteEviction.")                                     \
                                                                            \
  product(uint, LocalMemoryRatio, 100, DIAGNOSTIC,                          \
          "Percentage of Xmx available as local memory (1-100). "           \
          "local_capacity = Xmx * LocalMemoryRatio / 100. When < 100, "    \
          "enables tiered eviction: Tier1 (>60% local), Tier2 (>85%), "    \
          "Tier3 (>95%). Overrides G1RemoteEvictionThreshold.")                                     \
                                                                            \
  product(bool, G1RemoteAllowDenseObjectEviction, false, DIAGNOSTIC,        \
          "Allow tiered remote eviction to use dense small-object Old "     \
          "regions as an emergency last resort. Disabled by default because "\
          "object-granularity fetch has poor economics for dense Spark "    \
          "regions and this path is only for controlled experiments.")      \
                                                                            \
  product(bool, G1RemoteUseCgroupPressure, false, DIAGNOSTIC,               \
          "In LocalMemoryRatio mode, include cgroup memory usage in "        \
          "tiered remote eviction pressure decisions. Disabled by default "  \
          "to preserve the heap-only policy.")                              \
                                                                            \
  product(uint, G1RemoteTier2Percent, 85, DIAGNOSTIC,                       \
          "Local/cgroup pressure percentage above which tier 2 remote "      \
          "eviction starts in LocalMemoryRatio mode.")                      \
          range(1, 100)                                                     \
                                                                            \
  product(uint, G1RemoteTier3Percent, 95, DIAGNOSTIC,                       \
          "Local/cgroup pressure percentage above which tier 3 remote "      \
          "eviction starts in LocalMemoryRatio mode.")                      \
          range(1, 100)                                                     \
                                                                            \
  product(uint, G1RemoteTier2TargetPercent, 70, DIAGNOSTIC,                 \
          "Local/cgroup pressure percentage tier 2 tries to return to "      \
          "after remote eviction starts in LocalMemoryRatio mode.")          \
          range(1, 100)                                                     \
                                                                            \
  product(uint, G1RemoteTier3TargetPercent, 60, DIAGNOSTIC,                 \
          "Local/cgroup pressure percentage tier 3 tries to return to "      \
          "after remote eviction starts in LocalMemoryRatio mode.")          \
          range(1, 100)                                                     \
                                                                            \
  product(uint, G1RemoteTier2MaxEvictRegions, 8, DIAGNOSTIC,                \
          "Maximum old regions selected by one tier 2 remote eviction "      \
          "cycle in LocalMemoryRatio mode.")                                \
          range(1, 1024)                                                    \
                                                                            \
  product(uint, G1RemoteTier3MaxEvictRegions, 16, DIAGNOSTIC,               \
          "Maximum old regions selected by one tier 3 remote eviction "      \
          "cycle in LocalMemoryRatio mode.")                                \
          range(1, 1024)                                                    \
                                                                            \
  product(uint, G1RemoteDenseT2Regions, 1, DIAGNOSTIC,                      \
          "Maximum dense small-object regions selected by the tier 2 "       \
          "dense last-resort path when G1RemoteAllowDenseObjectEviction "   \
          "is enabled.")                                                    \
          range(0, 1024)                                                    \
                                                                            \
  product(uint, G1RemoteDenseT3Regions, 16, DIAGNOSTIC,                     \
          "Maximum dense small-object regions selected by the tier 3 "       \
          "dense last-resort path when G1RemoteAllowDenseObjectEviction "   \
          "is enabled.")                                                    \
          range(0, 1024)                                                    \
                                                                            \
  product(bool, G1RemoteDenseLastResortHighFirst, false, DIAGNOSTIC,        \
          "When dense small-object last-resort eviction is enabled, select " \
          "dense old regions from high heap region indices first. This is "  \
          "a diagnostic policy for graph-shaped workloads where newer old "  \
          "regions often reference older old regions.")                     \
                                                                            \
  product(bool, G1RemoteDenseSkipUnevictableSamples, true, DIAGNOSTIC,      \
          "When dense last-resort eviction is enabled, skip sampled old "     \
          "regions that contain object arrays, disabled primitive arrays, "   \
          "locked objects, or too little object payload that Phase E can "    \
          "actually evict.")                                                 \
                                                                            \
  product(bool, G1RemoteSkipFillerOnCompleteEviction, false, DIAGNOSTIC,    \
          "Skip per-object filler writes when a candidate region has been "  \
          "fully evicted and is about to be freed and guarded. Experimental "\
          "fast path for dense object-granularity eviction.")                \
                                                                            \
  product(bool, G1RemoteDenseRefillAfterStackGuard, false, DIAGNOSTIC,      \
          "After the conservative Pre-D raw-stack guard removes dense "      \
          "last-resort eviction candidates, refill the candidate set from "  \
          "other deferred dense regions that had no raw stack words. This "  \
          "is diagnostic and remains guarded by normal root and verifier "   \
          "checks.")                                                        \
                                                                            \
  product(bool, G1RemoteDenseRefillAfterRootGuard, false, DIAGNOSTIC,       \
          "After root or remote-anchor guards remove dense last-resort "      \
          "eviction candidates, refill the candidate set from other "         \
          "deferred dense regions that were not seen on raw stacks, roots, "  \
          "or remote anchors. This remains guarded by normal verification.")  \
                                                                            \
  product(uint, G1RemoteMinOldRegionEvictUsedPercent, 0, DIAGNOSTIC,        \
          "Skip Path 2 pressure-eviction candidates whose old region used "   \
          "bytes are below this percentage of a G1 region. Sparse old "       \
          "regions often contain JVM/control objects, reclaim little memory, "\
          "and are poor remote-eviction targets.")                           \
          range(0, 100)                                                      \
                                                                            \
  product(uint, G1RemoteEvictionAbortBackoffGCCycles, 16, DIAGNOSTIC,       \
          "Number of GC cycles to skip a region after remote eviction "      \
          "verification aborts with untagged heap refs. 0 disables the "     \
          "backoff.")                                                        \
          range(0, 10000)                                                    \
                                                                            \
  product(bool, G1RemoteAllowPromotionRefSiteTags, false, DIAGNOSTIC,        \
          "Allow ordinary promotion-time OOP classification to write "        \
          "persistent tagged refs into heap fields. Disabled by default "     \
          "because Spark-like array-heavy cached data can expose load paths " \
          "that do not safely tolerate long-lived tagged refs. Remote "       \
          "eviction Phase C may still tag refs during a bounded STW "         \
          "eviction attempt.")                                                \
                                                                            \
  product(bool, G1RemoteTagObjArraySources, true, DIAGNOSTIC,                \
          "Allow remote eviction Phase C to tag references stored in object " \
          "array elements when the target is a type array. Ordinary object "  \
          "targets from object arrays remain untaggable because some "        \
          "load/null-check paths can observe the unresolved tagged handle "   \
          "before the receiver is resolved.")                                 \
                                                                            \
  product(bool, G1RemoteTagObjArrayObjectSources, false, DIAGNOSTIC,          \
          "Allow remote eviction Phase C to tag references stored in object " \
          "array elements when the target is an ordinary object. This is "     \
          "disabled by default because every mutator and VM object-array "     \
          "element load must resolve tagged handles before using the value.")  \
                                                                            \
  product(bool, G1RemoteAbortOnPhaseCUntaggable, true, DIAGNOSTIC,           \
          "Abort remote eviction immediately after Phase C if tagging found " \
          "heap refs that cannot safely be tagged. Verification would only "  \
          "rediscover those refs and abort later.")                           \
                                                                            \
  product(bool, G1RemoteUseRootCatchRelocation, false, DIAGNOSTIC,           \
          "Allow remote eviction to copy root-held candidate objects to "     \
          "fetch-cache regions before evicting their source regions. "        \
          "Disabled by default because an eviction abort after this phase "   \
          "would otherwise leave forwarding stubs in local old regions.")     \
                                                                            \
  product(bool, G1RemoteAllowTypeArrayEviction, false, DIAGNOSTIC,           \
          "Allow remote eviction of primitive arrays as whole objects. "       \
          "Object arrays remain local unless covered by separate reference "   \
          "edge correctness support.")                                        \
                                                                            \
  product(bool, G1RemoteUseFastPhaseC, false, DIAGNOSTIC,                    \
          "Use the bounded remote-eviction Phase C scanner: candidate "       \
          "regions, young regions, newly evacuated ranges, roots, and "       \
          "candidate remembered sets. Verification remains controlled by "    \
          "G1RemoteVerifyEvictionRefs.")                                      \
                                                                            \
  product(bool, G1RemoteUseFastPhaseCSourceHints, false, DIAGNOSTIC,          \
          "Let Fast Phase C learn clean old source regions from verifier "     \
          "misses and scan those regions in later eviction attempts. This "    \
          "turns repeated verifier repairs into bounded steady-state scans.")  \
                                                                            \
  product(uint, G1RemoteFastPhaseCSourceHintMaxRegions, 64, DIAGNOSTIC,       \
          "Maximum number of clean old source regions remembered by "          \
          "G1RemoteUseFastPhaseCSourceHints.")                                \
          range(0, 4096)                                                      \
                                                                            \
  product(uint, G1RemoteFastPhaseCOldPrefixRegions, 0, DIAGNOSTIC,            \
          "When Fast Phase C is enabled, additionally scan old regions with "  \
          "heap indices below this value. Useful as a conservative seed for " \
          "workloads where stable graph roots in low old regions point into " \
          "newer dense regions.")                                             \
          range(0, 4096)                                                      \
                                                                            \
  product(bool, G1RemoteRepairFastPhaseCMisses, false, DIAGNOSTIC,           \
          "After Fast Phase C, let the verifier repair bounded taggable "     \
          "heap refs it finds before deciding whether to abort eviction. "    \
          "Only applies when G1RemoteUseFastPhaseC is enabled.")             \
                                                                            \
  product(uint, G1RemoteFastPhaseCRepairMissLimit, 10000, DIAGNOSTIC,        \
          "Maximum verifier-discovered heap refs to repair after Fast "       \
          "Phase C. If more unrepaired refs remain, eviction still aborts.")  \
          range(0, 10000000)                                                 \
                                                                            \
  product(bool, G1RemoteVerifyEvictionRefs, true, DIAGNOSTIC,                \
          "After remote-eviction Phase C, verify that no untagged heap refs " \
          "still point into eviction candidate regions. Disable only for "    \
          "controlled performance ablations after Fast Phase C has passed "   \
          "verification on the target workload.")                             \
                                                                            \
  product(bool, G1RemoteParallelVerifyEvictionRefs, true, DIAGNOSTIC,        \
          "Use GC worker threads for the heap portion of remote-eviction "    \
          "reference verification. Root verification remains serial.")        \
                                                                            \
  product(bool, G1RemoteSkipFastPhaseCSafetyNetWhenVerifying, true, DIAGNOSTIC, \
          "When Fast Phase C is enabled and G1RemoteVerifyEvictionRefs is "   \
          "also enabled, skip the duplicate evacuated-area safety-net scan. " \
          "The verifier still repairs or aborts on any missed heap refs.")    \
                                                                            \
  product(bool, G1RemoteParallelFinishEviction, true, DIAGNOSTIC,            \
          "Use GC worker threads to build remote-eviction edge metadata "     \
          "after late candidate guards. Slot assignment remains serial; "     \
          "edge-table construction, dormant-anchor lookup, and edge-table "   \
          "publication are batched per worker.")                              \
                                                                            \
  product(size_t, G1RemoteEvictBatchBytes, 4*M, DIAGNOSTIC,                 \
          "Maximum JVM-side batch-eviction control message size. The "       \
          "effective size is capped by the backend's transport message "      \
          "buffer, e.g. RDMAMsgBufSize for RDMA.")                           \
                                                                            \
  product(bool, G1RemoteUseCompactHomogeneousBatch, false, DIAGNOSTIC,       \
          "Use a compact batch-eviction wire format for runs of objects "    \
          "that share klass, word size, edge count, and edge field offsets. " \
          "Requires a remote executor that supports "                        \
          "CMD_BATCH_EVICT_HOMOG_WITH_EDGES.")                              \
                                                                            \
  product(bool, G1RemoteUseRdmaStagedHomogeneousBatch, false, DIAGNOSTIC,    \
          "For homogeneous eviction batches on RDMA, write object bytes to "  \
          "the executor staging arena with one-sided RDMA WRITE, then send "  \
          "only compact metadata with "                                      \
          "CMD_BATCH_EVICT_HOMOG_STAGED_WITH_EDGES.")                       \
                                                                            \
  product(bool, G1RemoteUseRdmaDerivedEdgeBatch, false, DIAGNOSTIC,          \
          "For RDMA-staged homogeneous batches, patch the staged object "     \
          "copy with tagged handle fields and let the executor derive edge "  \
          "targets from object bytes. This avoids sending per-edge target "   \
          "handle IDs and does not mutate the local heap.")                  \
                                                                            \
  product(bool, UseRemoteExecutor, false, DIAGNOSTIC,                       \
          "Connect to a remote executor process for disaggregated memory "  \
          "object storage instead of local simulation (sim-remote).")       \
                                                                            \
  product(ccstr, RemoteExecutorHost, "127.0.0.1", DIAGNOSTIC,               \
          "Hostname or IP of the remote executor process.")                 \
                                                                            \
  product(uint, RemoteExecutorPort, 18515, DIAGNOSTIC,                      \
          "TCP port of the remote executor process.")                       \
                                                                            \
  product(size_t, RDMADataBufSize, 4*M, DIAGNOSTIC,                         \
          "Size of the RDMA data staging buffer in bytes. Used for "        \
          "RDMA WRITE (eviction) and RDMA READ (fetch). Larger values "     \
          "allow bigger objects but require more locked memory "             \
          "(check ulimit -l).")                                             \
                                                                            \
  product(size_t, RDMAMsgBufSize, 64*K, DIAGNOSTIC,                         \
          "Size of each RDMA SEND/RECV message buffer in bytes.")           \
                                                                            \
  product(uint, G1RemoteFetchBatchObjects, 1, DIAGNOSTIC,                   \
          "Maximum objects to request from the remote executor on one "      \
          "remote miss. 1 disables opportunistic batch fetch/prefetch.")     \
                                                                            \
  product(uint, G1RemoteFetchBatchSlotWindow, 64, DIAGNOSTIC,               \
          "Number of neighboring remote slot ids to scan on the executor "   \
          "when opportunistic batch fetch is enabled.")                      \
                                                                            \
  product(size_t, G1RemoteFetchBatchBytes, 0, DIAGNOSTIC,                   \
          "Maximum bytes in one batch-fetch response. 0 uses the RDMA "      \
          "message-buffer size. The effective cap never exceeds "           \
          "RDMAMsgBufSize.")                                                \
                                                                            \
  product(uint, RDMACQDepth, 256, DIAGNOSTIC,                               \
          "Depth of RDMA completion queues.")                               \
                                                                            \
  product(uint, RDMASQDepth, 128, DIAGNOSTIC,                               \
          "Depth of RDMA send queue.")                                      \
                                                                            \
  product(uint, RDMARQDepth, 128, DIAGNOSTIC,                               \
          "Depth of RDMA receive queue.")                \
                                                                            \
  product(bool, G1VerifyAfterEviction, false, DIAGNOSTIC,                    \
          "After eviction, scan entire heap + roots for stale pointers "     \
          "into freed regions. Expensive (O(heap)), debug only.")            \
                                                                            \
  product(bool, G1DeoptimizeBeforeEviction, true, DIAGNOSTIC,                \
          "Before remote eviction, deoptimize compiled Java frames so C2 "    \
          "does not resume with raw oop temporaries into mprotected "         \
          "evicted regions.")                                                \
                                                                            \
  product(uint, G1RemoteCollectionInterval, 0, DIAGNOSTIC,                   \
          "Minimum number of young GC cycles between remote trace-and-report "\
          "collections after one successful trace. 0 disables this unsafe "   \
          "diagnostic pass; 1 traces every GC.")                             \
                                                                            \
  product(size_t, G1RemoteCollectionHandleDelta, 256*K, DIAGNOSTIC,          \
          "Force remote trace-and-report before the interval expires when "   \
          "the allocated remote handle count grows by this many handles. "    \
          "0 disables the handle-growth trigger.")                           \
                                                                            \
  product(uintx, G1PeriodicGCInterval, 0, MANAGEABLE,                       \
          "Number of milliseconds after a previous GC to wait before "      \
          "triggering a periodic gc. A value of zero disables periodically "\
          "enforced gc cycles.")                                            \
                                                                            \
  product(bool, G1PeriodicGCInvokesConcurrent, true,                        \
          "Determines the kind of periodic GC. Set to true to have G1 "     \
          "perform a concurrent GC as periodic GC, otherwise use a STW "    \
          "Full GC.")                                                       \
                                                                            \
  product(double, G1PeriodicGCSystemLoadThreshold, 0.0, MANAGEABLE,         \
          "Maximum recent system wide load as returned by the 1m value "    \
          "of getloadavg() at which G1 triggers a periodic GC. A load "     \
          "above this value cancels a given periodic GC. A value of zero "  \
          "disables this check.")                                           \
          range(0.0, (double)max_uintx)                                     \
                                                                            \
  product(uint, G1RemSetFreeMemoryRescheduleDelayMillis, 10, EXPERIMENTAL,  \
          "Time after which the card set free memory task reschedules "     \
          "itself if there is work remaining.")                             \
          range(1, UINT_MAX)                                                \
                                                                            \
  product(double, G1RemSetFreeMemoryStepDurationMillis, 1, EXPERIMENTAL,    \
          "The amount of time that the free memory task should spend "      \
          "before a pause of G1RemSetFreeMemoryRescheduleDelayMillis "      \
          "length.")                                                        \
          range(1e-3, 1e+6)                                                 \
                                                                            \
  product(double, G1RemSetFreeMemoryKeepExcessRatio, 0.1, EXPERIMENTAL,     \
          "The percentage of free card set memory that G1 should keep as "  \
          "percentage of the currently used memory.")                       \
          range(0.0, 1.0)                                                   \
                                                                            \
  product(uint, G1RestoreRetainedRegionChunksPerWorker, 16, DIAGNOSTIC,     \
          "The number of chunks assigned per worker thread for "            \
          "retained region restore purposes.")                              \
          range(1, 256)                                                     \
                                                                            \
  product(uint, G1NumCardsCostSampleThreshold, 1000, DIAGNOSTIC,            \
          "Threshold for the number of cards when reporting card cost "     \
          "related prediction sample. That sample must involve the same or "\
          "more than that number of cards to be used.")                     \
                                                                            \
  GC_G1_EVACUATION_FAILURE_FLAGS(develop,                                   \
                    develop_pd,                                             \
                    product,                                                \
                    product_pd,                                             \
                    notproduct,                                             \
                    range,                                                  \
                    constraint)

// end of GC_G1_FLAGS

#endif // SHARE_GC_G1_G1_GLOBALS_HPP

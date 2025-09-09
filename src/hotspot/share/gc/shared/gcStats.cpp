/*
 * Copyright (c) 2003, 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/gcStats.hpp"
#include "gc/shared/gcUtil.inline.hpp"
#include "gc/shared/gc_globals.hpp"

GCStats::GCStats() : _avg_promoted(new AdaptivePaddedNoZeroDevAverage(AdaptiveSizePolicyWeight, PromotedPadding)) {}

#ifdef XHN_THREAD_MAJFLT
// [xhn:thread-majflt]
GCMajfltStats::GCMajfltStats() : _stt_majflt(0), _stt_minflt(0), _stt_user_ms(0), _stt_sys_ms(0) {
}

GCMajfltStats::~GCMajfltStats() {
}

void GCMajfltStats::start() {
  os::get_accum_majflt_minflt_and_cputime(&_stt_majflt, &_stt_minflt, &_stt_user_ms, &_stt_sys_ms);
}

void GCMajfltStats::end_and_log(const char* cause) {
  long _end_majflt, _end_minflt, _end_user_ms, _end_sys_ms;
  os::get_accum_majflt_minflt_and_cputime(&_end_majflt, &_end_minflt, &_end_user_ms, &_end_sys_ms);
  log_info(gc)("Majflt(%s)=%ld (%ld -> %ld)", cause, _end_majflt - _stt_majflt , _stt_majflt, _end_majflt);
  log_info(gc)("Minflt(%s)=%ld (%ld -> %ld)", cause, _end_minflt - _stt_minflt , _stt_minflt, _end_minflt);
  log_info(gc)("PausePhase cputime(%s): user %ldms, sys %ldms", cause, _end_user_ms - _stt_user_ms, _end_sys_ms - _stt_sys_ms);
}
#endif // XHN_THREAD_MAJFLT

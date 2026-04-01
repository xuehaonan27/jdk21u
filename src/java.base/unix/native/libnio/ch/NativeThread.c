/*
 * Copyright (c) 2002, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 */

#include <sys/types.h>
#include <string.h>
#include "jni.h"
#include "jni_util.h"
#include "jvm.h"
#include "jlong.h"
#include "sun_nio_ch_NativeThread.h"
#include "nio_util.h"
#include <signal.h>
#include <pthread.h>

#ifdef __linux__
  /* Also defined in net/linux_close.c */
  #define INTERRUPT_SIGNAL (SIGRTMAX - 2)
#elif defined(_AIX)
  /* Also defined in net/aix_close.c */
  #define INTERRUPT_SIGNAL (SIGRTMAX - 1)
#elif defined(_ALLBSD_SOURCE)
  /* Also defined in net/bsd_close.c */
  #define INTERRUPT_SIGNAL SIGIO
#else
  #error "missing platform-specific definition here"
#endif

static void
nullHandler(int sig)
{
}

JNIEXPORT void JNICALL
Java_sun_nio_ch_NativeThread_init(JNIEnv *env, jclass cl)
{
    /* Install the null handler for INTERRUPT_SIGNAL.  This might overwrite the
     * handler previously installed by <platform>_close.c, but that's okay
     * since neither handler actually does anything.  We install our own
     * handler here simply out of paranoia; ultimately the two mechanisms
     * should somehow be unified, perhaps within the VM.
     */

    struct sigaction sa, osa;
    sa.sa_handler = nullHandler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(INTERRUPT_SIGNAL, &sa, &osa) < 0)
        JNU_ThrowIOExceptionWithLastError(env, "sigaction");
}

JNIEXPORT jlong JNICALL
Java_sun_nio_ch_NativeThread_current0(JNIEnv *env, jclass cl)
{
#ifdef USE_LIBAPTH
    /* For LIBAPTH M:N threads, return a sentinel (-2) instead of the
     * worker pthread_t.  This sentinel:
     * - Is treated as "native thread" by NativeThread.isNativeThread()
     *   (not 0, not -1), so preClose/dup2 interruption still works.
     * - Is skipped by signal0() to avoid sending pthread_kill to the
     *   wrong worker.  M:N threads don't need signals to interrupt I/O
     *   because LIBAPTH's hooks convert blocking I/O to cooperative
     *   yield; fd invalidation via preClose0/dup2 is sufficient.
     *
     * Check: apth_self() != NULL means LIBAPTH is initialized.
     * Dedicated threads (apth_self()->is_dedicated via apth_get_thread_stats)
     * fall through to the normal pthread_self() path.
     */
    {
        extern void *apth_self(void);
        extern int apth_get_thread_stats(void *, void *);
        void *self = apth_self();
        if (self != NULL) {
            struct { int dispatches; double cpu, wall; int thread_class, state; } st;
            if (apth_get_thread_stats(self, &st) == 0 && st.thread_class != 3/*DEDICATED*/) {
                return (jlong)-2;
            }
        }
    }
#endif
    return (jlong)pthread_self();
}

/* Sentinel value for LIBAPTH M:N threads (matches NativeThread.java) */
#define LIBAPTH_MN_THREAD_ID ((jlong)-2)

JNIEXPORT void JNICALL
Java_sun_nio_ch_NativeThread_signal0(JNIEnv *env, jclass cl, jlong thread)
{
#ifdef USE_LIBAPTH
    /* M:N threads don't need signal-based interruption: LIBAPTH hooks
     * handle I/O cooperatively, and fd invalidation via preClose0/dup2
     * is sufficient to interrupt blocked operations. */
    if (thread == LIBAPTH_MN_THREAD_ID) return;
#endif
    int ret;
    ret = pthread_kill((pthread_t)thread, INTERRUPT_SIGNAL);
#ifdef MACOSX
    if (ret != 0 && ret != ESRCH)
#else
    if (ret != 0)
#endif
        JNU_ThrowIOExceptionWithLastError(env, "Thread signal failed");
}

JNIEXPORT jboolean JNICALL
Java_sun_nio_ch_NativeThread_supportPendingSignals0(JNIEnv *env, jclass cl) {
#if defined(_AIX)
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

/*
 * Copyright (c) 2001, 2022, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2014 SAP SE. All rights reserved.
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
#include "runtime/javaThread.hpp"
#include "runtime/threadCritical.hpp"

// put OS-includes here
# include <pthread.h>

#ifdef USE_LIBAPTH
#include <apth.h>
#endif

//
// See threadCritical.hpp for details of this class.
//

#ifdef USE_LIBAPTH

static apth_t         tc_owner;
static apth_mutex_t   tc_mutex;
static apth_once_t    tc_once = 0;
static int            tc_count = 0;

static void tc_init() {
  apth_mutex_init(&tc_mutex, NULL);
  tc_owner = NULL;
}

ThreadCritical::ThreadCritical() {
  apth_once(&tc_once, tc_init);
  apth_t self = apth_self();
  if (!apth_equal(self, tc_owner)) {
    int ret = apth_mutex_lock(&tc_mutex);
    guarantee(ret == 0, "fatal error with apth_mutex_lock()");
    assert(tc_count == 0, "Lock acquired with illegal reentry count.");
    tc_owner = self;
  }
  tc_count++;
}

ThreadCritical::~ThreadCritical() {
  assert(apth_equal(tc_owner, apth_self()), "must have correct owner");
  assert(tc_count > 0, "must have correct count");

  tc_count--;
  if (tc_count == 0) {
    tc_owner = NULL;
    int ret = apth_mutex_unlock(&tc_mutex);
    guarantee(ret == 0, "fatal error with apth_mutex_unlock()");
  }
}

#else // !USE_LIBAPTH

static pthread_t             tc_owner = 0;
static pthread_mutex_t       tc_mutex = PTHREAD_MUTEX_INITIALIZER;
static int                   tc_count = 0;

ThreadCritical::ThreadCritical() {
  pthread_t self = pthread_self();
  if (self != tc_owner) {
    int ret = pthread_mutex_lock(&tc_mutex);
    guarantee(ret == 0, "fatal error with pthread_mutex_lock()");
    assert(tc_count == 0, "Lock acquired with illegal reentry count.");
    tc_owner = self;
  }
  tc_count++;
}

ThreadCritical::~ThreadCritical() {
  assert(tc_owner == pthread_self(), "must have correct owner");
  assert(tc_count > 0, "must have correct count");

  tc_count--;
  if (tc_count == 0) {
    tc_owner = 0;
    int ret = pthread_mutex_unlock(&tc_mutex);
    guarantee(ret == 0, "fatal error with pthread_mutex_unlock()");
  }
}

#endif // USE_LIBAPTH

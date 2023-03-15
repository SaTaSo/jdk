/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "precompiled.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"

#ifdef _WINDOWS
#include <processthreadsapi.h>
#include <timezoneapi.h>
#else
#include <time.h>
#endif

bool ZAdaptiveHeap::_enabled = false;
volatile size_t ZAdaptiveHeap::_barrier_slow_paths = 0;
ZAdaptiveHeap::ZGenerationData ZAdaptiveHeap::_generation_data[2];
NumberSeq ZAdaptiveHeap::_barrier_cpu_time(0.7);
ZLock* ZAdaptiveHeap::_lock = nullptr;

double ZAdaptiveHeap::process_cpu_time() {
#ifdef _WINDOWS
  FILETIME create;
  FILETIME exit;
  FILETIME kernel;
  FILETIME user;

  if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user) == -1) {
    return -1,0;
  }

  SYSTEMTIME user_total;
  if (FileTimeToSystemTime(&user, &user_total) == -1) {
    return -1.0;
  }

  SYSTEMTIME kernel_total;
  if (FileTimeToSystemTime(&kernel, &kernel_total) == -1) {
    return -1.0;
  }

  double user_seconds = double(user_total.wHour) * 3600.0 +
                        double(user_total.wMinute) * 60.0 +
                        double(user_total.wSecond) +
                        double(user_total.wMilliseconds) / 1000.0;

  double kernel_seconds = double(kernel_total.wHour) * 3600.0 +
                          double(kernel_total.wMinute) * 60.0 +
                          double(kernel_total.wSecond) +
                          double(kernel_total.wMilliseconds) / 1000.0;

  return user_seconds + kernel_seconds;
#else
  timespec tp;
  int status = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &tp);
  assert(status == 0, "clock_gettime error: %s", os::strerror(errno));
  if (status != 0) {
    return -1.0;
  }

  return double(tp.tv_sec) + double(tp.tv_nsec) / NANOSECS_PER_SEC;
#endif
}

ZAdaptiveHeap::ZGenerationData& ZAdaptiveHeap::young_data() {
  return _generation_data[(int)ZGenerationId::young];
}

ZAdaptiveHeap::ZGenerationData& ZAdaptiveHeap::old_data() {
  return _generation_data[(int)ZGenerationId::old];
}

void ZAdaptiveHeap::try_enable() {
  double time_now = process_cpu_time();
  if (time_now < 0.0) {
    return;
  }

  _enabled = true;
  young_data()._last_cpu_time = time_now;
  old_data()._last_cpu_time = time_now;
  _lock = new ZLock();
}

void ZAdaptiveHeap::record_barrier_slow_path_time(double seconds) {
  if (!_lock->try_lock()) {
    // Contention - this isn't important enough to block
    return;
  }
  _barrier_cpu_time.add(seconds);
  _lock->unlock();
}

void ZAdaptiveHeap::record_barrier_slow_paths(size_t barrier_slow_paths) {
  Atomic::add(&_barrier_slow_paths, barrier_slow_paths);
}

// Produces values in the range 0 - 1 in an S shape
static double sigmoid_function(double value) {
  return 1.0 / (1.0 + pow(M_E, -value));
}

void ZAdaptiveHeap::adapt(ZGenerationId generation, ZStatCycleStats stats) {
  log_info(gc)("We gonna print something ");
  assert(is_enabled(), "Adapting heap even though adaptation is disabled");
  ZGenerationData& generation_data = _generation_data[(int)generation];

  double time_last = generation_data._last_cpu_time;
  double time_now = process_cpu_time();
  generation_data._last_cpu_time = time_now;

  double total_time = time_now - time_last;
  generation_data._process_cpu_time.add(total_time);

  size_t barriers = Atomic::xchg(&_barrier_slow_paths, (size_t)0u);
  double barrier_slow_path_time;
  {
    ZLocker<ZLock> locker(_lock);
    barrier_slow_path_time = _barrier_cpu_time.davg();
  }
  double avg_barrier_time = barriers * barrier_slow_path_time;
  double avg_gc_time = stats._avg_serial_time + stats._avg_parallelizable_time;
  double avg_total_time = generation_data._process_cpu_time.davg();

  double avg_generation_cpu_overhead = (avg_gc_time + avg_barrier_time) / avg_total_time;
  log_debug(gc, adaptive)("Adaptive barriers " SIZE_FORMAT ", time %f", barriers, barrier_slow_path_time);
  log_debug(gc, adaptive)("Adaptive avg gc time %f, avg barrier time %f, avg total time %f", avg_gc_time, avg_barrier_time, avg_total_time);
  Atomic::store(&generation_data._generation_cpu_overhead, avg_generation_cpu_overhead);

  double young_cpu_overhead = Atomic::load(&young_data()._generation_cpu_overhead);
  double old_cpu_overhead = Atomic::load(&old_data()._generation_cpu_overhead);
  double cpu_overhead = young_cpu_overhead + old_cpu_overhead;

  double cpu_overhead_error = cpu_overhead - (ZCPUOverheadPercent / 100.0);
  double cpu_overhead_sigmoid_error = sigmoid_function(cpu_overhead_error);
  double correction_factor = cpu_overhead_sigmoid_error + 0.5;

  log_debug(gc, adaptive)("Adaptive total time %f, avg gc time %f, avg total CPU time %f, avg young cpu overhead %f, avg old cpu overhead %f, avg total gc overhead %f, cpu overhead error %f sigmoid error %f correction factor %f",
                          total_time, avg_gc_time, avg_total_time, young_cpu_overhead, old_cpu_overhead, cpu_overhead, cpu_overhead_error, cpu_overhead_sigmoid_error, correction_factor);

  ZHeap::heap()->resize_heap(correction_factor);
}

bool ZAdaptiveHeap::is_enabled() {
  return Atomic::load(&_enabled);
}

void ZAdaptiveHeap::disable() {
  Atomic::store(&_enabled, false);
}

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

#ifndef SHARE_GC_Z_ZADAPTIVEHEAP_HPP
#define SHARE_GC_Z_ZADAPTIVEHEAP_HPP

#include "gc/z/zGenerationId.hpp"
#include "memory/allocation.hpp"
#include "gc/z/zStat.hpp"


class ZAdaptiveHeap : public AllStatic {
private:
  static bool _enabled;
  static volatile size_t _barrier_slow_paths;
  static NumberSeq _barrier_cpu_time;
  static ZLock* _lock;

  struct ZGenerationData {
    double _last_cpu_time;
    volatile double _generation_cpu_overhead;
    NumberSeq _process_cpu_time;

    ZGenerationData() :
        _last_cpu_time(),
        _generation_cpu_overhead(),
        _process_cpu_time(0.7 /* alpha */) {}
  };

  static ZGenerationData& young_minor_data();
  static ZGenerationData& young_major_data();
  static ZGenerationData& old_data();

  static ZGenerationData _generation_data[4];

  static double process_cpu_time();

public:
  static bool is_enabled();
  static void try_enable();
  static void disable();

  static void record_barrier_slow_path_time(double seconds);
  static void record_barrier_slow_paths(size_t barrier_slow_paths);

  static void adapt(ZGenerationId generation, ZStatCycleStats stats);
};

#endif // SHARE_GC_Z_ZADAPTIVEHEAP_HPP

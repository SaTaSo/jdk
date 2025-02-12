/*
 * Copyright (c) 2017, 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZSERVICEABILITY_HPP
#define SHARE_GC_Z_ZSERVICEABILITY_HPP

#include "gc/shared/collectorCounters.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "gc/z/zGenerationId.hpp"
#include "memory/allocation.hpp"
#include "services/memoryManager.hpp"
#include "services/memoryPool.hpp"
#include "services/memoryService.hpp"

class ZServiceabilityCounters;

class ZServiceabilityMemoryPool : public CollectedMemoryPool {
private:
  const ZGenerationId _generation_id;

public:
  ZServiceabilityMemoryPool(const char* name, ZGenerationId id, size_t min_capacity, size_t max_capacity);

  virtual size_t used_in_bytes();
  virtual MemoryUsage get_memory_usage();
};

class ZServiceabilityMemoryManager : public GCMemoryManager {
public:
  ZServiceabilityMemoryManager(const char* name,
                               const char* end_message,
                               MemoryPool* young_memory_pool,
                               MemoryPool* old_memory_pool);
};

class ZServiceability {
private:
  const size_t                 _initial_capacity;
  const size_t                 _min_capacity;
  const size_t                 _max_capacity;
  ZServiceabilityMemoryPool    _young_memory_pool;
  ZServiceabilityMemoryPool    _old_memory_pool;
  ZServiceabilityMemoryManager _minor_cycle_memory_manager;
  ZServiceabilityMemoryManager _major_cycle_memory_manager;
  ZServiceabilityMemoryManager _minor_pause_memory_manager;
  ZServiceabilityMemoryManager _major_pause_memory_manager;
  ZServiceabilityCounters*     _counters;

public:
  ZServiceability(size_t initial_capacity,
                  size_t min_capacity,
                  size_t max_capacity);

  void initialize();

  MemoryPool* memory_pool(ZGenerationId id);
  GCMemoryManager* cycle_memory_manager(bool minor);
  GCMemoryManager* pause_memory_manager(bool minor);
  ZServiceabilityCounters* counters();
};

class ZServiceabilityCycleTracer : public StackObj {
private:
  static bool _minor_is_active;

  TraceMemoryManagerStats _memory_manager_stats;

public:
  ZServiceabilityCycleTracer(bool minor);
  ~ZServiceabilityCycleTracer();

  static bool minor_is_active();
};

class ZServiceabilityPauseTracer : public StackObj {
private:
  SvcGCMarker             _svc_gc_marker;
  TraceCollectorStats     _counters_stats;
  TraceMemoryManagerStats _memory_manager_stats;

  bool minor_is_active() const;

public:
  ZServiceabilityPauseTracer();
  ~ZServiceabilityPauseTracer();
};

#endif // SHARE_GC_Z_ZSERVICEABILITY_HPP

/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zGeneration.hpp"
#include "gc/z/zHeap.inline.hpp"

static const ZStatSubPhase ZSubPhaseConcurrentMinorMarkRootRemset("Concurrent Minor Mark Root Remset");

ZGeneration::ZGeneration(ZGenerationId generation_id, ZPageAge age) :
    _generation_id(generation_id),
    _used(0),
    _object_allocator(generation_id, age) {
}

void ZGeneration::increase_used(size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::add(&_used, size, memory_order_relaxed);
}

void ZGeneration::decrease_used(size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::sub(&_used, size, memory_order_relaxed);
}

size_t ZGeneration::used_total() const {
  return Atomic::load(&_used);
}

ZYoungGeneration::ZYoungGeneration(ZPageTable* page_table, ZPageAllocator* page_allocator) :
  ZGeneration(ZGenerationId::young, ZPageAge::eden),
    _remembered(page_table, page_allocator),
    _survivor_allocator(ZGenerationId::young, ZPageAge::survivor) {
}

void ZYoungGeneration::scan_remembered_sets() {
  ZStatTimerYoung timer(ZSubPhaseConcurrentMinorMarkRootRemset);
  _remembered.scan();
}

void ZYoungGeneration::flip_remembered_sets() {
  _remembered.flip();
}

zaddress ZYoungGeneration::alloc_object_for_relocation(size_t size) {
  return _survivor_allocator.alloc_object_for_relocation(size);
}

void ZYoungGeneration::undo_alloc_object_for_relocation(zaddress addr, size_t size) {
  ZPage* const page = ZHeap::heap()->page(addr);
  _survivor_allocator.undo_alloc_object_for_relocation(page, addr, size);
}

void ZYoungGeneration::retire_pages() {
  _object_allocator.retire_pages();
  _survivor_allocator.retire_pages();
}

size_t ZYoungGeneration::used() const {
  return _object_allocator.used() + _survivor_allocator.used();
}

size_t ZYoungGeneration::remaining() const {
  return _object_allocator.remaining() + _survivor_allocator.remaining();
}

size_t ZYoungGeneration::relocated() const {
  return _object_allocator.relocated() + _survivor_allocator.relocated();
}

ZOldGeneration::ZOldGeneration() :
  ZGeneration(ZGenerationId::old, ZPageAge::old) {
}

zaddress ZOldGeneration::alloc_object_for_relocation(size_t size) {
  return _object_allocator.alloc_object_for_relocation(size);
}

void ZOldGeneration::undo_alloc_object_for_relocation(zaddress addr, size_t size) {
  ZPage* const page = ZHeap::heap()->page(addr);
  _object_allocator.undo_alloc_object_for_relocation(page, addr, size);
}

void ZOldGeneration::retire_pages() {
  _object_allocator.retire_pages();
}

size_t ZOldGeneration::used() const {
  return _object_allocator.used();
}

size_t ZOldGeneration::remaining() const {
  return _object_allocator.remaining();
}

size_t ZOldGeneration::relocated() const {
  return _object_allocator.relocated();
}

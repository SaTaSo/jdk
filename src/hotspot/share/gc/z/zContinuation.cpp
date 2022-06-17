/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zContinuation.inline.hpp"
#include "gc/z/zStackChunkGCData.inline.hpp"
#include "runtime/atomic.hpp"

ZContinuation::ZColorStackOopClosure::ZColorStackOopClosure(stackChunkOop chunk)
    : _color(ZStackChunkGCData::color(chunk)) {
}

void ZContinuation::ZColorStackOopClosure::do_oop(oop* p) {
  // Convert zaddress to zpointer
  zaddress_unsafe* p_zaddress_unsafe = (zaddress_unsafe*)p;
  zpointer* p_zpointer = (zpointer*)p;
  *p_zpointer = ZAddress::color(*p_zaddress_unsafe, _color);

}

void ZContinuation::ZColorStackOopClosure::do_oop(narrowOop* p) {
  ShouldNotReachHere();
}

void ZContinuation::ZUncolorStackOopClosure::do_oop(oop* p) {
  zpointer ptr = *(volatile zpointer*)p;
  zaddress addr = ZPointer::uncolor(ptr);
  *(volatile zaddress*)p = addr;
}

void ZContinuation::ZUncolorStackOopClosure::do_oop(narrowOop* p) {
  ShouldNotReachHere();
}

oop ZContinuation::load_oop(stackChunkOop chunk, void* addr) {
  volatile uintptr_t* value_addr = reinterpret_cast<volatile uintptr_t*>(addr);
  uintptr_t value = Atomic::load(value_addr);

  if ((value & ~ZPointerAllMetadataMask) == 0) {
    // Must be null of some sort
    return (oop)NULL;
  }

  uintptr_t impossible_zaddress_mask = ~((ZAddressHeapBase - 1) | ZAddressHeapBase);

  if ((value & impossible_zaddress_mask) != 0) {
    // If it isn't a zaddress, it's a zpointer
    zpointer zptr = to_zpointer(value);
    return to_oop(ZBarrier::load_barrier_on_oop_field_preloaded(NULL /* p */, zptr));
  }

  // Must be zaddress
  zaddress_unsafe zaddr = to_zaddress_unsafe(value);
  // A zaddress can only be written to the chunk when the global color
  // matches the color of the chunk, which was populated when the chunk
  // was allocated. Therefore, we can create a zpointer based on the address
  // and the chunk color.
  uintptr_t color = ZStackChunkGCData::color(chunk);
  zpointer zptr = ZAddress::color(zaddr, color);

  if (!ZPointer::is_load_good(zptr)) {
    return to_oop(ZBarrier::relocate_or_remap(zaddr, ZBarrier::remap_generation(zptr)));
  }

  return to_oop(safe(zaddr));
}

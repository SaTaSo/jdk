/*
 * Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.
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
#include "code/nmethod.hpp"
#include "gc/z/zBarrierSetNMethod.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zOopClosures.hpp"
#include "gc/z/zNMethod.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "logging/log.hpp"

void ZBarrierSetNMethod::nmethod_entry_barrier(nmethod* nm) {
  log_trace(nmethod, barrier)("Entered critical zone for %p", nm);

  // Heal oops and disarm
  ZLocker<ZReentrantLock> locker(ZNMethod::lock_for_nmethod(nm));
  ZNMethodOopClosure cl;
  ZNMethod::nmethod_oops_do(nm, &cl);
}

int* ZBarrierSetNMethod::disarmed_value_address() const {
  const uintptr_t mask_addr = reinterpret_cast<uintptr_t>(&ZAddressBadMask);
  const uintptr_t disarmed_addr = mask_addr + ZNMethodDisarmedOffset;
  return reinterpret_cast<int*>(disarmed_addr);
}

ByteSize ZBarrierSetNMethod::thread_disarmed_offset() const {
  return ZThreadLocalData::nmethod_disarmed_offset();
}

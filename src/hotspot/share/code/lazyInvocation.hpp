/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_CODE_LAZYINVOCATION_HPP
#define SHARE_CODE_LAZYINVOCATION_HPP

#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"

class CallInfo;
class MetadataClosure;
class Method;
class nmethod;
class OopClosure;

// This class contains information required for lazily resolved invocations.
// Generated sites refer to this data so that the races with resolution are
// in data not patched code.
class LazyInvocation : public CHeapObj<mtCode> {
public:
  enum CallKind {
    direct_call,
    vtable_call,
    itable_call
  };

private:
  LazyInvocation*   _next;
  intptr_t          _pc_offset;
  volatile intptr_t _value;           // Method*, vtable, itable or sentinel value.
  Method*           _attached_method; // metadata used by method handles
  InstanceKlass*    _refc;
  CallKind          _call_kind;
  oop               _value_oop;       // if not alive, causes nmethod attached to unload
  oop               _attached_method_oop;

  static intptr_t resolve_method_sentinel();
  static intptr_t resolve_vtable_sentinel() { return -1; }
  static intptr_t resolve_selector_sentinel() { return 0; }

public:
  LazyInvocation(CallKind call_kind, LazyInvocation* next = NULL);

  LazyInvocation* next() const { return _next; }
  intptr_t pc_offset() const { return _pc_offset; }
  Method* attached_method() const { return _attached_method; }
  CallKind call_kind() const { return _call_kind; }
  address value_addr() const { return (address)&_value; }
  address refc_addr() const { return (address)&_refc; }

  void set_pc_offset(intptr_t pc_offset) { _pc_offset = pc_offset; }
  void set_vtable_index(int vtable_index) { _value = vtable_index; }
  void set_selector(uint32_t selector) { _value = selector; }
  void set_method(Method* method);
  void set_refc(InstanceKlass* refc);
  void set_attached_method(Method* method);

  // Returns false if the callinfo is incompatible; deoptimize caller
  bool update(nmethod* nm, CallInfo& callinfo);

  void oops_do(OopClosure* cl);
  void metadata_do(MetadataClosure* cl);
};

#endif // SHARE_CODE_LAZYINVOCATION_HPP

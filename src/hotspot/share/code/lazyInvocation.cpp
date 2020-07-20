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

#include "precompiled.hpp"
#include "code/lazyInvocation.hpp"
#include "code/nmethod.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "interpreter/linkResolver.hpp"
#include "memory/universe.hpp"
#include "oops/klassVtable.hpp"
#include "oops/method.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/debug.hpp"

LazyInvocation::LazyInvocation(CallKind call_kind, LazyInvocation* next)
  : _next(next),
    _pc_offset(0),
    _value(0),
    _attached_method(NULL),
    _refc(NULL),
    _call_kind(call_kind),
    _value_oop(NULL),
    _attached_method_oop(NULL)
{
  if (call_kind == direct_call) {
    _value = LazyInvocation::resolve_method_sentinel();
  } else if (call_kind == vtable_call) {
    _value = LazyInvocation::resolve_vtable_sentinel();
  } else {
    assert(call_kind == itable_call, "must be");
    _value = LazyInvocation::resolve_selector_sentinel();
  }
}

intptr_t LazyInvocation::resolve_method_sentinel() {
  static address resolve_addr = NULL;
  if (resolve_addr == NULL) {
    resolve_addr = SharedRuntime::get_bad_call_stub();
  }
  return intptr_t(&resolve_addr) - in_bytes(Method::from_compiled_offset());
}

void LazyInvocation::set_method(Method* method) {
  _value_oop = method->method_holder()->klass_holder();
  Atomic::release_store(&_value, (intptr_t)method);
}

void LazyInvocation::set_refc(InstanceKlass* refc) {
  _refc = refc;
  _value_oop = refc->klass_holder();
}

void LazyInvocation::set_attached_method(Method* method) {
  _attached_method_oop = method->method_holder()->klass_holder();
  _attached_method = method;
}

void LazyInvocation::oops_do(OopClosure* cl) {
  if ((_call_kind == direct_call && Atomic::load(&_value) != resolve_method_sentinel()) ||
      (_call_kind == itable_call && Atomic::load(&_value) != resolve_selector_sentinel())) {
    // A GC with concurrent class unloading may call this during concurrent execution.
    // Therefore, it is important that we acquire before reading the oops.
    OrderAccess::acquire();
    cl->do_oop(&_value_oop);
  }
  if (_attached_method != NULL) {
    cl->do_oop(&_attached_method_oop);
  }
}

void LazyInvocation::metadata_do(MetadataClosure* cl) {
  if (_call_kind == direct_call && _value != resolve_method_sentinel()) {
    cl->do_metadata((Metadata*)_value);
  }
  if (_attached_method != NULL) {
    cl->do_metadata(_attached_method);
  }
  if (_refc != NULL) {
    cl->do_metadata(_refc);
  }
}

bool LazyInvocation::update(nmethod* nm, CallInfo& callinfo) {
  bool register_oops = false;
  { MutexLocker ml(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
    if (call_kind() == LazyInvocation::direct_call) {
      if (callinfo.call_kind() != CallInfo::direct_call) {
        // Incompatible lazy invocation and link resolved call
        return false;
      }
      if (_value == resolve_method_sentinel()) {
        set_method(callinfo.selected_method());
        register_oops = true;
      }
    } else if (call_kind() == LazyInvocation::vtable_call) {
      if (callinfo.call_kind() != CallInfo::vtable_call) {
        // Incompatible lazy invocation and link resolved call
        return false;
      }
      if (callinfo.vtable_index() >= 0) {
        if (_value == resolve_vtable_sentinel()) {
          set_vtable_index(callinfo.vtable_index());
        }
      } else {
        assert(callinfo.vtable_index() == Method::nonvirtual_vtable_index, "unexpected non-vtable index");
      }
    } else if (call_kind() == LazyInvocation::itable_call) {
      if (callinfo.call_kind() != CallInfo::itable_call) {
        // Incompatible lazy invocation and link resolved call
        return false;
      }
      if (!klassItable::interface_method_needs_itable_index(callinfo.resolved_method())) {
        // Incompatible lazy invocation and link resolved call
        return false;
      }
      if (_value == resolve_selector_sentinel()) {
        set_refc((InstanceKlass*)callinfo.resolved_klass());
        set_selector(callinfo.itable_selector());
      }
    }
  }
  if (register_oops) {
    MutexLocker ml(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    Universe::heap()->register_nmethod(nm);
  }
  return true;
}

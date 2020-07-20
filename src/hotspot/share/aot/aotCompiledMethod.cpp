/*
 * Copyright (c) 2016, 2020, Oracle and/or its affiliates. All rights reserved.
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

#include "aot/aotCodeHeap.hpp"
#include "aot/aotLoader.hpp"
#include "code/codeCache.hpp"
#include "code/nativeInst.hpp"
#include "compiler/compilerOracle.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/os.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/sizes.hpp"
#include "utilities/xmlstream.hpp"

#include <stdio.h>

#if 0
static void metadata_oops_do(Metadata** metadata_begin, Metadata **metadata_end, OopClosure* f) {
  // Visit the metadata/oops section
  for (Metadata** p = metadata_begin; p < metadata_end; p++) {
    Metadata* m = *p;

    intptr_t meta = (intptr_t)m;
    if ((meta & 1) == 1) {
      // already resolved
      m = (Metadata*)(meta & ~1);
    } else {
      continue;
    }
    assert(Metaspace::contains(m), "");
    if (m->is_method()) {
      m = ((Method*)m)->method_holder();
    }
    assert(m->is_klass(), "must be");
    oop o = ((Klass*)m)->klass_holder();
    if (o != NULL) {
      f->do_oop(&o);
    }
  }
}
#endif

address* AOTCompiledMethod::orig_pc_addr(const frame* fr) {
  return (address*) ((address)fr->unextended_sp() + _meta->orig_pc_offset());
}

oop AOTCompiledMethod::oop_at(int index) const {
  if (index == 0) { // 0 is reserved
    return NULL;
  }
  Metadata** entry = _metadata_got + (index - 1);
  intptr_t meta = (intptr_t)*entry;
  if ((meta & 1) == 1) {
    // already resolved
    Klass* k = (Klass*)(meta & ~1);
    return k->java_mirror();
  }
  // The entry is string which we need to resolve.
  const char* meta_name = _heap->get_name_at((int)meta);
  int klass_len = Bytes::get_Java_u2((address)meta_name);
  const char* klass_name = meta_name + 2;
  // Quick check the current method's holder.
  Klass* k = _method->method_holder();

  ResourceMark rm; // for signature_name()
  if (strncmp(k->signature_name(), klass_name, klass_len) != 0) { // Does not match?
    // Search klass in got cells in DSO which have this compiled method.
    k = _heap->get_klass_from_got(klass_name, klass_len, _method);
  }
  int method_name_len = Bytes::get_Java_u2((address)klass_name + klass_len);
  guarantee(method_name_len == 0, "only klass is expected here");
  meta = ((intptr_t)k) | 1;
  *entry = (Metadata*)meta; // Should be atomic on x64
  return k->java_mirror();
}

Metadata* AOTCompiledMethod::metadata_at(int index) const {
  if (index == 0) { // 0 is reserved
    return NULL;
  }
  assert(index - 1 < _metadata_size, "");
  {
    Metadata** entry = _metadata_got + (index - 1);
    intptr_t meta = (intptr_t)*entry;
    if ((meta & 1) == 1) {
      // already resolved
      Metadata *m = (Metadata*)(meta & ~1);
      return m;
    }
    // The entry is string which we need to resolve.
    const char* meta_name = _heap->get_name_at((int)meta);
    int klass_len = Bytes::get_Java_u2((address)meta_name);
    const char* klass_name = meta_name + 2;
    // Quick check the current method's holder.
    Klass* k = _method->method_holder();
    bool klass_matched = true;

    ResourceMark rm; // for signature_name() and find_method()
    if (strncmp(k->signature_name(), klass_name, klass_len) != 0) { // Does not match?
      // Search klass in got cells in DSO which have this compiled method.
      k = _heap->get_klass_from_got(klass_name, klass_len, _method);
      klass_matched = false;
    }
    int method_name_len = Bytes::get_Java_u2((address)klass_name + klass_len);
    if (method_name_len == 0) { // Array or Klass name only?
      meta = ((intptr_t)k) | 1;
      *entry = (Metadata*)meta; // Should be atomic on x64
      return (Metadata*)k;
    } else { // Method
      // Quick check the current method's name.
      Method* m = _method;
      int signature_len = Bytes::get_Java_u2((address)klass_name + klass_len + 2 + method_name_len);
      int full_len = 2 + klass_len + 2 + method_name_len + 2 + signature_len;
      if (!klass_matched || memcmp(_name, meta_name, full_len) != 0) { // Does not match?
        Thread* thread = Thread::current();
        const char* method_name = klass_name + klass_len;
        m = AOTCodeHeap::find_method(k, thread, method_name);
      }
      meta = ((intptr_t)m) | 1;
      *entry = (Metadata*)meta; // Should be atomic on x64
      return (Metadata*)m;
    }
  }
  ShouldNotReachHere(); return NULL;
}

void AOTCompiledMethod::do_unloading(bool unloading_occurred) {
  unload_nmethod_caches(unloading_occurred);
}

bool AOTCompiledMethod::make_not_entrant_helper(int new_state) {
  NoSafepointVerifier nsv;

  {
    // Enter critical section.  Does not block for safepoint.
    MutexLocker pl(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);

    if (*_state_adr == new_state) {
      // another thread already performed this transition so nothing
      // to do, but return false to indicate this.
      return false;
    }

    // Change state
    OrderAccess::storestore();
    *_state_adr = new_state;

    // Log the transition once
    log_state_change();

#ifdef TIERED
    // Remain non-entrant forever
    if (new_state == not_entrant && method() != NULL) {
        method()->set_aot_code(NULL);
    }
#endif

    // Remove AOTCompiledMethod from method.
    if (method() != NULL && (method()->code() == this ||
                             method()->from_compiled_entry() == entry_point())) {
      method()->clear_code(false /* acquire_lock */, false /* update_tables */);
    }
  } // leave critical region under CompiledMethod_lock

  return true;
}

#ifdef TIERED
bool AOTCompiledMethod::make_entrant() {
  assert(!method()->is_old(), "reviving evolved method!");

  NoSafepointVerifier nsv;
  {
    // Enter critical section.  Does not block for safepoint.
    MutexLocker pl(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);

    if (*_state_adr == in_use || *_state_adr == not_entrant) {
      // another thread already performed this transition so nothing
      // to do, but return false to indicate this.
      return false;
    }

    // Change state
    OrderAccess::storestore();
    *_state_adr = in_use;

    // Log the transition once
    log_state_change();
  } // leave critical region under CompiledMethod_lock

  return true;
}
#endif // TIERED

// Iterate over metadata calling this function.   Used by RedefineClasses
// Copied from nmethod::metadata_do
void AOTCompiledMethod::metadata_do(MetadataClosure* f) {
  address low_boundary = entry_point();
  {
    // Visit all immediate references that are embedded in the instruction stream.
    RelocIterator iter(this, low_boundary);
    while (iter.next()) {
      if (iter.type() == relocInfo::metadata_type ) {
        metadata_Relocation* r = iter.metadata_reloc();
        // In this metadata, we must only follow those metadatas directly embedded in
        // the code.  Other metadatas (oop_index>0) are seen as part of
        // the metadata section below.
        assert(1 == (r->metadata_is_immediate()) +
               (r->metadata_addr() >= metadata_begin() && r->metadata_addr() < metadata_end()),
               "metadata must be found in exactly one place");
        if (r->metadata_is_immediate() && r->metadata_value() != NULL) {
          Metadata* md = r->metadata_value();
          if (md != _method) f->do_metadata(md);
        }
      }
    }
  }

  // Visit the metadata section
  for (Metadata** p = metadata_begin(); p < metadata_end(); p++) {
    Metadata* m = *p;

    intptr_t meta = (intptr_t)m;
    if ((meta & 1) == 1) {
      // already resolved
      m = (Metadata*)(meta & ~1);
    } else {
      continue;
    }
    assert(Metaspace::contains(m), "");
    f->do_metadata(m);
  }

  // Visit metadata not embedded in the other places.
  if (_method != NULL) f->do_metadata(_method);
}

void AOTCompiledMethod::print() const {
  print_on(tty, "AOTCompiledMethod");
}

void AOTCompiledMethod::print_on(outputStream* st) const {
  print_on(st, "AOTCompiledMethod");
}

// Print out more verbose output usually for a newly created aot method.
void AOTCompiledMethod::print_on(outputStream* st, const char* msg) const {
  if (st != NULL) {
    ttyLocker ttyl;
    st->print("%7d ", (int) tty->time_stamp().milliseconds());
    st->print("%4d ", _aot_id);    // print compilation number
    st->print("    aot[%2d]", _heap->dso_id());
    // Stubs have _method == NULL
    if (_method == NULL) {
      st->print("   %s", _name);
    } else {
      ResourceMark m;
      st->print("   %s", _method->name_and_sig_as_C_string());
    }
    if (Verbose) {
      st->print(" entry at " INTPTR_FORMAT, p2i(_code));
    }
    if (msg != NULL) {
      st->print("   %s", msg);
    }
    st->cr();
  }
}

void AOTCompiledMethod::print_value_on(outputStream* st) const {
  st->print("AOTCompiledMethod ");
  print_on(st, NULL);
}

// Print a short set of xml attributes to identify this aot method.  The
// output should be embedded in some other element.
void AOTCompiledMethod::log_identity(xmlStream* log) const {
  log->print(" aot_id='%d'", _aot_id);
  log->print(" aot='%2d'", _heap->dso_id());
}

void AOTCompiledMethod::log_state_change() const {
  if (LogCompilation) {
    ResourceMark m;
    if (xtty != NULL) {
      ttyLocker ttyl;  // keep the following output all in one block
      if (*_state_adr == not_entrant) {
        xtty->begin_elem("make_not_entrant thread='" UINTX_FORMAT "'",
                         os::current_thread_id());
      } else if (*_state_adr == not_used) {
        xtty->begin_elem("make_not_used thread='" UINTX_FORMAT "'",
                         os::current_thread_id());
      } else if (*_state_adr == in_use) {
        xtty->begin_elem("make_entrant thread='" UINTX_FORMAT "'",
                         os::current_thread_id());
      }
      log_identity(xtty);
      xtty->stamp();
      xtty->end_elem();
    }
  }
  if (PrintCompilation) {
    ResourceMark m;
    if (*_state_adr == not_entrant) {
      print_on(tty, "made not entrant");
    } else if (*_state_adr == not_used) {
      print_on(tty, "made not used");
    } else if (*_state_adr == in_use) {
      print_on(tty, "made entrant");
    }
  }
}


address AOTCompiledMethod::call_instruction_address(address pc) const {
  NativePltCall* pltcall = nativePltCall_before(pc);
  return pltcall->instruction_address();
}

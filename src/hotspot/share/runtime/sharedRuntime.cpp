/*
 * Copyright (c) 1997, 2020, Oracle and/or its affiliates. All rights reserved.
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
#include "jvm.h"
#include "aot/aotLoader.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/compiledMethod.inline.hpp"
#include "code/scopeDesc.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/gcLocker.inline.hpp"
#include "interpreter/bytecode.inline.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/klass.hpp"
#include "oops/method.inline.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/forte.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/methodHandles.hpp"
#include "prims/nativeLookup.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/init.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vframeArray.hpp"
#include "utilities/copy.hpp"
#include "utilities/dtrace.hpp"
#include "utilities/events.hpp"
#include "utilities/hashtable.inline.hpp"
#include "utilities/macros.hpp"
#include "utilities/xmlstream.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#endif

// Shared stub locations
RuntimeStub*        SharedRuntime::_wrong_method_abstract_blob;
RuntimeStub*        SharedRuntime::_resolve_bad_call_blob;

DeoptimizationBlob* SharedRuntime::_deopt_blob;
SafepointBlob*      SharedRuntime::_polling_page_vectors_safepoint_handler_blob;
SafepointBlob*      SharedRuntime::_polling_page_safepoint_handler_blob;
SafepointBlob*      SharedRuntime::_polling_page_return_handler_blob;

#ifdef COMPILER2
UncommonTrapBlob*   SharedRuntime::_uncommon_trap_blob;
#endif // COMPILER2


//----------------------------generate_stubs-----------------------------------
void SharedRuntime::generate_stubs() {
  _wrong_method_abstract_blob          = generate_resolve_blob(CAST_FROM_FN_PTR(address, SharedRuntime::handle_wrong_method_abstract), "wrong_method_abstract_stub");
  _resolve_bad_call_blob               = generate_resolve_blob(CAST_FROM_FN_PTR(address, SharedRuntime::resolve_bad_call_C),           "resolve_bad_call");

#if COMPILER2_OR_JVMCI
  // Vectors are generated only by C2 and JVMCI.
  bool support_wide = is_wide_vector(MaxVectorSize);
  if (support_wide) {
    _polling_page_vectors_safepoint_handler_blob = generate_handler_blob(CAST_FROM_FN_PTR(address, SafepointSynchronize::handle_polling_page_exception), POLL_AT_VECTOR_LOOP);
  }
#endif // COMPILER2_OR_JVMCI
  _polling_page_safepoint_handler_blob = generate_handler_blob(CAST_FROM_FN_PTR(address, SafepointSynchronize::handle_polling_page_exception), POLL_AT_LOOP);
  _polling_page_return_handler_blob    = generate_handler_blob(CAST_FROM_FN_PTR(address, SafepointSynchronize::handle_polling_page_exception), POLL_AT_RETURN);

  generate_deopt_blob();

#ifdef COMPILER2
  generate_uncommon_trap_blob();
#endif // COMPILER2
}

#include <math.h>

// Implementation of SharedRuntime

#ifndef PRODUCT
// For statistics
int SharedRuntime::_implicit_null_throws = 0;
int SharedRuntime::_implicit_div0_throws = 0;
int SharedRuntime::_throw_null_ctr = 0;

int SharedRuntime::_nof_normal_calls = 0;
int SharedRuntime::_nof_inlined_calls = 0;
int SharedRuntime::_nof_static_calls = 0;
int SharedRuntime::_nof_inlined_static_calls = 0;
int SharedRuntime::_nof_interface_calls = 0;
int SharedRuntime::_nof_inlined_interface_calls = 0;
int SharedRuntime::_nof_removable_exceptions = 0;

int SharedRuntime::_new_instance_ctr=0;
int SharedRuntime::_new_array_ctr=0;
int SharedRuntime::_multi1_ctr=0;
int SharedRuntime::_multi2_ctr=0;
int SharedRuntime::_multi3_ctr=0;
int SharedRuntime::_multi4_ctr=0;
int SharedRuntime::_multi5_ctr=0;
int SharedRuntime::_mon_enter_stub_ctr=0;
int SharedRuntime::_mon_exit_stub_ctr=0;
int SharedRuntime::_mon_enter_ctr=0;
int SharedRuntime::_mon_exit_ctr=0;
int SharedRuntime::_partial_subtype_ctr=0;
int SharedRuntime::_jbyte_array_copy_ctr=0;
int SharedRuntime::_jshort_array_copy_ctr=0;
int SharedRuntime::_jint_array_copy_ctr=0;
int SharedRuntime::_jlong_array_copy_ctr=0;
int SharedRuntime::_oop_array_copy_ctr=0;
int SharedRuntime::_checkcast_array_copy_ctr=0;
int SharedRuntime::_unsafe_array_copy_ctr=0;
int SharedRuntime::_generic_array_copy_ctr=0;
int SharedRuntime::_slow_array_copy_ctr=0;
int SharedRuntime::_find_handler_ctr=0;
int SharedRuntime::_rethrow_ctr=0;
#endif

JRT_LEAF(jlong, SharedRuntime::lmul(jlong y, jlong x))
  return x * y;
JRT_END


JRT_LEAF(jlong, SharedRuntime::ldiv(jlong y, jlong x))
  if (x == min_jlong && y == CONST64(-1)) {
    return x;
  } else {
    return x / y;
  }
JRT_END


JRT_LEAF(jlong, SharedRuntime::lrem(jlong y, jlong x))
  if (x == min_jlong && y == CONST64(-1)) {
    return 0;
  } else {
    return x % y;
  }
JRT_END


const juint  float_sign_mask  = 0x7FFFFFFF;
const juint  float_infinity   = 0x7F800000;
const julong double_sign_mask = CONST64(0x7FFFFFFFFFFFFFFF);
const julong double_infinity  = CONST64(0x7FF0000000000000);

JRT_LEAF(jfloat, SharedRuntime::frem(jfloat  x, jfloat  y))
#ifdef _WIN64
  // 64-bit Windows on amd64 returns the wrong values for
  // infinity operands.
  union { jfloat f; juint i; } xbits, ybits;
  xbits.f = x;
  ybits.f = y;
  // x Mod Infinity == x unless x is infinity
  if (((xbits.i & float_sign_mask) != float_infinity) &&
       ((ybits.i & float_sign_mask) == float_infinity) ) {
    return x;
  }
  return ((jfloat)fmod_winx64((double)x, (double)y));
#else
  return ((jfloat)fmod((double)x,(double)y));
#endif
JRT_END


JRT_LEAF(jdouble, SharedRuntime::drem(jdouble x, jdouble y))
#ifdef _WIN64
  union { jdouble d; julong l; } xbits, ybits;
  xbits.d = x;
  ybits.d = y;
  // x Mod Infinity == x unless x is infinity
  if (((xbits.l & double_sign_mask) != double_infinity) &&
       ((ybits.l & double_sign_mask) == double_infinity) ) {
    return x;
  }
  return ((jdouble)fmod_winx64((double)x, (double)y));
#else
  return ((jdouble)fmod((double)x,(double)y));
#endif
JRT_END

#ifdef __SOFTFP__
JRT_LEAF(jfloat, SharedRuntime::fadd(jfloat x, jfloat y))
  return x + y;
JRT_END

JRT_LEAF(jfloat, SharedRuntime::fsub(jfloat x, jfloat y))
  return x - y;
JRT_END

JRT_LEAF(jfloat, SharedRuntime::fmul(jfloat x, jfloat y))
  return x * y;
JRT_END

JRT_LEAF(jfloat, SharedRuntime::fdiv(jfloat x, jfloat y))
  return x / y;
JRT_END

JRT_LEAF(jdouble, SharedRuntime::dadd(jdouble x, jdouble y))
  return x + y;
JRT_END

JRT_LEAF(jdouble, SharedRuntime::dsub(jdouble x, jdouble y))
  return x - y;
JRT_END

JRT_LEAF(jdouble, SharedRuntime::dmul(jdouble x, jdouble y))
  return x * y;
JRT_END

JRT_LEAF(jdouble, SharedRuntime::ddiv(jdouble x, jdouble y))
  return x / y;
JRT_END

JRT_LEAF(jfloat, SharedRuntime::i2f(jint x))
  return (jfloat)x;
JRT_END

JRT_LEAF(jdouble, SharedRuntime::i2d(jint x))
  return (jdouble)x;
JRT_END

JRT_LEAF(jdouble, SharedRuntime::f2d(jfloat x))
  return (jdouble)x;
JRT_END

JRT_LEAF(int,  SharedRuntime::fcmpl(float x, float y))
  return x>y ? 1 : (x==y ? 0 : -1);  /* x<y or is_nan*/
JRT_END

JRT_LEAF(int,  SharedRuntime::fcmpg(float x, float y))
  return x<y ? -1 : (x==y ? 0 : 1);  /* x>y or is_nan */
JRT_END

JRT_LEAF(int,  SharedRuntime::dcmpl(double x, double y))
  return x>y ? 1 : (x==y ? 0 : -1); /* x<y or is_nan */
JRT_END

JRT_LEAF(int,  SharedRuntime::dcmpg(double x, double y))
  return x<y ? -1 : (x==y ? 0 : 1);  /* x>y or is_nan */
JRT_END

// Functions to return the opposite of the aeabi functions for nan.
JRT_LEAF(int, SharedRuntime::unordered_fcmplt(float x, float y))
  return (x < y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

JRT_LEAF(int, SharedRuntime::unordered_dcmplt(double x, double y))
  return (x < y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

JRT_LEAF(int, SharedRuntime::unordered_fcmple(float x, float y))
  return (x <= y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

JRT_LEAF(int, SharedRuntime::unordered_dcmple(double x, double y))
  return (x <= y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

JRT_LEAF(int, SharedRuntime::unordered_fcmpge(float x, float y))
  return (x >= y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

JRT_LEAF(int, SharedRuntime::unordered_dcmpge(double x, double y))
  return (x >= y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

JRT_LEAF(int, SharedRuntime::unordered_fcmpgt(float x, float y))
  return (x > y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

JRT_LEAF(int, SharedRuntime::unordered_dcmpgt(double x, double y))
  return (x > y) ? 1 : ((g_isnan(x) || g_isnan(y)) ? 1 : 0);
JRT_END

// Intrinsics make gcc generate code for these.
float  SharedRuntime::fneg(float f)   {
  return -f;
}

double SharedRuntime::dneg(double f)  {
  return -f;
}

#endif // __SOFTFP__

#if defined(__SOFTFP__) || defined(E500V2)
// Intrinsics make gcc generate code for these.
double SharedRuntime::dabs(double f)  {
  return (f <= (double)0.0) ? (double)0.0 - f : f;
}

#endif

#if defined(__SOFTFP__) || defined(PPC)
double SharedRuntime::dsqrt(double f) {
  return sqrt(f);
}
#endif

JRT_LEAF(jint, SharedRuntime::f2i(jfloat  x))
  if (g_isnan(x))
    return 0;
  if (x >= (jfloat) max_jint)
    return max_jint;
  if (x <= (jfloat) min_jint)
    return min_jint;
  return (jint) x;
JRT_END


JRT_LEAF(jlong, SharedRuntime::f2l(jfloat  x))
  if (g_isnan(x))
    return 0;
  if (x >= (jfloat) max_jlong)
    return max_jlong;
  if (x <= (jfloat) min_jlong)
    return min_jlong;
  return (jlong) x;
JRT_END


JRT_LEAF(jint, SharedRuntime::d2i(jdouble x))
  if (g_isnan(x))
    return 0;
  if (x >= (jdouble) max_jint)
    return max_jint;
  if (x <= (jdouble) min_jint)
    return min_jint;
  return (jint) x;
JRT_END


JRT_LEAF(jlong, SharedRuntime::d2l(jdouble x))
  if (g_isnan(x))
    return 0;
  if (x >= (jdouble) max_jlong)
    return max_jlong;
  if (x <= (jdouble) min_jlong)
    return min_jlong;
  return (jlong) x;
JRT_END


JRT_LEAF(jfloat, SharedRuntime::d2f(jdouble x))
  return (jfloat)x;
JRT_END


JRT_LEAF(jfloat, SharedRuntime::l2f(jlong x))
  return (jfloat)x;
JRT_END


JRT_LEAF(jdouble, SharedRuntime::l2d(jlong x))
  return (jdouble)x;
JRT_END

// Exception handling across interpreter/compiler boundaries
//
// exception_handler_for_return_address(...) returns the continuation address.
// The continuation address is the entry point of the exception handler of the
// previous frame depending on the return address.

address SharedRuntime::raw_exception_handler_for_return_address(JavaThread* thread, address return_address) {
  assert(frame::verify_return_pc(return_address), "must be a return address: " INTPTR_FORMAT, p2i(return_address));
  assert(thread->frames_to_pop_failed_realloc() == 0 || Interpreter::contains(return_address), "missed frames to pop?");

  // Reset method handle flag.
  thread->set_is_method_handle_return(false);

#if INCLUDE_JVMCI
  // JVMCI's ExceptionHandlerStub expects the thread local exception PC to be clear
  // and other exception handler continuations do not read it
  thread->set_exception_pc(NULL);
#endif // INCLUDE_JVMCI

  // The fastest case first
  CodeBlob* blob = CodeCache::find_blob(return_address);
  CompiledMethod* nm = (blob != NULL) ? blob->as_compiled_method_or_null() : NULL;
  if (nm != NULL) {
    // Set flag if return address is a method handle call site.
    thread->set_is_method_handle_return(nm->is_method_handle_return(return_address));
    // native nmethods don't have exception handlers
    assert(!nm->is_native_method(), "no exception handler");
    assert(nm->header_begin() != nm->exception_begin(), "no exception handler");
    if (nm->is_deopt_pc(return_address)) {
      // If we come here because of a stack overflow, the stack may be
      // unguarded. Reguard the stack otherwise if we return to the
      // deopt blob and the stack bang causes a stack overflow we
      // crash.
      bool guard_pages_enabled = thread->stack_guards_enabled();
      if (!guard_pages_enabled) guard_pages_enabled = thread->reguard_stack();
      if (thread->reserved_stack_activation() != thread->stack_base()) {
        thread->set_reserved_stack_activation(thread->stack_base());
      }
      assert(guard_pages_enabled, "stack banging in deopt blob may cause crash");
      return SharedRuntime::deopt_blob()->unpack_with_exception();
    } else {
      return nm->exception_begin();
    }
  }

  // Entry code
  if (StubRoutines::returns_to_call_stub(return_address)) {
    return StubRoutines::catch_exception_entry();
  }
  // Interpreted code
  if (Interpreter::contains(return_address)) {
    return Interpreter::rethrow_exception_entry();
  }

  guarantee(blob == NULL || !blob->is_runtime_stub(), "caller should have skipped stub");

#ifndef PRODUCT
  { ResourceMark rm;
    tty->print_cr("No exception handler found for exception at " INTPTR_FORMAT " - potential problems:", p2i(return_address));
    tty->print_cr("a) exception happened in (new?) code stubs/buffers that is not handled here");
    tty->print_cr("b) other problem");
  }
#endif // PRODUCT

  ShouldNotReachHere();
  return NULL;
}


JRT_LEAF(address, SharedRuntime::exception_handler_for_return_address(JavaThread* thread, address return_address))
  return raw_exception_handler_for_return_address(thread, return_address);
JRT_END


address SharedRuntime::get_poll_stub(address pc) {
  address stub;
  // Look up the code blob
  CodeBlob *cb = CodeCache::find_blob(pc);

  // Should be an nmethod
  guarantee(cb != NULL && cb->is_compiled(), "safepoint polling: pc must refer to an nmethod");

  // Look up the relocation information
  assert(((CompiledMethod*)cb)->is_at_poll_or_poll_return(pc),
    "safepoint polling: type must be poll");

#ifdef ASSERT
  if (!((NativeInstruction*)pc)->is_safepoint_poll()) {
    tty->print_cr("bad pc: " PTR_FORMAT, p2i(pc));
    Disassembler::decode(cb);
    fatal("Only polling locations are used for safepoint");
  }
#endif

  bool at_poll_return = ((CompiledMethod*)cb)->is_at_poll_return(pc);
  bool has_wide_vectors = ((CompiledMethod*)cb)->has_wide_vectors();
  if (at_poll_return) {
    assert(SharedRuntime::polling_page_return_handler_blob() != NULL,
           "polling page return stub not created yet");
    stub = SharedRuntime::polling_page_return_handler_blob()->entry_point();
  } else if (has_wide_vectors) {
    assert(SharedRuntime::polling_page_vectors_safepoint_handler_blob() != NULL,
           "polling page vectors safepoint stub not created yet");
    stub = SharedRuntime::polling_page_vectors_safepoint_handler_blob()->entry_point();
  } else {
    assert(SharedRuntime::polling_page_safepoint_handler_blob() != NULL,
           "polling page safepoint stub not created yet");
    stub = SharedRuntime::polling_page_safepoint_handler_blob()->entry_point();
  }
  log_debug(safepoint)("... found polling page %s exception at pc = "
                       INTPTR_FORMAT ", stub =" INTPTR_FORMAT,
                       at_poll_return ? "return" : "loop",
                       (intptr_t)pc, (intptr_t)stub);
  return stub;
}


oop SharedRuntime::retrieve_receiver( Symbol* sig, frame caller ) {
  assert(caller.is_interpreted_frame(), "");
  int args_size = ArgumentSizeComputer(sig).size() + 1;
  assert(args_size <= caller.interpreter_frame_expression_stack_size(), "receiver must be on interpreter stack");
  oop result = cast_to_oop(*caller.interpreter_frame_tos_at(args_size - 1));
  assert(Universe::heap()->is_in(result) && oopDesc::is_oop(result), "receiver must be an oop");
  return result;
}


void SharedRuntime::throw_and_post_jvmti_exception(JavaThread *thread, Handle h_exception) {
  if (JvmtiExport::can_post_on_exceptions()) {
    vframeStream vfst(thread, true);
    methodHandle method = methodHandle(thread, vfst.method());
    address bcp = method()->bcp_from(vfst.bci());
    JvmtiExport::post_exception_throw(thread, method(), bcp, h_exception());
  }
  Exceptions::_throw(thread, __FILE__, __LINE__, h_exception);
}

void SharedRuntime::throw_and_post_jvmti_exception(JavaThread *thread, Symbol* name, const char *message) {
  Handle h_exception = Exceptions::new_exception(thread, name, message);
  throw_and_post_jvmti_exception(thread, h_exception);
}

// The interpreter code to call this tracing function is only
// called/generated when UL is on for redefine, class and has the right level
// and tags. Since obsolete methods are never compiled, we don't have
// to modify the compilers to generate calls to this function.
//
JRT_LEAF(int, SharedRuntime::rc_trace_method_entry(
    JavaThread* thread, Method* method))
  if (method->is_obsolete()) {
    // We are calling an obsolete method, but this is not necessarily
    // an error. Our method could have been redefined just after we
    // fetched the Method* from the constant pool.
    ResourceMark rm;
    log_trace(redefine, class, obsolete)("calling obsolete method '%s'", method->name_and_sig_as_C_string());
  }
  return 0;
JRT_END

// ret_pc points into caller; we are returning caller's exception handler
// for given exception
address SharedRuntime::compute_compiled_exc_handler(CompiledMethod* cm, address ret_pc, Handle& exception,
                                                    bool force_unwind, bool top_frame_only, bool& recursive_exception_occurred) {
  assert(cm != NULL, "must exist");
  ResourceMark rm;

#if INCLUDE_JVMCI
  if (cm->is_compiled_by_jvmci()) {
    // lookup exception handler for this pc
    int catch_pco = ret_pc - cm->code_begin();
    ExceptionHandlerTable table(cm);
    HandlerTableEntry *t = table.entry_for(catch_pco, -1, 0);
    if (t != NULL) {
      return cm->code_begin() + t->pco();
    } else {
      return Deoptimization::deoptimize_for_missing_exception_handler(cm);
    }
  }
#endif // INCLUDE_JVMCI

  nmethod* nm = cm->as_nmethod();
  ScopeDesc* sd = nm->scope_desc_at(ret_pc);
  // determine handler bci, if any
  EXCEPTION_MARK;

  int handler_bci = -1;
  int scope_depth = 0;
  if (!force_unwind) {
    int bci = sd->bci();
    bool recursive_exception = false;
    do {
      bool skip_scope_increment = false;
      // exception handler lookup
      Klass* ek = exception->klass();
      methodHandle mh(THREAD, sd->method());
      handler_bci = Method::fast_exception_handler_bci_for(mh, ek, bci, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        recursive_exception = true;
        // We threw an exception while trying to find the exception handler.
        // Transfer the new exception to the exception handle which will
        // be set into thread local storage, and do another lookup for an
        // exception handler for this exception, this time starting at the
        // BCI of the exception handler which caused the exception to be
        // thrown (bugs 4307310 and 4546590). Set "exception" reference
        // argument to ensure that the correct exception is thrown (4870175).
        recursive_exception_occurred = true;
        exception = Handle(THREAD, PENDING_EXCEPTION);
        CLEAR_PENDING_EXCEPTION;
        if (handler_bci >= 0) {
          bci = handler_bci;
          handler_bci = -1;
          skip_scope_increment = true;
        }
      }
      else {
        recursive_exception = false;
      }
      if (!top_frame_only && handler_bci < 0 && !skip_scope_increment) {
        sd = sd->sender();
        if (sd != NULL) {
          bci = sd->bci();
        }
        ++scope_depth;
      }
    } while (recursive_exception || (!top_frame_only && handler_bci < 0 && sd != NULL));
  }

  // found handling method => lookup exception handler
  int catch_pco = ret_pc - nm->code_begin();

  ExceptionHandlerTable table(nm);
  HandlerTableEntry *t = table.entry_for(catch_pco, handler_bci, scope_depth);
  if (t == NULL && (nm->is_compiled_by_c1() || handler_bci != -1)) {
    // Allow abbreviated catch tables.  The idea is to allow a method
    // to materialize its exceptions without committing to the exact
    // routing of exceptions.  In particular this is needed for adding
    // a synthetic handler to unlock monitors when inlining
    // synchronized methods since the unlock path isn't represented in
    // the bytecodes.
    t = table.entry_for(catch_pco, -1, 0);
  }

#ifdef COMPILER1
  if (t == NULL && nm->is_compiled_by_c1()) {
    assert(nm->unwind_handler_begin() != NULL, "");
    return nm->unwind_handler_begin();
  }
#endif

  if (t == NULL) {
    ttyLocker ttyl;
    tty->print_cr("MISSING EXCEPTION HANDLER for pc " INTPTR_FORMAT " and handler bci %d", p2i(ret_pc), handler_bci);
    tty->print_cr("   Exception:");
    exception->print();
    tty->cr();
    tty->print_cr(" Compiled exception table :");
    table.print();
    nm->print_code();
    guarantee(false, "missing exception handler");
    return NULL;
  }

  return nm->code_begin() + t->pco();
}

JRT_ENTRY(void, SharedRuntime::throw_AbstractMethodError(JavaThread* thread))
  // These errors occur only at call sites
  throw_and_post_jvmti_exception(thread, vmSymbols::java_lang_AbstractMethodError());
JRT_END

JRT_ENTRY(void, SharedRuntime::throw_IncompatibleClassChangeError(JavaThread* thread))
  // These errors occur only at call sites
  throw_and_post_jvmti_exception(thread, vmSymbols::java_lang_IncompatibleClassChangeError(), "does not implement the requested interface");
JRT_END

JRT_ENTRY(void, SharedRuntime::throw_ArithmeticException(JavaThread* thread))
  throw_and_post_jvmti_exception(thread, vmSymbols::java_lang_ArithmeticException(), "/ by zero");
JRT_END

JRT_ENTRY(void, SharedRuntime::throw_NullPointerException(JavaThread* thread))
  throw_and_post_jvmti_exception(thread, vmSymbols::java_lang_NullPointerException());
JRT_END

JRT_ENTRY(void, SharedRuntime::throw_NullPointerException_at_call(JavaThread* thread))
  // This entry point is effectively only used for NullPointerExceptions which occur at inline
  // cache sites (when the callee activation is not yet set up) so we are at a call site
  throw_and_post_jvmti_exception(thread, vmSymbols::java_lang_NullPointerException());
JRT_END

JRT_ENTRY(void, SharedRuntime::throw_StackOverflowError(JavaThread* thread))
  throw_StackOverflowError_common(thread, false);
JRT_END

JRT_ENTRY(void, SharedRuntime::throw_delayed_StackOverflowError(JavaThread* thread))
  throw_StackOverflowError_common(thread, true);
JRT_END

void SharedRuntime::throw_StackOverflowError_common(JavaThread* thread, bool delayed) {
  // We avoid using the normal exception construction in this case because
  // it performs an upcall to Java, and we're already out of stack space.
  Thread* THREAD = thread;
  Klass* k = SystemDictionary::StackOverflowError_klass();
  oop exception_oop = InstanceKlass::cast(k)->allocate_instance(CHECK);
  if (delayed) {
    java_lang_Throwable::set_message(exception_oop,
                                     Universe::delayed_stack_overflow_error_message());
  }
  Handle exception (thread, exception_oop);
  if (StackTraceInThrowable) {
    java_lang_Throwable::fill_in_stack_trace(exception);
  }
  // Increment counter for hs_err file reporting
  Atomic::inc(&Exceptions::_stack_overflow_errors);
  throw_and_post_jvmti_exception(thread, exception);
}

address SharedRuntime::continuation_for_implicit_exception(JavaThread* thread,
                                                           address pc,
                                                           ImplicitExceptionKind exception_kind)
{
  address target_pc = NULL;

  if (Interpreter::contains(pc)) {
    switch (exception_kind) {
      case IMPLICIT_NULL:           return Interpreter::throw_NullPointerException_entry();
      case IMPLICIT_DIVIDE_BY_ZERO: return Interpreter::throw_ArithmeticException_entry();
      case STACK_OVERFLOW:          return Interpreter::throw_StackOverflowError_entry();
      default:                      ShouldNotReachHere();
    }
  } else {
    switch (exception_kind) {
      case STACK_OVERFLOW: {
        // Stack overflow only occurs upon frame setup; the callee is
        // going to be unwound. Dispatch to a shared runtime stub
        // which will cause the StackOverflowError to be fabricated
        // and processed.
        // Stack overflow should never occur during deoptimization:
        // the compiled method bangs the stack by as much as the
        // interpreter would need in case of a deoptimization. The
        // deoptimization blob and uncommon trap blob bang the stack
        // in a debug VM to verify the correctness of the compiled
        // method stack banging.
        assert(thread->deopt_mark() == NULL, "no stack overflow from deopt blob/uncommon trap");
        Events::log_exception(thread, "StackOverflowError at " INTPTR_FORMAT, p2i(pc));
        return StubRoutines::throw_StackOverflowError_entry();
      }

      case IMPLICIT_NULL: {
        {
          CodeBlob* cb = CodeCache::find_blob(pc);

          // If code blob is NULL, then return NULL to signal handler to report the SEGV error.
          if (cb == NULL) return NULL;

          // Exception happened in CodeCache. Must be either:
          // 1. Inline-cache check in C2I handler blob,
          // 2. Inline-cache check in nmethod, or
          // 3. Implicit null exception in nmethod

          if (!cb->is_compiled()) {
            bool is_in_blob = cb->is_adapter_blob() || cb->is_method_handles_adapter_blob();
            if (!is_in_blob) {
              // Allow normal crash reporting to handle this
              return NULL;
            }
            Events::log_exception(thread, "NullPointerException in code blob at " INTPTR_FORMAT, p2i(pc));
            // There is no handler here, so we will simply unwind.
            return StubRoutines::throw_NullPointerException_at_call_entry();
          }

          // Otherwise, it's a compiled method.  Consult its exception handlers.
          CompiledMethod* cm = (CompiledMethod*)cb;

          if (cm->method()->is_method_handle_intrinsic()) {
            // exception happened inside MH dispatch code, similar to a vtable stub
            Events::log_exception(thread, "NullPointerException in MH adapter " INTPTR_FORMAT, p2i(pc));
            return StubRoutines::throw_NullPointerException_at_call_entry();
          }

#ifndef PRODUCT
          _implicit_null_throws++;
#endif
          target_pc = cm->continuation_for_implicit_exception(pc, false);
          // If there's an unexpected fault, target_pc might be NULL,
          // in which case we want to fall through into the normal
          // error handling code.
        }

        break; // fall through
      }


      case IMPLICIT_DIVIDE_BY_ZERO: {
        CompiledMethod* cm = CodeCache::find_compiled(pc);
        guarantee(cm != NULL, "must have containing compiled method for implicit division-by-zero exceptions");
#ifndef PRODUCT
        _implicit_div0_throws++;
#endif
        target_pc = cm->continuation_for_implicit_exception(pc, true);
        // If there's an unexpected fault, target_pc might be NULL,
        // in which case we want to fall through into the normal
        // error handling code.
        break; // fall through
      }

      default: ShouldNotReachHere();
    }

    assert(exception_kind == IMPLICIT_NULL || exception_kind == IMPLICIT_DIVIDE_BY_ZERO, "wrong implicit exception kind");

    if (exception_kind == IMPLICIT_NULL) {
#ifndef PRODUCT
      // for AbortVMOnException flag
      Exceptions::debug_check_abort("java.lang.NullPointerException");
#endif //PRODUCT
      Events::log_exception(thread, "Implicit null exception at " INTPTR_FORMAT " to " INTPTR_FORMAT, p2i(pc), p2i(target_pc));
    } else {
#ifndef PRODUCT
      // for AbortVMOnException flag
      Exceptions::debug_check_abort("java.lang.ArithmeticException");
#endif //PRODUCT
      Events::log_exception(thread, "Implicit division by zero exception at " INTPTR_FORMAT " to " INTPTR_FORMAT, p2i(pc), p2i(target_pc));
    }
    return target_pc;
  }

  ShouldNotReachHere();
  return NULL;
}


/**
 * Throws an java/lang/UnsatisfiedLinkError.  The address of this method is
 * installed in the native function entry of all native Java methods before
 * they get linked to their actual native methods.
 *
 * \note
 * This method actually never gets called!  The reason is because
 * the interpreter's native entries call NativeLookup::lookup() which
 * throws the exception when the lookup fails.  The exception is then
 * caught and forwarded on the return from NativeLookup::lookup() call
 * before the call to the native function.  This might change in the future.
 */
JNI_ENTRY(void*, throw_unsatisfied_link_error(JNIEnv* env, ...))
{
  // We return a bad value here to make sure that the exception is
  // forwarded before we look at the return value.
  THROW_(vmSymbols::java_lang_UnsatisfiedLinkError(), (void*)badAddress);
}
JNI_END

address SharedRuntime::native_method_throw_unsatisfied_link_error_entry() {
  return CAST_FROM_FN_PTR(address, &throw_unsatisfied_link_error);
}

JRT_ENTRY_NO_ASYNC(void, SharedRuntime::register_finalizer(JavaThread* thread, oopDesc* obj))
#if INCLUDE_JVMCI
  if (!obj->klass()->has_finalizer()) {
    return;
  }
#endif // INCLUDE_JVMCI
  assert(oopDesc::is_oop(obj), "must be a valid oop");
  assert(obj->klass()->has_finalizer(), "shouldn't be here otherwise");
  InstanceKlass::register_finalizer(instanceOop(obj), CHECK);
JRT_END


jlong SharedRuntime::get_java_tid(Thread* thread) {
  if (thread != NULL) {
    if (thread->is_Java_thread()) {
      oop obj = ((JavaThread*)thread)->threadObj();
      return (obj == NULL) ? 0 : java_lang_Thread::thread_id(obj);
    }
  }
  return 0;
}

/**
 * This function ought to be a void function, but cannot be because
 * it gets turned into a tail-call on sparc, which runs into dtrace bug
 * 6254741.  Once that is fixed we can remove the dummy return value.
 */
int SharedRuntime::dtrace_object_alloc(oopDesc* o, int size) {
  return dtrace_object_alloc_base(Thread::current(), o, size);
}

int SharedRuntime::dtrace_object_alloc_base(Thread* thread, oopDesc* o, int size) {
  assert(DTraceAllocProbes, "wrong call");
  Klass* klass = o->klass();
  Symbol* name = klass->name();
  HOTSPOT_OBJECT_ALLOC(
                   get_java_tid(thread),
                   (char *) name->bytes(), name->utf8_length(), size * HeapWordSize);
  return 0;
}

JRT_LEAF(int, SharedRuntime::dtrace_method_entry(
    JavaThread* thread, Method* method))
  assert(DTraceMethodProbes, "wrong call");
  Symbol* kname = method->klass_name();
  Symbol* name = method->name();
  Symbol* sig = method->signature();
  HOTSPOT_METHOD_ENTRY(
      get_java_tid(thread),
      (char *) kname->bytes(), kname->utf8_length(),
      (char *) name->bytes(), name->utf8_length(),
      (char *) sig->bytes(), sig->utf8_length());
  return 0;
JRT_END

JRT_LEAF(int, SharedRuntime::dtrace_method_exit(
    JavaThread* thread, Method* method))
  assert(DTraceMethodProbes, "wrong call");
  Symbol* kname = method->klass_name();
  Symbol* name = method->name();
  Symbol* sig = method->signature();
  HOTSPOT_METHOD_RETURN(
      get_java_tid(thread),
      (char *) kname->bytes(), kname->utf8_length(),
      (char *) name->bytes(), name->utf8_length(),
      (char *) sig->bytes(), sig->utf8_length());
  return 0;
JRT_END


// Finds receiver, CallInfo (i.e. receiver method), and calling bytecode
// for a call current in progress, i.e., arguments has been pushed on stack
// but callee has not been invoked yet.  Caller frame must be compiled.
void SharedRuntime::find_callee_info(JavaThread* thread,
                                     vframeStream& vfst,
                                     Bytecodes::Code& bc,
                                     CallInfo& callinfo, TRAPS) {
  Handle receiver;

  assert(!vfst.at_end(), "Java frame must exist");

  // Find caller and bci from vframe
  methodHandle caller(THREAD, vfst.method());
  int          bci   = vfst.bci();

  Bytecode_invoke bytecode(caller, bci);
  int bytecode_index = bytecode.index();
  bc = bytecode.invoke_code();
  assert(bc != Bytecodes::_illegal, "not initialized");

  // This register map must be update since we need to find the receiver for
  // compiled frames. The receiver might be in a register.
  RegisterMap reg_map(thread);
  frame stubFrame   = thread->last_frame();
  // Caller-frame is a compiled frame
  frame callerFrame = stubFrame.sender(&reg_map);

  // Find lazy resolution
  address pc = callerFrame.pc();
  CompiledMethod* cm = CodeCache::find_compiled(pc);
  LazyInvocation* lazy = cm->lazy_invocation_at(pc);
  methodHandle attached_method(THREAD, lazy == NULL ? NULL : lazy->attached_method());

  if (attached_method.not_null()) {
    Method* callee = bytecode.static_target(CHECK);
    vmIntrinsics::ID id = callee->intrinsic_id();
    // When VM replaces MH.invokeBasic/linkTo* call with a direct/virtual call,
    // it attaches statically resolved method to the call site.
    if (MethodHandles::is_signature_polymorphic(id) &&
        MethodHandles::is_signature_polymorphic_intrinsic(id)) {
      bc = MethodHandles::signature_polymorphic_intrinsic_bytecode(id);

      // Adjust invocation mode according to the attached method.
      switch (bc) {
        case Bytecodes::_invokevirtual:
          if (attached_method->method_holder()->is_interface()) {
            bc = Bytecodes::_invokeinterface;
          }
          break;
        case Bytecodes::_invokeinterface:
          if (!attached_method->method_holder()->is_interface()) {
            bc = Bytecodes::_invokevirtual;
          }
          break;
        case Bytecodes::_invokehandle:
          if (!MethodHandles::is_signature_polymorphic_method(attached_method())) {
            bc = attached_method->is_static() ? Bytecodes::_invokestatic
                                              : Bytecodes::_invokevirtual;
          }
           break;
         default:
           break;
      }
    }
  }

  bool has_receiver = bc != Bytecodes::_invokestatic &&
                      bc != Bytecodes::_invokedynamic &&
                      bc != Bytecodes::_invokehandle;

  // Find receiver for non-static call
  if (has_receiver) {
    Method* callee = bytecode.static_target(CHECK);
    if (callee == NULL) {
      THROW(vmSymbols::java_lang_NoSuchMethodException());
    }

    // Retrieve from a compiled argument list
    receiver = Handle(THREAD, callerFrame.retrieve_receiver(&reg_map));

    if (receiver.is_null()) {
      THROW(vmSymbols::java_lang_NullPointerException());
    }
  }

  // Resolve method
  if (attached_method.not_null()) {
    // Parameterized by attached method.
    LinkResolver::resolve_invoke(callinfo, receiver, attached_method, bc, CHECK);
  } else {
    constantPoolHandle constants(THREAD, caller->constants());
    LinkResolver::resolve_invoke(callinfo, receiver, constants, bytecode_index, bc, CHECK);
  }

  if (lazy != NULL) {
    // We got here through a lazy resolution. Enter the resolved data
    // to avoid further slowpaths.
    if (!lazy->update(cm->as_nmethod(), callinfo)) {
      // In very rare situations, it is possible that the invocation type emitted
      // for the lazy invocation is incompatible with the link resolved call type.
      // In such rare situations, we just deoptimize the caller.
      log_info(vtables)("Deoptimizing caller due to incorrect lazy invocation type");
      Deoptimization::deoptimize_all_marked(cm->as_nmethod());
    }
  }

  // Update tables to selected method
  if (callinfo.call_kind() == CallInfo::vtable_call) {
    klassVtable vtable = receiver->klass()->vtable();
    MutexLocker pl(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
    vtable.link_code(callinfo.vtable_index(), callinfo.selected_method());
  }
  if (callinfo.call_kind() == CallInfo::itable_call) {
    InstanceKlass* ik = (InstanceKlass*)receiver->klass();
    klassItable itable(ik);
    Method* method = itable.target_method_for_selector(callinfo.itable_selector());
    MutexLocker pl(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
    itable.link_code(method);
  }

  // If this call has a MemberName argument, we might want to link the receiver
  // code tables to make sure calls make progress.
  if (callinfo.resolved_method()->intrinsic_id() == vmIntrinsics::_linkToVirtual ||
      callinfo.resolved_method()->intrinsic_id() == vmIntrinsics::_linkToInterface) {
    Handle receiver = Handle(THREAD, callerFrame.retrieve_receiver(&reg_map));
    klassVtable vtable = receiver->klass()->vtable();
    vtable.link_table_code();

    if (receiver->klass()->is_instance_klass()) {
      InstanceKlass* ik = (InstanceKlass*)receiver->klass();
      klassItable itable(ik);
      itable.link_table_code();
    }
  }

  log_info(vtables)("Slow path call triggered");

#ifdef ASSERT
  // Check that the receiver klass is of the right subtype and that it is initialized for virtual calls
  if (has_receiver) {
    assert(receiver.not_null(), "should have thrown exception");
    Klass* receiver_klass = receiver->klass();
    Klass* rk = NULL;
    if (attached_method.not_null()) {
      // In case there's resolved method attached, use its holder during the check.
      rk = attached_method->method_holder();
    } else {
      // Klass is already loaded.
      constantPoolHandle constants(THREAD, caller->constants());
      rk = constants->klass_ref_at(bytecode_index, CHECK);
    }
    Klass* static_receiver_klass = rk;
    assert(receiver_klass->is_subtype_of(static_receiver_klass),
           "actual receiver must be subclass of static receiver klass");
    if (receiver_klass->is_instance_klass()) {
      if (InstanceKlass::cast(receiver_klass)->is_not_initialized()) {
        tty->print_cr("ERROR: Klass not yet initialized!!");
        receiver_klass->print();
      }
      assert(!InstanceKlass::cast(receiver_klass)->is_not_initialized(), "receiver_klass must be initialized");
    }
  }
#endif
}

// Handle abstract method call
JRT_BLOCK_ENTRY(address, SharedRuntime::handle_wrong_method_abstract(JavaThread* thread))
  // Verbose error message for AbstractMethodError.
  // Get the called method from the invoke bytecode.
  vframeStream vfst(thread, true);
  assert(!vfst.at_end(), "Java frame must exist");
  methodHandle caller(thread, vfst.method());
  Bytecode_invoke invoke(caller, vfst.bci());
  DEBUG_ONLY( invoke.verify(); )

  // Find the compiled caller frame.
  RegisterMap reg_map(thread);
  frame stubFrame = thread->last_frame();
  assert(stubFrame.is_runtime_frame(), "must be");
  frame callerFrame = stubFrame.sender(&reg_map);
  assert(callerFrame.is_compiled_frame(), "must be");

  // Install exception and return forward entry.
  address res = StubRoutines::throw_AbstractMethodError_entry();
  JRT_BLOCK
    methodHandle callee(THREAD, invoke.static_target(thread));
    if (callee.not_null()) {
      oop recv = callerFrame.retrieve_receiver(&reg_map);
      Klass *recv_klass = (recv != NULL) ? recv->klass() : NULL;
      LinkResolver::throw_abstract_method_error(callee, recv_klass, thread);
      res = StubRoutines::forward_exception_entry();
    }
  JRT_BLOCK_END
  return res;
JRT_END

// resolve virtual call and update inline cache to monomorphic
JRT_BLOCK_ENTRY(address, SharedRuntime::resolve_bad_call_C(JavaThread *thread ))
  // 6243940 We might end up in here if the callee is deoptimized
  // as we race to call it.  We don't want to take a safepoint if
  // the caller was interpreted because the caller frame will look
  // interpreted to the stack walkers and arguments are now
  // "compiled" so it is much better to make this transition
  // invisible to the stack walking code. The i2c path will
  // place the callee method in the callee_target. It is stashed
  // there because if we try and find the callee by normal means a
  // safepoint is possible and have trouble gc'ing the compiled args.
  RegisterMap reg_map(thread, false);
  frame stub_frame = thread->last_frame();
  assert(stub_frame.is_runtime_frame(), "sanity check");
  frame caller_frame = stub_frame.sender(&reg_map);

  if (caller_frame.is_interpreted_frame() ||
      caller_frame.is_entry_frame()) {
    Method* callee = thread->callee_target();
    guarantee(callee != NULL && callee->is_method(), "bad handshake");
    thread->set_vm_result_2(callee);
    thread->set_callee_target(NULL);
    if (VM_Version::supports_fast_class_init_checks()) {
      // Bypass class initialization checks in c2i when caller is in native.
      // JNI calls to static methods don't have class initialization checks.
      // Fast class initialization checks are present in c2i adapters and call into
      // SharedRuntime::handle_wrong_method() on the slow path.
      //
      // JVM upcalls may land here as well, but there's a proper check present in
      // LinkResolver::resolve_static_call (called from JavaCalls::call_static),
      // so bypassing it in c2i adapter is benign.
      return callee->get_c2i_no_clinit_check_entry();
    } else {
      return callee->get_c2i_entry();
    }
  }

  methodHandle callee_method;

  JRT_BLOCK
    ResourceMark rm(thread);
    // determine call info & receiver
    // note: a) receiver is NULL for static calls
    //       b) an exception is thrown if receiver is NULL for non-static calls
    CallInfo call_info;
    Bytecodes::Code invoke_code = Bytecodes::_illegal;
    // last java frame on stack (which includes native call frames)
    vframeStream vfst(thread, true); // Do not skip and javaCalls
    find_callee_info(thread, vfst, invoke_code, call_info, CHECK_NULL);
    callee_method = methodHandle(THREAD, call_info.selected_method());
    log_debug(itables)("Resolving to %s", callee_method->name()->as_C_string());
    thread->set_vm_result_2(callee_method());
  JRT_BLOCK_END
  // return compiled code entry point after potential safepoints
  return (address)callee_method->from_compiled_entry();
JRT_END

address SharedRuntime::handle_unsafe_access(JavaThread* thread, address next_pc) {
  // The faulting unsafe accesses should be changed to throw the error
  // synchronously instead. Meanwhile the faulting instruction will be
  // skipped over (effectively turning it into a no-op) and an
  // asynchronous exception will be raised which the thread will
  // handle at a later point. If the instruction is a load it will
  // return garbage.

  // Request an async exception.
  thread->set_pending_unsafe_access_error();

  // Return address of next instruction to execute.
  return next_pc;
}

#ifdef ASSERT
void SharedRuntime::check_member_name_argument_is_last_argument(const methodHandle& method,
                                                                const BasicType* sig_bt,
                                                                const VMRegPair* regs) {
  ResourceMark rm;
  const int total_args_passed = method->size_of_parameters();
  const VMRegPair*    regs_with_member_name = regs;
        VMRegPair* regs_without_member_name = NEW_RESOURCE_ARRAY(VMRegPair, total_args_passed - 1);

  const int member_arg_pos = total_args_passed - 1;
  assert(member_arg_pos >= 0 && member_arg_pos < total_args_passed, "oob");
  assert(sig_bt[member_arg_pos] == T_OBJECT, "dispatch argument must be an object");

  const bool is_outgoing = method->is_method_handle_intrinsic();
  int comp_args_on_stack = java_calling_convention(sig_bt, regs_without_member_name, total_args_passed - 1, is_outgoing);

  for (int i = 0; i < member_arg_pos; i++) {
    VMReg a =    regs_with_member_name[i].first();
    VMReg b = regs_without_member_name[i].first();
    assert(a->value() == b->value(), "register allocation mismatch: a=" INTX_FORMAT ", b=" INTX_FORMAT, a->value(), b->value());
  }
  assert(regs_with_member_name[member_arg_pos].first()->is_valid(), "bad member arg");
}
#endif

// same as JVM_Arraycopy, but called directly from compiled code
JRT_ENTRY(void, SharedRuntime::slow_arraycopy_C(oopDesc* src,  jint src_pos,
                                                oopDesc* dest, jint dest_pos,
                                                jint length,
                                                JavaThread* thread)) {
#ifndef PRODUCT
  _slow_array_copy_ctr++;
#endif
  // Check if we have null pointers
  if (src == NULL || dest == NULL) {
    THROW(vmSymbols::java_lang_NullPointerException());
  }
  // Do the copy.  The casts to arrayOop are necessary to the copy_array API,
  // even though the copy_array API also performs dynamic checks to ensure
  // that src and dest are truly arrays (and are conformable).
  // The copy_array mechanism is awkward and could be removed, but
  // the compilers don't call this function except as a last resort,
  // so it probably doesn't matter.
  src->klass()->copy_array((arrayOopDesc*)src, src_pos,
                                        (arrayOopDesc*)dest, dest_pos,
                                        length, thread);
}
JRT_END

// The caller of generate_class_cast_message() (or one of its callers)
// must use a ResourceMark in order to correctly free the result.
char* SharedRuntime::generate_class_cast_message(
    JavaThread* thread, Klass* caster_klass) {

  // Get target class name from the checkcast instruction
  vframeStream vfst(thread, true);
  assert(!vfst.at_end(), "Java frame must exist");
  Bytecode_checkcast cc(vfst.method(), vfst.method()->bcp_from(vfst.bci()));
  constantPoolHandle cpool(thread, vfst.method()->constants());
  Klass* target_klass = ConstantPool::klass_at_if_loaded(cpool, cc.index());
  Symbol* target_klass_name = NULL;
  if (target_klass == NULL) {
    // This klass should be resolved, but just in case, get the name in the klass slot.
    target_klass_name = cpool->klass_name_at(cc.index());
  }
  return generate_class_cast_message(caster_klass, target_klass, target_klass_name);
}


// The caller of generate_class_cast_message() (or one of its callers)
// must use a ResourceMark in order to correctly free the result.
char* SharedRuntime::generate_class_cast_message(
    Klass* caster_klass, Klass* target_klass, Symbol* target_klass_name) {
  const char* caster_name = caster_klass->external_name();

  assert(target_klass != NULL || target_klass_name != NULL, "one must be provided");
  const char* target_name = target_klass == NULL ? target_klass_name->as_klass_external_name() :
                                                   target_klass->external_name();

  size_t msglen = strlen(caster_name) + strlen("class ") + strlen(" cannot be cast to class ") + strlen(target_name) + 1;

  const char* caster_klass_description = "";
  const char* target_klass_description = "";
  const char* klass_separator = "";
  if (target_klass != NULL && caster_klass->module() == target_klass->module()) {
    caster_klass_description = caster_klass->joint_in_module_of_loader(target_klass);
  } else {
    caster_klass_description = caster_klass->class_in_module_of_loader();
    target_klass_description = (target_klass != NULL) ? target_klass->class_in_module_of_loader() : "";
    klass_separator = (target_klass != NULL) ? "; " : "";
  }

  // add 3 for parenthesis and preceeding space
  msglen += strlen(caster_klass_description) + strlen(target_klass_description) + strlen(klass_separator) + 3;

  char* message = NEW_RESOURCE_ARRAY_RETURN_NULL(char, msglen);
  if (message == NULL) {
    // Shouldn't happen, but don't cause even more problems if it does
    message = const_cast<char*>(caster_klass->external_name());
  } else {
    jio_snprintf(message,
                 msglen,
                 "class %s cannot be cast to class %s (%s%s%s)",
                 caster_name,
                 target_name,
                 caster_klass_description,
                 klass_separator,
                 target_klass_description
                 );
  }
  return message;
}

JRT_LEAF(void, SharedRuntime::reguard_yellow_pages())
  (void) JavaThread::current()->reguard_stack();
JRT_END

void SharedRuntime::monitor_enter_helper(oopDesc* obj, BasicLock* lock, JavaThread* thread) {
  if (!SafepointSynchronize::is_synchronizing()) {
    // Only try quick_enter() if we're not trying to reach a safepoint
    // so that the calling thread reaches the safepoint more quickly.
    if (ObjectSynchronizer::quick_enter(obj, thread, lock)) return;
  }
  // NO_ASYNC required because an async exception on the state transition destructor
  // would leave you with the lock held and it would never be released.
  // The normal monitorenter NullPointerException is thrown without acquiring a lock
  // and the model is that an exception implies the method failed.
  JRT_BLOCK_NO_ASYNC
  if (PrintBiasedLockingStatistics) {
    Atomic::inc(BiasedLocking::slow_path_entry_count_addr());
  }
  Handle h_obj(THREAD, obj);
  ObjectSynchronizer::enter(h_obj, lock, CHECK);
  assert(!HAS_PENDING_EXCEPTION, "Should have no exception here");
  JRT_BLOCK_END
}

// Handles the uncommon case in locking, i.e., contention or an inflated lock.
JRT_BLOCK_ENTRY(void, SharedRuntime::complete_monitor_locking_C(oopDesc* obj, BasicLock* lock, JavaThread* thread))
  SharedRuntime::monitor_enter_helper(obj, lock, thread);
JRT_END

void SharedRuntime::monitor_exit_helper(oopDesc* obj, BasicLock* lock, JavaThread* thread) {
  assert(JavaThread::current() == thread, "invariant");
  // Exit must be non-blocking, and therefore no exceptions can be thrown.
  EXCEPTION_MARK;
  ObjectSynchronizer::exit(obj, lock, THREAD);
}

// Handles the uncommon cases of monitor unlocking in compiled code
JRT_LEAF(void, SharedRuntime::complete_monitor_unlocking_C(oopDesc* obj, BasicLock* lock, JavaThread* thread))
  SharedRuntime::monitor_exit_helper(obj, lock, thread);
JRT_END

#ifndef PRODUCT

void SharedRuntime::print_statistics() {
  ttyLocker ttyl;
  if (xtty != NULL)  xtty->head("statistics type='SharedRuntime'");

  if (_throw_null_ctr) tty->print_cr("%5d implicit null throw", _throw_null_ctr);

  if (CountRemovableExceptions) {
    if (_nof_removable_exceptions > 0) {
      Unimplemented(); // this counter is not yet incremented
      tty->print_cr("Removable exceptions: %d", _nof_removable_exceptions);
    }
  }

  // Dump the JRT_ENTRY counters
  if (_new_instance_ctr) tty->print_cr("%5d new instance requires GC", _new_instance_ctr);
  if (_new_array_ctr) tty->print_cr("%5d new array requires GC", _new_array_ctr);
  if (_multi1_ctr) tty->print_cr("%5d multianewarray 1 dim", _multi1_ctr);
  if (_multi2_ctr) tty->print_cr("%5d multianewarray 2 dim", _multi2_ctr);
  if (_multi3_ctr) tty->print_cr("%5d multianewarray 3 dim", _multi3_ctr);
  if (_multi4_ctr) tty->print_cr("%5d multianewarray 4 dim", _multi4_ctr);
  if (_multi5_ctr) tty->print_cr("%5d multianewarray 5 dim", _multi5_ctr);

  if (_mon_enter_stub_ctr) tty->print_cr("%5d monitor enter stub", _mon_enter_stub_ctr);
  if (_mon_exit_stub_ctr) tty->print_cr("%5d monitor exit stub", _mon_exit_stub_ctr);
  if (_mon_enter_ctr) tty->print_cr("%5d monitor enter slow", _mon_enter_ctr);
  if (_mon_exit_ctr) tty->print_cr("%5d monitor exit slow", _mon_exit_ctr);
  if (_partial_subtype_ctr) tty->print_cr("%5d slow partial subtype", _partial_subtype_ctr);
  if (_jbyte_array_copy_ctr) tty->print_cr("%5d byte array copies", _jbyte_array_copy_ctr);
  if (_jshort_array_copy_ctr) tty->print_cr("%5d short array copies", _jshort_array_copy_ctr);
  if (_jint_array_copy_ctr) tty->print_cr("%5d int array copies", _jint_array_copy_ctr);
  if (_jlong_array_copy_ctr) tty->print_cr("%5d long array copies", _jlong_array_copy_ctr);
  if (_oop_array_copy_ctr) tty->print_cr("%5d oop array copies", _oop_array_copy_ctr);
  if (_checkcast_array_copy_ctr) tty->print_cr("%5d checkcast array copies", _checkcast_array_copy_ctr);
  if (_unsafe_array_copy_ctr) tty->print_cr("%5d unsafe array copies", _unsafe_array_copy_ctr);
  if (_generic_array_copy_ctr) tty->print_cr("%5d generic array copies", _generic_array_copy_ctr);
  if (_slow_array_copy_ctr) tty->print_cr("%5d slow array copies", _slow_array_copy_ctr);
  if (_find_handler_ctr) tty->print_cr("%5d find exception handler", _find_handler_ctr);
  if (_rethrow_ctr) tty->print_cr("%5d rethrow handler", _rethrow_ctr);

  AdapterHandlerLibrary::print_statistics();

  if (xtty != NULL)  xtty->tail("statistics");
}

inline double percent(int x, int y) {
  return 100.0 * x / MAX2(y, 1);
}

class MethodArityHistogram {
 public:
  enum { MAX_ARITY = 256 };
 private:
  static int _arity_histogram[MAX_ARITY];     // histogram of #args
  static int _size_histogram[MAX_ARITY];      // histogram of arg size in words
  static int _max_arity;                      // max. arity seen
  static int _max_size;                       // max. arg size seen

  static void add_method_to_histogram(nmethod* nm) {
    Method* method = nm->method();
    ArgumentCount args(method->signature());
    int arity   = args.size() + (method->is_static() ? 0 : 1);
    int argsize = method->size_of_parameters();
    arity   = MIN2(arity, MAX_ARITY-1);
    argsize = MIN2(argsize, MAX_ARITY-1);
    int count = method->compiled_invocation_count();
    _arity_histogram[arity]  += count;
    _size_histogram[argsize] += count;
    _max_arity = MAX2(_max_arity, arity);
    _max_size  = MAX2(_max_size, argsize);
  }

  void print_histogram_helper(int n, int* histo, const char* name) {
    const int N = MIN2(5, n);
    tty->print_cr("\nHistogram of call arity (incl. rcvr, calls to compiled methods only):");
    double sum = 0;
    double weighted_sum = 0;
    int i;
    for (i = 0; i <= n; i++) { sum += histo[i]; weighted_sum += i*histo[i]; }
    double rest = sum;
    double percent = sum / 100;
    for (i = 0; i <= N; i++) {
      rest -= histo[i];
      tty->print_cr("%4d: %7d (%5.1f%%)", i, histo[i], histo[i] / percent);
    }
    tty->print_cr("rest: %7d (%5.1f%%))", (int)rest, rest / percent);
    tty->print_cr("(avg. %s = %3.1f, max = %d)", name, weighted_sum / sum, n);
  }

  void print_histogram() {
    tty->print_cr("\nHistogram of call arity (incl. rcvr, calls to compiled methods only):");
    print_histogram_helper(_max_arity, _arity_histogram, "arity");
    tty->print_cr("\nSame for parameter size (in words):");
    print_histogram_helper(_max_size, _size_histogram, "size");
    tty->cr();
  }

 public:
  MethodArityHistogram() {
    MutexLocker mu1(Compile_lock, Mutex::_no_safepoint_check_flag); // To filter out nmethods that are not installed yet.
    MutexLocker mu2(CodeCache_lock, Mutex::_no_safepoint_check_flag);
    _max_arity = _max_size = 0;
    for (int i = 0; i < MAX_ARITY; i++) _arity_histogram[i] = _size_histogram[i] = 0;
    CodeCache::nmethods_do(add_method_to_histogram);
    print_histogram();
  }
};

int MethodArityHistogram::_arity_histogram[MethodArityHistogram::MAX_ARITY];
int MethodArityHistogram::_size_histogram[MethodArityHistogram::MAX_ARITY];
int MethodArityHistogram::_max_arity;
int MethodArityHistogram::_max_size;

void SharedRuntime::print_call_statistics(int comp_total) {
  tty->print_cr("Calls from compiled code:");
  int total  = _nof_normal_calls + _nof_interface_calls + _nof_static_calls;
  tty->print_cr("\t%9d   (%4.1f%%) total non-inlined   ", total, percent(total, total));
  tty->print_cr("\t%9d   (%4.1f%%) virtual calls       ", _nof_normal_calls, percent(_nof_normal_calls, total));
  tty->print_cr("\t  %9d  (%3.0f%%)   inlined          ", _nof_inlined_calls, percent(_nof_inlined_calls, _nof_normal_calls));
  tty->print_cr("\t%9d   (%4.1f%%) interface calls     ", _nof_interface_calls, percent(_nof_interface_calls, total));
  tty->print_cr("\t  %9d  (%3.0f%%)   inlined          ", _nof_inlined_interface_calls, percent(_nof_inlined_interface_calls, _nof_interface_calls));
  tty->print_cr("\t%9d   (%4.1f%%) static/special calls", _nof_static_calls, percent(_nof_static_calls, total));
  tty->print_cr("\t  %9d  (%3.0f%%)   inlined          ", _nof_inlined_static_calls, percent(_nof_inlined_static_calls, _nof_static_calls));
  tty->cr();
  tty->print_cr("Note 1: counter updates are not MT-safe.");
  tty->print_cr("Note 2: %% in major categories are relative to total non-inlined calls;");
  tty->print_cr("        %% in nested categories are relative to their category");
  tty->print_cr("        (and thus add up to more than 100%% with inlining)");
  tty->cr();

  MethodArityHistogram h;
}
#endif


// A simple wrapper class around the calling convention information
// that allows sharing of adapters for the same calling convention.
class AdapterFingerPrint : public CHeapObj<mtCode> {
 private:
  enum {
    _basic_type_bits = 4,
    _basic_type_mask = right_n_bits(_basic_type_bits),
    _basic_types_per_int = BitsPerInt / _basic_type_bits,
    _compact_int_count = 3
  };
  // TO DO:  Consider integrating this with a more global scheme for compressing signatures.
  // For now, 4 bits per components (plus T_VOID gaps after double/long) is not excessive.

  union {
    int  _compact[_compact_int_count];
    int* _fingerprint;
  } _value;
  int _length; // A negative length indicates the fingerprint is in the compact form,
               // Otherwise _value._fingerprint is the array.

  // Remap BasicTypes that are handled equivalently by the adapters.
  // These are correct for the current system but someday it might be
  // necessary to make this mapping platform dependent.
  static int adapter_encoding(BasicType in) {
    switch (in) {
      case T_BOOLEAN:
      case T_BYTE:
      case T_SHORT:
      case T_CHAR:
        // There are all promoted to T_INT in the calling convention
        return T_INT;

      case T_OBJECT:
      case T_ARRAY:
        // In other words, we assume that any register good enough for
        // an int or long is good enough for a managed pointer.
#ifdef _LP64
        return T_LONG;
#else
        return T_INT;
#endif

      case T_INT:
      case T_LONG:
      case T_FLOAT:
      case T_DOUBLE:
      case T_VOID:
        return in;

      default:
        ShouldNotReachHere();
        return T_CONFLICT;
    }
  }

 public:
  AdapterFingerPrint(int total_args_passed, BasicType* sig_bt) {
    // The fingerprint is based on the BasicType signature encoded
    // into an array of ints with eight entries per int.
    int* ptr;
    int len = (total_args_passed + (_basic_types_per_int-1)) / _basic_types_per_int;
    if (len <= _compact_int_count) {
      assert(_compact_int_count == 3, "else change next line");
      _value._compact[0] = _value._compact[1] = _value._compact[2] = 0;
      // Storing the signature encoded as signed chars hits about 98%
      // of the time.
      _length = -len;
      ptr = _value._compact;
    } else {
      _length = len;
      _value._fingerprint = NEW_C_HEAP_ARRAY(int, _length, mtCode);
      ptr = _value._fingerprint;
    }

    // Now pack the BasicTypes with 8 per int
    int sig_index = 0;
    for (int index = 0; index < len; index++) {
      int value = 0;
      for (int byte = 0; byte < _basic_types_per_int; byte++) {
        int bt = ((sig_index < total_args_passed)
                  ? adapter_encoding(sig_bt[sig_index++])
                  : 0);
        assert((bt & _basic_type_mask) == bt, "must fit in 4 bits");
        value = (value << _basic_type_bits) | bt;
      }
      ptr[index] = value;
    }
  }

  ~AdapterFingerPrint() {
    if (_length > 0) {
      FREE_C_HEAP_ARRAY(int, _value._fingerprint);
    }
  }

  int value(int index) {
    if (_length < 0) {
      return _value._compact[index];
    }
    return _value._fingerprint[index];
  }
  int length() {
    if (_length < 0) return -_length;
    return _length;
  }

  bool is_compact() {
    return _length <= 0;
  }

  unsigned int compute_hash() {
    int hash = 0;
    for (int i = 0; i < length(); i++) {
      int v = value(i);
      hash = (hash << 8) ^ v ^ (hash >> 5);
    }
    return (unsigned int)hash;
  }

  const char* as_string() {
    stringStream st;
    st.print("0x");
    for (int i = 0; i < length(); i++) {
      st.print("%08x", value(i));
    }
    return st.as_string();
  }

  bool equals(AdapterFingerPrint* other) {
    if (other->_length != _length) {
      return false;
    }
    if (_length < 0) {
      assert(_compact_int_count == 3, "else change next line");
      return _value._compact[0] == other->_value._compact[0] &&
             _value._compact[1] == other->_value._compact[1] &&
             _value._compact[2] == other->_value._compact[2];
    } else {
      for (int i = 0; i < _length; i++) {
        if (_value._fingerprint[i] != other->_value._fingerprint[i]) {
          return false;
        }
      }
    }
    return true;
  }
};


// A hashtable mapping from AdapterFingerPrints to AdapterHandlerEntries
class AdapterHandlerTable : public BasicHashtable<mtCode> {
  friend class AdapterHandlerTableIterator;

 private:

#ifndef PRODUCT
  static int _lookups; // number of calls to lookup
  static int _buckets; // number of buckets checked
  static int _equals;  // number of buckets checked with matching hash
  static int _hits;    // number of successful lookups
  static int _compact; // number of equals calls with compact signature
#endif

  AdapterHandlerEntry* bucket(int i) {
    return (AdapterHandlerEntry*)BasicHashtable<mtCode>::bucket(i);
  }

 public:
  AdapterHandlerTable()
    : BasicHashtable<mtCode>(293, (DumpSharedSpaces ? sizeof(CDSAdapterHandlerEntry) : sizeof(AdapterHandlerEntry))) { }

  // Create a new entry suitable for insertion in the table
  AdapterHandlerEntry* new_entry(AdapterFingerPrint* fingerprint, address i2c_entry,
                                 address c2i_entry, address c2i_itable_entry,
                                 address c2i_vtable_entry, address c2i_no_clinit_check_entry) {
    AdapterHandlerEntry* entry = (AdapterHandlerEntry*)BasicHashtable<mtCode>::new_entry(fingerprint->compute_hash());
    entry->init(fingerprint, i2c_entry, c2i_entry, c2i_itable_entry, c2i_vtable_entry, c2i_no_clinit_check_entry);
    if (DumpSharedSpaces) {
      ((CDSAdapterHandlerEntry*)entry)->init();
    }
    return entry;
  }

  // Insert an entry into the table
  void add(AdapterHandlerEntry* entry) {
    int index = hash_to_index(entry->hash());
    add_entry(index, entry);
  }

  void free_entry(AdapterHandlerEntry* entry) {
    entry->deallocate();
    BasicHashtable<mtCode>::free_entry(entry);
  }

  // Find a entry with the same fingerprint if it exists
  AdapterHandlerEntry* lookup(int total_args_passed, BasicType* sig_bt) {
    NOT_PRODUCT(_lookups++);
    AdapterFingerPrint fp(total_args_passed, sig_bt);
    unsigned int hash = fp.compute_hash();
    int index = hash_to_index(hash);
    for (AdapterHandlerEntry* e = bucket(index); e != NULL; e = e->next()) {
      NOT_PRODUCT(_buckets++);
      if (e->hash() == hash) {
        NOT_PRODUCT(_equals++);
        if (fp.equals(e->fingerprint())) {
#ifndef PRODUCT
          if (fp.is_compact()) _compact++;
          _hits++;
#endif
          return e;
        }
      }
    }
    return NULL;
  }

#ifndef PRODUCT
  void print_statistics() {
    ResourceMark rm;
    int longest = 0;
    int empty = 0;
    int total = 0;
    int nonempty = 0;
    for (int index = 0; index < table_size(); index++) {
      int count = 0;
      for (AdapterHandlerEntry* e = bucket(index); e != NULL; e = e->next()) {
        count++;
      }
      if (count != 0) nonempty++;
      if (count == 0) empty++;
      if (count > longest) longest = count;
      total += count;
    }
    tty->print_cr("AdapterHandlerTable: empty %d longest %d total %d average %f",
                  empty, longest, total, total / (double)nonempty);
    tty->print_cr("AdapterHandlerTable: lookups %d buckets %d equals %d hits %d compact %d",
                  _lookups, _buckets, _equals, _hits, _compact);
  }
#endif
};


#ifndef PRODUCT

int AdapterHandlerTable::_lookups;
int AdapterHandlerTable::_buckets;
int AdapterHandlerTable::_equals;
int AdapterHandlerTable::_hits;
int AdapterHandlerTable::_compact;

#endif

class AdapterHandlerTableIterator : public StackObj {
 private:
  AdapterHandlerTable* _table;
  int _index;
  AdapterHandlerEntry* _current;

  void scan() {
    while (_index < _table->table_size()) {
      AdapterHandlerEntry* a = _table->bucket(_index);
      _index++;
      if (a != NULL) {
        _current = a;
        return;
      }
    }
  }

 public:
  AdapterHandlerTableIterator(AdapterHandlerTable* table): _table(table), _index(0), _current(NULL) {
    scan();
  }
  bool has_next() {
    return _current != NULL;
  }
  AdapterHandlerEntry* next() {
    if (_current != NULL) {
      AdapterHandlerEntry* result = _current;
      _current = _current->next();
      if (_current == NULL) scan();
      return result;
    } else {
      return NULL;
    }
  }
};


// ---------------------------------------------------------------------------
// Implementation of AdapterHandlerLibrary
AdapterHandlerTable* AdapterHandlerLibrary::_adapters = NULL;
AdapterHandlerEntry* AdapterHandlerLibrary::_abstract_method_handler = NULL;
const int AdapterHandlerLibrary_size = 16*K;
BufferBlob* AdapterHandlerLibrary::_buffer = NULL;

BufferBlob* AdapterHandlerLibrary::buffer_blob() {
  // Should be called only when AdapterHandlerLibrary_lock is active.
  if (_buffer == NULL) // Initialize lazily
      _buffer = BufferBlob::create("adapters", AdapterHandlerLibrary_size);
  return _buffer;
}

extern "C" void unexpected_adapter_call() {
  ShouldNotCallThis();
}

void AdapterHandlerLibrary::initialize() {
  if (_adapters != NULL) return;
  _adapters = new AdapterHandlerTable();

  // Create a special handler for abstract methods.  Abstract methods
  // are never compiled so an i2c entry is somewhat meaningless, but
  // throw AbstractMethodError just in case.
  // Pass wrong_method_abstract for the c2i transitions to return
  // AbstractMethodError for invalid invocations.
  address wrong_method_abstract = SharedRuntime::get_handle_wrong_method_abstract_stub();
  _abstract_method_handler = AdapterHandlerLibrary::new_entry(new AdapterFingerPrint(0, NULL),
                                                              StubRoutines::throw_AbstractMethodError_entry(),
                                                              wrong_method_abstract, wrong_method_abstract,
                                                              wrong_method_abstract, wrong_method_abstract);
}

AdapterHandlerEntry* AdapterHandlerLibrary::new_entry(AdapterFingerPrint* fingerprint,
                                                      address i2c_entry,
                                                      address c2i_entry,
                                                      address c2i_itable_entry,
                                                      address c2i_vtable_entry,
                                                      address c2i_no_clinit_check_entry) {
  return _adapters->new_entry(fingerprint, i2c_entry, c2i_entry, c2i_itable_entry, c2i_vtable_entry, c2i_no_clinit_check_entry);
}

AdapterHandlerEntry* AdapterHandlerLibrary::get_adapter(const methodHandle& method) {
  AdapterHandlerEntry* entry = get_adapter0(method);
  if (entry != NULL && method->is_shared()) {
    // See comments around Method::link_method()
    MutexLocker mu(AdapterHandlerLibrary_lock);
    if (method->adapter() == NULL) {
      method->update_adapter_trampoline(entry);
    }
    address trampoline = method->from_compiled_entry();
    if (*(int*)trampoline == 0) {
      CodeBuffer buffer(trampoline, (int)SharedRuntime::trampoline_size());
      MacroAssembler _masm(&buffer);
      SharedRuntime::generate_trampoline(&_masm, entry->get_c2i_entry());
      assert(*(int*)trampoline != 0, "Instruction(s) for trampoline must not be encoded as zeros.");
      _masm.flush();

      if (PrintInterpreter) {
        Disassembler::decode(buffer.insts_begin(), buffer.insts_end());
      }
    }
  }

  return entry;
}

AdapterHandlerEntry* AdapterHandlerLibrary::get_adapter0(const methodHandle& method) {
  // Use customized signature handler.  Need to lock around updates to
  // the AdapterHandlerTable (it is not safe for concurrent readers
  // and a single writer: this could be fixed if it becomes a
  // problem).

  ResourceMark rm;

  NOT_PRODUCT(int insts_size);
  AdapterBlob* new_adapter = NULL;
  AdapterHandlerEntry* entry = NULL;
  AdapterFingerPrint* fingerprint = NULL;
  {
    MutexLocker mu(AdapterHandlerLibrary_lock);
    // make sure data structure is initialized
    initialize();

    if (method->is_abstract()) {
      return _abstract_method_handler;
    }

    // Fill in the signature array, for the calling-convention call.
    int total_args_passed = method->size_of_parameters(); // All args on stack

    BasicType* sig_bt = NEW_RESOURCE_ARRAY(BasicType, total_args_passed);
    VMRegPair* regs   = NEW_RESOURCE_ARRAY(VMRegPair, total_args_passed);
    int i = 0;
    if (!method->is_static())  // Pass in receiver first
      sig_bt[i++] = T_OBJECT;
    for (SignatureStream ss(method->signature()); !ss.at_return_type(); ss.next()) {
      sig_bt[i++] = ss.type();  // Collect remaining bits of signature
      if (ss.type() == T_LONG || ss.type() == T_DOUBLE)
        sig_bt[i++] = T_VOID;   // Longs & doubles take 2 Java slots
    }
    assert(i == total_args_passed, "");

    // Lookup method signature's fingerprint
    entry = _adapters->lookup(total_args_passed, sig_bt);

#ifdef ASSERT
    AdapterHandlerEntry* shared_entry = NULL;
    // Start adapter sharing verification only after the VM is booted.
    if (VerifyAdapterSharing && (entry != NULL)) {
      shared_entry = entry;
      entry = NULL;
    }
#endif

    if (entry != NULL) {
      return entry;
    }

    // Get a description of the compiled java calling convention and the largest used (VMReg) stack slot usage
    int comp_args_on_stack = SharedRuntime::java_calling_convention(sig_bt, regs, total_args_passed, false);

    // Make a C heap allocated version of the fingerprint to store in the adapter
    fingerprint = new AdapterFingerPrint(total_args_passed, sig_bt);

    // StubRoutines::code2() is initialized after this function can be called. As a result,
    // VerifyAdapterCalls and VerifyAdapterSharing can fail if we re-use code that generated
    // prior to StubRoutines::code2() being set. Checks refer to checks generated in an I2C
    // stub that ensure that an I2C stub is called from an interpreter frame.
    bool contains_all_checks = StubRoutines::code2() != NULL;

    // Create I2C & C2I handlers
    BufferBlob* buf = buffer_blob(); // the temporary code buffer in CodeCache
    if (buf != NULL) {
      CodeBuffer buffer(buf);
      short buffer_locs[20];
      buffer.insts()->initialize_shared_locs((relocInfo*)buffer_locs,
                                             sizeof(buffer_locs)/sizeof(relocInfo));

      MacroAssembler _masm(&buffer);
      entry = SharedRuntime::generate_i2c2i_adapters(&_masm,
                                                     total_args_passed,
                                                     comp_args_on_stack,
                                                     sig_bt,
                                                     regs,
                                                     fingerprint);
#ifdef ASSERT
      if (VerifyAdapterSharing) {
        if (shared_entry != NULL) {
          assert(shared_entry->compare_code(buf->code_begin(), buffer.insts_size()), "code must match");
          // Release the one just created and return the original
          _adapters->free_entry(entry);
          return shared_entry;
        } else  {
          entry->save_code(buf->code_begin(), buffer.insts_size());
        }
      }
#endif

      new_adapter = AdapterBlob::create(&buffer);
      NOT_PRODUCT(insts_size = buffer.insts_size());
    }
    if (new_adapter == NULL) {
      // CodeCache is full, disable compilation
      // Ought to log this but compile log is only per compile thread
      // and we're some non descript Java thread.
      return NULL; // Out of CodeCache space
    }
    entry->relocate(new_adapter->content_begin());
#ifndef PRODUCT
    // debugging suppport
    if (PrintAdapterHandlers || PrintStubCode) {
      ttyLocker ttyl;
      entry->print_adapter_on(tty);
      tty->print_cr("i2c argument handler #%d for: %s %s %s (%d bytes generated)",
                    _adapters->number_of_entries(), (method->is_static() ? "static" : "receiver"),
                    method->signature()->as_C_string(), fingerprint->as_string(), insts_size);
      tty->print_cr("c2i argument handler starts at %p", entry->get_c2i_entry());
      if (Verbose || PrintStubCode) {
        address first_pc = entry->base_address();
        if (first_pc != NULL) {
          Disassembler::decode(first_pc, first_pc + insts_size);
          tty->cr();
        }
      }
    }
#endif
    // Add the entry only if the entry contains all required checks (see sharedRuntime_xxx.cpp)
    // The checks are inserted only if -XX:+VerifyAdapterCalls is specified.
    if (contains_all_checks || !VerifyAdapterCalls) {
      _adapters->add(entry);
    }
  }
  // Outside of the lock
  if (new_adapter != NULL) {
    char blob_id[256];
    jio_snprintf(blob_id,
                 sizeof(blob_id),
                 "%s(%s)@" PTR_FORMAT,
                 new_adapter->name(),
                 fingerprint->as_string(),
                 new_adapter->content_begin());
    Forte::register_stub(blob_id, new_adapter->content_begin(), new_adapter->content_end());

    if (JvmtiExport::should_post_dynamic_code_generated()) {
      JvmtiExport::post_dynamic_code_generated(blob_id, new_adapter->content_begin(), new_adapter->content_end());
    }
  }
  return entry;
}

address AdapterHandlerEntry::base_address() {
  address base = _i2c_entry;
  if (base == NULL)  base = _c2i_entry;
  assert(base <= _c2i_entry || _c2i_entry == NULL, "");
  assert(base <= _c2i_itable_entry || _c2i_itable_entry == NULL, "");
  assert(base <= _c2i_vtable_entry || _c2i_vtable_entry == NULL, "");
  assert(base <= _c2i_no_clinit_check_entry || _c2i_no_clinit_check_entry == NULL, "");
  return base;
}

void AdapterHandlerEntry::relocate(address new_base) {
  address old_base = base_address();
  assert(old_base != NULL, "");
  ptrdiff_t delta = new_base - old_base;
  if (_i2c_entry != NULL) {
    _i2c_entry += delta;
  }
  if (_c2i_entry != NULL) {
    _c2i_entry += delta;
  }
  if (_c2i_itable_entry != NULL) {
    _c2i_itable_entry += delta;
  }
  if (_c2i_vtable_entry != NULL) {
    _c2i_vtable_entry += delta;
  }
  if (_c2i_no_clinit_check_entry != NULL) {
    _c2i_no_clinit_check_entry += delta;
  }
  assert(base_address() == new_base, "");
}


void AdapterHandlerEntry::deallocate() {
  delete _fingerprint;
#ifdef ASSERT
  FREE_C_HEAP_ARRAY(unsigned char, _saved_code);
#endif
}


#ifdef ASSERT
// Capture the code before relocation so that it can be compared
// against other versions.  If the code is captured after relocation
// then relative instructions won't be equivalent.
void AdapterHandlerEntry::save_code(unsigned char* buffer, int length) {
  _saved_code = NEW_C_HEAP_ARRAY(unsigned char, length, mtCode);
  _saved_code_length = length;
  memcpy(_saved_code, buffer, length);
}


bool AdapterHandlerEntry::compare_code(unsigned char* buffer, int length) {
  if (length != _saved_code_length) {
    return false;
  }

  return (memcmp(buffer, _saved_code, length) == 0) ? true : false;
}
#endif


/**
 * Create a native wrapper for this native method.  The wrapper converts the
 * Java-compiled calling convention to the native convention, handles
 * arguments, and transitions to native.  On return from the native we transition
 * back to java blocking if a safepoint is in progress.
 */
void AdapterHandlerLibrary::create_native_wrapper(const methodHandle& method) {
  ResourceMark rm;
  nmethod* nm = NULL;
  address critical_entry = NULL;

  assert(method->is_native(), "must be native");
  assert(method->is_method_handle_intrinsic() ||
         method->has_native_function(), "must have something valid to call!");

  if (CriticalJNINatives && !method->is_method_handle_intrinsic()) {
    // We perform the I/O with transition to native before acquiring AdapterHandlerLibrary_lock.
    critical_entry = NativeLookup::lookup_critical_entry(method);
  }

  {
    // Perform the work while holding the lock, but perform any printing outside the lock
    MutexLocker mu(AdapterHandlerLibrary_lock);
    // See if somebody beat us to it
    if (method->code() != NULL) {
      return;
    }

    const int compile_id = CompileBroker::assign_compile_id(method, CompileBroker::standard_entry_bci);
    assert(compile_id > 0, "Must generate native wrapper");


    ResourceMark rm;
    BufferBlob*  buf = buffer_blob(); // the temporary code buffer in CodeCache
    if (buf != NULL) {
      CodeBuffer buffer(buf);
      double locs_buf[20];
      buffer.insts()->initialize_shared_locs((relocInfo*)locs_buf, sizeof(locs_buf) / sizeof(relocInfo));
#if defined(AARCH64)
      // On AArch64 with ZGC and nmethod entry barriers, we need all oops to be
      // in the constant pool to ensure ordering between the barrier and oops
      // accesses. For native_wrappers we need a constant.
      buffer.initialize_consts_size(8);
#endif
      MacroAssembler _masm(&buffer);

      // Fill in the signature array, for the calling-convention call.
      const int total_args_passed = method->size_of_parameters();

      BasicType* sig_bt = NEW_RESOURCE_ARRAY(BasicType, total_args_passed);
      VMRegPair*   regs = NEW_RESOURCE_ARRAY(VMRegPair, total_args_passed);
      int i=0;
      if (!method->is_static())  // Pass in receiver first
        sig_bt[i++] = T_OBJECT;
      SignatureStream ss(method->signature());
      for (; !ss.at_return_type(); ss.next()) {
        sig_bt[i++] = ss.type();  // Collect remaining bits of signature
        if (ss.type() == T_LONG || ss.type() == T_DOUBLE)
          sig_bt[i++] = T_VOID;   // Longs & doubles take 2 Java slots
      }
      assert(i == total_args_passed, "");
      BasicType ret_type = ss.type();

      // Now get the compiled-Java layout as input (or output) arguments.
      // NOTE: Stubs for compiled entry points of method handle intrinsics
      // are just trampolines so the argument registers must be outgoing ones.
      const bool is_outgoing = method->is_method_handle_intrinsic();
      int comp_args_on_stack = SharedRuntime::java_calling_convention(sig_bt, regs, total_args_passed, is_outgoing);

      // Generate the compiled-to-native wrapper code
      nm = SharedRuntime::generate_native_wrapper(&_masm, method, compile_id, sig_bt, regs, ret_type, critical_entry);

      if (nm != NULL) {
        {
          MutexLocker pl(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
          if (nm->is_in_use()) {
            method->set_code(method, nm);
          }
        }

        DirectiveSet* directive = DirectivesStack::getDefaultDirective(CompileBroker::compiler(CompLevel_simple));
        if (directive->PrintAssemblyOption) {
          nm->print_code();
        }
        DirectivesStack::release(directive);
      }
    }
  } // Unlock AdapterHandlerLibrary_lock


  // Install the generated code.
  if (nm != NULL) {
    const char *msg = method->is_static() ? "(static)" : "";
    CompileTask::print_ul(nm, msg);
    if (PrintCompilation) {
      ttyLocker ttyl;
      CompileTask::print(tty, nm, msg);
    }
    nm->post_compiled_method_load_event();
  }
}

JRT_ENTRY_NO_ASYNC(void, SharedRuntime::block_for_jni_critical(JavaThread* thread))
  assert(thread == JavaThread::current(), "must be");
  // The code is about to enter a JNI lazy critical native method and
  // _needs_gc is true, so if this thread is already in a critical
  // section then just return, otherwise this thread should block
  // until needs_gc has been cleared.
  if (thread->in_critical()) {
    return;
  }
  // Lock and unlock a critical section to give the system a chance to block
  GCLocker::lock_critical(thread);
  GCLocker::unlock_critical(thread);
JRT_END

JRT_LEAF(oopDesc*, SharedRuntime::pin_object(JavaThread* thread, oopDesc* obj))
  assert(Universe::heap()->supports_object_pinning(), "Why we are here?");
  assert(obj != NULL, "Should not be null");
  oop o(obj);
  o = Universe::heap()->pin_object(thread, o);
  assert(o != NULL, "Should not be null");
  return o;
JRT_END

JRT_LEAF(void, SharedRuntime::unpin_object(JavaThread* thread, oopDesc* obj))
  assert(Universe::heap()->supports_object_pinning(), "Why we are here?");
  assert(obj != NULL, "Should not be null");
  oop o(obj);
  Universe::heap()->unpin_object(thread, o);
JRT_END

// -------------------------------------------------------------------------
// Java-Java calling convention
// (what you use when Java calls Java)

//------------------------------name_for_receiver----------------------------------
// For a given signature, return the VMReg for parameter 0.
VMReg SharedRuntime::name_for_receiver() {
  VMRegPair regs;
  BasicType sig_bt = T_OBJECT;
  (void) java_calling_convention(&sig_bt, &regs, 1, true);
  // Return argument 0 register.  In the LP64 build pointers
  // take 2 registers, but the VM wants only the 'main' name.
  return regs.first();
}

VMRegPair *SharedRuntime::find_callee_arguments(Symbol* sig, bool has_receiver, bool has_appendix, int* arg_size) {
  // This method is returning a data structure allocating as a
  // ResourceObject, so do not put any ResourceMarks in here.

  BasicType *sig_bt = NEW_RESOURCE_ARRAY(BasicType, 256);
  VMRegPair *regs = NEW_RESOURCE_ARRAY(VMRegPair, 256);
  int cnt = 0;
  if (has_receiver) {
    sig_bt[cnt++] = T_OBJECT; // Receiver is argument 0; not in signature
  }

  for (SignatureStream ss(sig); !ss.at_return_type(); ss.next()) {
    BasicType type = ss.type();
    sig_bt[cnt++] = type;
    if (is_double_word_type(type))
      sig_bt[cnt++] = T_VOID;
  }

  if (has_appendix) {
    sig_bt[cnt++] = T_OBJECT;
  }

  assert(cnt < 256, "grow table size");

  int comp_args_on_stack;
  comp_args_on_stack = java_calling_convention(sig_bt, regs, cnt, true);

  // the calling convention doesn't count out_preserve_stack_slots so
  // we must add that in to get "true" stack offsets.

  if (comp_args_on_stack) {
    for (int i = 0; i < cnt; i++) {
      VMReg reg1 = regs[i].first();
      if (reg1->is_stack()) {
        // Yuck
        reg1 = reg1->bias(out_preserve_stack_slots());
      }
      VMReg reg2 = regs[i].second();
      if (reg2->is_stack()) {
        // Yuck
        reg2 = reg2->bias(out_preserve_stack_slots());
      }
      regs[i].set_pair(reg2, reg1);
    }
  }

  // results
  *arg_size = cnt;
  return regs;
}

// OSR Migration Code
//
// This code is used convert interpreter frames into compiled frames.  It is
// called from very start of a compiled OSR nmethod.  A temp array is
// allocated to hold the interesting bits of the interpreter frame.  All
// active locks are inflated to allow them to move.  The displaced headers and
// active interpreter locals are copied into the temp buffer.  Then we return
// back to the compiled code.  The compiled code then pops the current
// interpreter frame off the stack and pushes a new compiled frame.  Then it
// copies the interpreter locals and displaced headers where it wants.
// Finally it calls back to free the temp buffer.
//
// All of this is done NOT at any Safepoint, nor is any safepoint or GC allowed.

JRT_LEAF(intptr_t*, SharedRuntime::OSR_migration_begin( JavaThread *thread) )

  //
  // This code is dependent on the memory layout of the interpreter local
  // array and the monitors. On all of our platforms the layout is identical
  // so this code is shared. If some platform lays the their arrays out
  // differently then this code could move to platform specific code or
  // the code here could be modified to copy items one at a time using
  // frame accessor methods and be platform independent.

  frame fr = thread->last_frame();
  assert(fr.is_interpreted_frame(), "");
  assert(fr.interpreter_frame_expression_stack_size()==0, "only handle empty stacks");

  // Figure out how many monitors are active.
  int active_monitor_count = 0;
  for (BasicObjectLock *kptr = fr.interpreter_frame_monitor_end();
       kptr < fr.interpreter_frame_monitor_begin();
       kptr = fr.next_monitor_in_interpreter_frame(kptr) ) {
    if (kptr->obj() != NULL) active_monitor_count++;
  }

  // QQQ we could place number of active monitors in the array so that compiled code
  // could double check it.

  Method* moop = fr.interpreter_frame_method();
  int max_locals = moop->max_locals();
  // Allocate temp buffer, 1 word per local & 2 per active monitor
  int buf_size_words = max_locals + active_monitor_count * BasicObjectLock::size();
  intptr_t *buf = NEW_C_HEAP_ARRAY(intptr_t,buf_size_words, mtCode);

  // Copy the locals.  Order is preserved so that loading of longs works.
  // Since there's no GC I can copy the oops blindly.
  assert(sizeof(HeapWord)==sizeof(intptr_t), "fix this code");
  Copy::disjoint_words((HeapWord*)fr.interpreter_frame_local_at(max_locals-1),
                       (HeapWord*)&buf[0],
                       max_locals);

  // Inflate locks.  Copy the displaced headers.  Be careful, there can be holes.
  int i = max_locals;
  for (BasicObjectLock *kptr2 = fr.interpreter_frame_monitor_end();
       kptr2 < fr.interpreter_frame_monitor_begin();
       kptr2 = fr.next_monitor_in_interpreter_frame(kptr2) ) {
    if (kptr2->obj() != NULL) {         // Avoid 'holes' in the monitor array
      BasicLock *lock = kptr2->lock();
      // Inflate so the object's header no longer refers to the BasicLock.
      if (lock->displaced_header().is_unlocked()) {
        // The object is locked and the resulting ObjectMonitor* will also be
        // locked so it can't be async deflated until ownership is dropped.
        // See the big comment in basicLock.cpp: BasicLock::move_to().
        ObjectSynchronizer::inflate_helper(kptr2->obj());
      }
      // Now the displaced header is free to move because the
      // object's header no longer refers to it.
      buf[i++] = (intptr_t)lock->displaced_header().value();
      buf[i++] = cast_from_oop<intptr_t>(kptr2->obj());
    }
  }
  assert(i - max_locals == active_monitor_count*2, "found the expected number of monitors");

  return buf;
JRT_END

JRT_LEAF(void, SharedRuntime::OSR_migration_end( intptr_t* buf) )
  FREE_C_HEAP_ARRAY(intptr_t, buf);
JRT_END

bool AdapterHandlerLibrary::contains(const CodeBlob* b) {
  AdapterHandlerTableIterator iter(_adapters);
  while (iter.has_next()) {
    AdapterHandlerEntry* a = iter.next();
    if (b == CodeCache::find_blob(a->get_i2c_entry())) return true;
  }
  return false;
}

void AdapterHandlerLibrary::print_handler_on(outputStream* st, const CodeBlob* b) {
  AdapterHandlerTableIterator iter(_adapters);
  while (iter.has_next()) {
    AdapterHandlerEntry* a = iter.next();
    if (b == CodeCache::find_blob(a->get_i2c_entry())) {
      st->print("Adapter for signature: ");
      a->print_adapter_on(tty);
      return;
    }
  }
  assert(false, "Should have found handler");
}

void AdapterHandlerEntry::print_adapter_on(outputStream* st) const {
  st->print("AHE@" INTPTR_FORMAT ": %s", p2i(this), fingerprint()->as_string());
  if (get_i2c_entry() != NULL) {
    st->print(" i2c: " INTPTR_FORMAT, p2i(get_i2c_entry()));
  }
  if (get_c2i_entry() != NULL) {
    st->print(" c2i: " INTPTR_FORMAT, p2i(get_c2i_entry()));
  }
  if (get_c2i_itable_entry() != NULL) {
    st->print(" c2i_itable: " INTPTR_FORMAT, p2i(get_c2i_itable_entry()));
  }
  if (get_c2i_vtable_entry() != NULL) {
    st->print(" c2i_vtable: " INTPTR_FORMAT, p2i(get_c2i_vtable_entry()));
  }
  if (get_c2i_no_clinit_check_entry() != NULL) {
    st->print(" c2iNCI: " INTPTR_FORMAT, p2i(get_c2i_no_clinit_check_entry()));
  }
  st->cr();
}

#if INCLUDE_CDS

void CDSAdapterHandlerEntry::init() {
  assert(DumpSharedSpaces, "used during dump time only");
  _c2i_entry_trampoline = (address)MetaspaceShared::misc_code_space_alloc(SharedRuntime::trampoline_size());
  _adapter_trampoline = (AdapterHandlerEntry**)MetaspaceShared::misc_code_space_alloc(sizeof(AdapterHandlerEntry*));
};

#endif // INCLUDE_CDS


#ifndef PRODUCT

void AdapterHandlerLibrary::print_statistics() {
  _adapters->print_statistics();
}

#endif /* PRODUCT */

JRT_LEAF(void, SharedRuntime::enable_stack_reserved_zone(JavaThread* thread))
  assert(thread->is_Java_thread(), "Only Java threads have a stack reserved zone");
  if (thread->stack_reserved_zone_disabled()) {
  thread->enable_stack_reserved_zone();
  }
  thread->set_reserved_stack_activation(thread->stack_base());
JRT_END

frame SharedRuntime::look_for_reserved_stack_annotated_method(JavaThread* thread, frame fr) {
  ResourceMark rm(thread);
  frame activation;
  CompiledMethod* nm = NULL;
  int count = 1;

  assert(fr.is_java_frame(), "Must start on Java frame");

  while (true) {
    Method* method = NULL;
    bool found = false;
    if (fr.is_interpreted_frame()) {
      method = fr.interpreter_frame_method();
      if (method != NULL && method->has_reserved_stack_access()) {
        found = true;
      }
    } else {
      CodeBlob* cb = fr.cb();
      if (cb != NULL && cb->is_compiled()) {
        nm = cb->as_compiled_method();
        method = nm->method();
        // scope_desc_near() must be used, instead of scope_desc_at() because on
        // SPARC, the pcDesc can be on the delay slot after the call instruction.
        for (ScopeDesc *sd = nm->scope_desc_near(fr.pc()); sd != NULL; sd = sd->sender()) {
          method = sd->method();
          if (method != NULL && method->has_reserved_stack_access()) {
            found = true;
      }
    }
      }
    }
    if (found) {
      activation = fr;
      warning("Potentially dangerous stack overflow in "
              "ReservedStackAccess annotated method %s [%d]",
              method->name_and_sig_as_C_string(), count++);
      EventReservedStackActivation event;
      if (event.should_commit()) {
        event.set_method(method);
        event.commit();
      }
    }
    if (fr.is_first_java_frame()) {
      break;
    } else {
      fr = fr.java_sender();
    }
  }
  return activation;
}

void SharedRuntime::on_slowpath_allocation_exit(JavaThread* thread) {
  // After any safepoint, just before going back to compiled code,
  // we inform the GC that we will be doing initializing writes to
  // this object in the future without emitting card-marks, so
  // GC may take any compensating steps.

  oop new_obj = thread->vm_result();
  if (new_obj == NULL) return;

  BarrierSet *bs = BarrierSet::barrier_set();
  bs->on_slowpath_allocation_exit(thread, new_obj);
}

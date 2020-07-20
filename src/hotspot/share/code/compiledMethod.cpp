/*
 * Copyright (c) 2015, 2019, Oracle and/or its affiliates. All rights reserved.
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
#include "code/compiledMethod.inline.hpp"
#include "code/exceptionHandlerTable.hpp"
#include "code/scopeDesc.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "gc/shared/gcBehaviours.hpp"
#include "interpreter/bytecode.inline.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "memory/resourceArea.hpp"
#include "oops/methodData.hpp"
#include "oops/method.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/atomic.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/sharedRuntime.hpp"

CompiledMethod::CompiledMethod(Method* method, const char* name, CompilerType type, const CodeBlobLayout& layout,
                               int frame_complete_offset, int frame_size, ImmutableOopMapSet* oop_maps,
                               bool caller_must_gc_arguments, LazyInvocation* lazy_invocations)
  : CodeBlob(name, type, layout, frame_complete_offset, frame_size, oop_maps, caller_must_gc_arguments),
    _mark_for_deoptimization_status(not_marked),
    _method(method),
    _lazy_invocations(lazy_invocations),
    _purge_list_next(NULL),
    _gc_data(NULL)
{
  init_defaults();
}

CompiledMethod::CompiledMethod(Method* method, const char* name, CompilerType type, int size,
                               int header_size, CodeBuffer* cb, int frame_complete_offset, int frame_size,
                               OopMapSet* oop_maps, bool caller_must_gc_arguments, LazyInvocation* lazy_invocations)
  : CodeBlob(name, type, CodeBlobLayout((address) this, size, header_size, cb), cb,
             frame_complete_offset, frame_size, oop_maps, caller_must_gc_arguments),
    _mark_for_deoptimization_status(not_marked),
    _method(method),
    _lazy_invocations(lazy_invocations),
    _purge_list_next(NULL),
    _gc_data(NULL)
{
  init_defaults();
}

void CompiledMethod::init_defaults() {
  _has_unsafe_access          = 0;
  _has_method_handle_invokes  = 0;
  _lazy_critical_native       = 0;
  _has_wide_vectors           = 0;
}

bool CompiledMethod::is_method_handle_return(address return_pc) {
  if (!has_method_handle_invokes())  return false;
  PcDesc* pd = pc_desc_at(return_pc);
  if (pd == NULL)
    return false;
  return pd->is_method_handle_invoke();
}

// Returns a string version of the method state.
const char* CompiledMethod::state() const {
  int state = get_state();
  switch (state) {
  case in_use:
    return "in use";
  case not_used:
    return "not_used";
  case not_entrant:
    return "not_entrant";
  default:
    fatal("unexpected method state: %d", state);
    return NULL;
  }
}

//-----------------------------------------------------------------------------
void CompiledMethod::mark_for_deoptimization(bool inc_recompile_counts) {
  MutexLocker ml(CompiledMethod_lock->owned_by_self() ? NULL : CompiledMethod_lock,
                 Mutex::_no_safepoint_check_flag);
  _mark_for_deoptimization_status = (inc_recompile_counts ? deoptimize : deoptimize_noupdate);
}

//-----------------------------------------------------------------------------

ExceptionCache* CompiledMethod::exception_cache_acquire() const {
  return Atomic::load_acquire(&_exception_cache);
}

void CompiledMethod::add_exception_cache_entry(ExceptionCache* new_entry) {
  assert(ExceptionCache_lock->owned_by_self(),"Must hold the ExceptionCache_lock");
  assert(new_entry != NULL,"Must be non null");
  assert(new_entry->next() == NULL, "Must be null");

  for (;;) {
    ExceptionCache *ec = exception_cache();
    if (ec != NULL) {
      Klass* ex_klass = ec->exception_type();
      if (!ex_klass->is_loader_alive()) {
        // We must guarantee that entries are not inserted with new next pointer
        // edges to ExceptionCache entries with dead klasses, due to bad interactions
        // with concurrent ExceptionCache cleanup. Therefore, the inserts roll
        // the head pointer forward to the first live ExceptionCache, so that the new
        // next pointers always point at live ExceptionCaches, that are not removed due
        // to concurrent ExceptionCache cleanup.
        ExceptionCache* next = ec->next();
        if (Atomic::cmpxchg(&_exception_cache, ec, next) == ec) {
          CodeCache::release_exception_cache(ec);
        }
        continue;
      }
      ec = exception_cache();
      if (ec != NULL) {
        new_entry->set_next(ec);
      }
    }
    if (Atomic::cmpxchg(&_exception_cache, ec, new_entry) == ec) {
      return;
    }
  }
}

void CompiledMethod::clean_exception_cache() {
  // For each nmethod, only a single thread may call this cleanup function
  // at the same time, whether called in STW cleanup or concurrent cleanup.
  // Note that if the GC is processing exception cache cleaning in a concurrent phase,
  // then a single writer may contend with cleaning up the head pointer to the
  // first ExceptionCache node that has a Klass* that is alive. That is fine,
  // as long as there is no concurrent cleanup of next pointers from concurrent writers.
  // And the concurrent writers do not clean up next pointers, only the head.
  // Also note that concurent readers will walk through Klass* pointers that are not
  // alive. That does not cause ABA problems, because Klass* is deleted after
  // a handshake with all threads, after all stale ExceptionCaches have been
  // unlinked. That is also when the CodeCache::exception_cache_purge_list()
  // is deleted, with all ExceptionCache entries that were cleaned concurrently.
  // That similarly implies that CAS operations on ExceptionCache entries do not
  // suffer from ABA problems as unlinking and deletion is separated by a global
  // handshake operation.
  ExceptionCache* prev = NULL;
  ExceptionCache* curr = exception_cache_acquire();

  while (curr != NULL) {
    ExceptionCache* next = curr->next();

    if (!curr->exception_type()->is_loader_alive()) {
      if (prev == NULL) {
        // Try to clean head; this is contended by concurrent inserts, that
        // both lazily clean the head, and insert entries at the head. If
        // the CAS fails, the operation is restarted.
        if (Atomic::cmpxchg(&_exception_cache, curr, next) != curr) {
          prev = NULL;
          curr = exception_cache_acquire();
          continue;
        }
      } else {
        // It is impossible to during cleanup connect the next pointer to
        // an ExceptionCache that has not been published before a safepoint
        // prior to the cleanup. Therefore, release is not required.
        prev->set_next(next);
      }
      // prev stays the same.

      CodeCache::release_exception_cache(curr);
    } else {
      prev = curr;
    }

    curr = next;
  }
}

// public method for accessing the exception cache
// These are the public access methods.
address CompiledMethod::handler_for_exception_and_pc(Handle exception, address pc) {
  // We never grab a lock to read the exception cache, so we may
  // have false negatives. This is okay, as it can only happen during
  // the first few exception lookups for a given nmethod.
  ExceptionCache* ec = exception_cache_acquire();
  while (ec != NULL) {
    address ret_val;
    if ((ret_val = ec->match(exception,pc)) != NULL) {
      return ret_val;
    }
    ec = ec->next();
  }
  return NULL;
}

void CompiledMethod::add_handler_for_exception_and_pc(Handle exception, address pc, address handler) {
  // There are potential race conditions during exception cache updates, so we
  // must own the ExceptionCache_lock before doing ANY modifications. Because
  // we don't lock during reads, it is possible to have several threads attempt
  // to update the cache with the same data. We need to check for already inserted
  // copies of the current data before adding it.

  MutexLocker ml(ExceptionCache_lock);
  ExceptionCache* target_entry = exception_cache_entry_for_exception(exception);

  if (target_entry == NULL || !target_entry->add_address_and_handler(pc,handler)) {
    target_entry = new ExceptionCache(exception,pc,handler);
    add_exception_cache_entry(target_entry);
  }
}

// private method for handling exception cache
// These methods are private, and used to manipulate the exception cache
// directly.
ExceptionCache* CompiledMethod::exception_cache_entry_for_exception(Handle exception) {
  ExceptionCache* ec = exception_cache_acquire();
  while (ec != NULL) {
    if (ec->match_exception_with_space(exception)) {
      return ec;
    }
    ec = ec->next();
  }
  return NULL;
}

//-------------end of code for ExceptionCache--------------

bool CompiledMethod::is_at_poll_return(address pc) {
  RelocIterator iter(this, pc, pc+1);
  while (iter.next()) {
    if (iter.type() == relocInfo::poll_return_type)
      return true;
  }
  return false;
}


bool CompiledMethod::is_at_poll_or_poll_return(address pc) {
  RelocIterator iter(this, pc, pc+1);
  while (iter.next()) {
    relocInfo::relocType t = iter.type();
    if (t == relocInfo::poll_return_type || t == relocInfo::poll_type)
      return true;
  }
  return false;
}

void CompiledMethod::verify_oop_relocations() {
  // Ensure sure that the code matches the current oop values
  RelocIterator iter(this, NULL, NULL);
  while (iter.next()) {
    if (iter.type() == relocInfo::oop_type) {
      oop_Relocation* reloc = iter.oop_reloc();
      if (!reloc->oop_is_immediate()) {
        reloc->verify_oop_relocation();
      }
    }
  }
}

LazyInvocation* CompiledMethod::lazy_invocation_at(address pc) {
  address base_address = code_begin();
  intptr_t offset = (intptr_t)pc - (intptr_t)base_address;
  for (LazyInvocation* current = _lazy_invocations;
       current != NULL;
       current = current->next()) {
    if (current->pc_offset() == offset) {
      return current;
    }
  }
  return NULL;
}

ScopeDesc* CompiledMethod::scope_desc_at(address pc) {
  PcDesc* pd = pc_desc_at(pc);
  guarantee(pd != NULL, "scope must be present");
  return new ScopeDesc(this, pd->scope_decode_offset(),
                       pd->obj_decode_offset(), pd->should_reexecute(), pd->rethrow_exception(),
                       pd->return_oop());
}

ScopeDesc* CompiledMethod::scope_desc_near(address pc) {
  PcDesc* pd = pc_desc_near(pc);
  guarantee(pd != NULL, "scope must be present");
  return new ScopeDesc(this, pd->scope_decode_offset(),
                       pd->obj_decode_offset(), pd->should_reexecute(), pd->rethrow_exception(),
                       pd->return_oop());
}

address CompiledMethod::oops_reloc_begin() const {
  return entry_point();
}

// Method that knows how to preserve outgoing arguments at call. This method must be
// called with a frame corresponding to a Java invoke
void CompiledMethod::preserve_callee_argument_oops(frame fr, const RegisterMap *reg_map, OopClosure* f) {
  if (method() != NULL && !method()->is_native()) {
    address pc = fr.pc();
    SimpleScopeDesc ssd(this, pc);
    Bytecode_invoke call(methodHandle(Thread::current(), ssd.method()), ssd.bci());
    bool has_receiver = call.has_receiver();
    bool has_appendix = call.has_appendix();
    Symbol* signature = call.signature();

    // The method attached by JIT-compilers should be used, if present.
    // Bytecode can be inaccurate in such case.
    Method* callee = attached_method_at(pc);
    if (callee != NULL) {
      has_receiver = !(callee->access_flags().is_static());
      has_appendix = false;
      signature = callee->signature();
    }

    fr.oops_compiled_arguments_do(signature, has_receiver, has_appendix, reg_map, f);
  }
}

Method* CompiledMethod::attached_method_at(address call_instr) {
  assert(code_contains(call_instr), "not part of the nmethod");
  LazyInvocation* lazy = lazy_invocation_at(call_instr);
  return lazy == NULL ? NULL : lazy->attached_method();
}

#ifdef ASSERT
// Check class_loader is alive for this bit of metadata.
class CheckClass : public MetadataClosure {
  void do_metadata(Metadata* md) {
    Klass* klass = NULL;
    if (md->is_klass()) {
      klass = ((Klass*)md);
    } else if (md->is_method()) {
      klass = ((Method*)md)->method_holder();
    } else if (md->is_methodData()) {
      klass = ((MethodData*)md)->method()->method_holder();
    } else {
      md->print();
      ShouldNotReachHere();
    }
    assert(klass->is_loader_alive(), "must be alive");
  }
};
#endif // ASSERT


// Cleans caches in nmethods that point to either classes that are unloaded
// or nmethods that are unloaded.
//
// Can be called either in parallel by G1 currently or after all
// nmethods are unloaded.  Return postponed=true in the parallel case for
// inline caches found that point to nmethods that are not yet visited during
// the do_unloading walk.
void CompiledMethod::unload_nmethod_caches(bool unloading_occurred) {
  ResourceMark rm;

  // Exception cache only needs to be called if unloading occurred
  if (unloading_occurred) {
    clean_exception_cache();
  }

#ifdef ASSERT
  // Check that the metadata embedded in the nmethod is alive
  CheckClass check_class;
  metadata_do(&check_class);
#endif
}

void CompiledMethod::run_nmethod_entry_barrier() {
  BarrierSetNMethod* bs_nm = BarrierSet::barrier_set()->barrier_set_nmethod();
  if (bs_nm != NULL) {
    // We want to keep an invariant that nmethods found through iterations of a Thread's
    // nmethods found in safepoints have gone through an entry barrier and are not armed.
    // By calling this nmethod entry barrier, it plays along and acts
    // like any other nmethod found on the stack of a thread (fewer surprises).
    nmethod* nm = as_nmethod_or_null();
    if (nm != NULL) {
      bs_nm->nmethod_entry_barrier(nm);
    }
  }
}

address CompiledMethod::continuation_for_implicit_exception(address pc, bool for_div0_check) {
  // Exception happened outside inline-cache check code => we are inside
  // an active nmethod => use cpc to determine a return address
  int exception_offset = pc - code_begin();
  int cont_offset = ImplicitExceptionTable(this).continuation_offset( exception_offset );
#ifdef ASSERT
  if (cont_offset == 0) {
    Thread* thread = Thread::current();
    ResetNoHandleMark rnm; // Might be called from LEAF/QUICK ENTRY
    HandleMark hm(thread);
    ResourceMark rm(thread);
    CodeBlob* cb = CodeCache::find_blob(pc);
    assert(cb != NULL && cb == this, "");
    ttyLocker ttyl;
    tty->print_cr("implicit exception happened at " INTPTR_FORMAT, p2i(pc));
    print();
    method()->print_codes();
    print_code();
    print_pcs();
  }
#endif
  if (cont_offset == 0) {
    // Let the normal error handling report the exception
    return NULL;
  }
  if (cont_offset == exception_offset) {
#if INCLUDE_JVMCI
    Deoptimization::DeoptReason deopt_reason = for_div0_check ? Deoptimization::Reason_div0_check : Deoptimization::Reason_null_check;
    JavaThread *thread = JavaThread::current();
    thread->set_jvmci_implicit_exception_pc(pc);
    thread->set_pending_deoptimization(Deoptimization::make_trap_request(deopt_reason,
                                                                         Deoptimization::Action_reinterpret));
    return (SharedRuntime::deopt_blob()->implicit_exception_uncommon_trap());
#else
    ShouldNotReachHere();
#endif
  }
  return code_begin() + cont_offset;
}

class HasEvolDependency : public MetadataClosure {
  bool _has_evol_dependency;
 public:
  HasEvolDependency() : _has_evol_dependency(false) {}
  void do_metadata(Metadata* md) {
    if (md->is_method()) {
      Method* method = (Method*)md;
      if (method->is_old()) {
        _has_evol_dependency = true;
      }
    }
  }
  bool has_evol_dependency() const { return _has_evol_dependency; }
};

bool CompiledMethod::has_evol_metadata() {
  // Check the metadata in relocIter and CompiledIC and also deoptimize
  // any nmethod that has reference to old methods.
  HasEvolDependency check_evol;
  metadata_do(&check_evol);
  if (check_evol.has_evol_dependency() && log_is_enabled(Debug, redefine, class, nmethod)) {
    ResourceMark rm;
    log_debug(redefine, class, nmethod)
            ("Found evol dependency of nmethod %s.%s(%s) compile_id=%d on in nmethod metadata",
             _method->method_holder()->external_name(),
             _method->name()->as_C_string(),
             _method->signature()->as_C_string(),
             compile_id());
  }
  return check_evol.has_evol_dependency();
}

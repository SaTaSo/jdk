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
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/nmethod.hpp"
#include "code/codeCache.hpp"
#include "interpreter/linkResolver.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klassVtable.hpp"
#include "oops/method.inline.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/selectorMap.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/copy.hpp"

static inline tableEntry make_entry(uint32_t selector, address code_addr) {
  tableEntry entry;
  uintptr_t code_intptr = reinterpret_cast<uintptr_t>(code_addr);
  if (code_addr != NULL) {
    if (CodeCache::supports_32_bit_code_pointers()) {
      code_intptr <<= (32 - CodeCache::code_pointer_shift());
    } else {
      uintptr_t code_base = (uintptr_t)CodeCache::low_bound();
      code_intptr = (code_intptr - code_base) << 32;
    }
  }
  entry._entry = code_intptr | selector;
  assert(selector == 0 || entry.method() != NULL, "sanity");
  return entry;
}

address tableEntry::table_entry_code(Method* method, bool is_itable) {
  assert_lock_strong(CompiledMethod_lock);
  CompiledMethod* cm = method->code();
  if (method->is_overpass() || cm == NULL || cm->is_unloading()) {
    if (method->adapter() == NULL) {
      // during bootstrapping; adapter not yet initialized
      return SharedRuntime::get_bad_call_stub();
    }
    // interpreter calls
    if (is_itable) {
      return method->get_c2i_itable_entry();
    } else {
      return method->get_c2i_vtable_entry();
    }
  } else {
    // to compiled calls
    return cm->entry_point();
  }
}

static inline tableEntry make_table_entry(uint32_t selector, Method* method, bool is_itable) {
  if (method == NULL) {
    return make_entry(0, SharedRuntime::get_bad_call_stub());
  }
  return make_entry(selector, tableEntry::table_entry_code(method, is_itable));
}

static inline tableEntry make_vtable_entry(Method* method) {
  return make_table_entry(method == NULL ? 0 : method->selector(), method, false /* is_itable */);
}

static inline tableEntry make_itable_entry(uint32_t selector, Method* method) {
  return make_table_entry(selector, method, true /* is_itable */);
}

void klassVtable::link_table_code() {
  MutexLocker ml(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
  SelectorMap<Method*> method_selector_map = SystemDictionary::method_selector_map();
  for (int vtable_index = 0; vtable_index < length(); ++vtable_index) {
    tableEntry vtable_entry = _table[-vtable_index];
    Method* method = method_selector_map.get(vtable_entry.selector());
    _table[-vtable_index] = make_vtable_entry(method);
  }
}

void klassVtable::link_code(int vtable_index, Method* method) {
  assert_lock_strong(CompiledMethod_lock);
  assert(method_at(vtable_index) == method, "methods must match");
  assert(vtable_index >= 0 && vtable_index < length(), "out of bounds");
  _table[-vtable_index] = make_vtable_entry(method);
}

Method** klassVtable::scratch_table() {
  if (_scratch_table == NULL) {
    // TODO adjust address for 32 bit.
    _scratch_table = reinterpret_cast<Method**>(_table);
  }
  return _scratch_table;
}

void klassVtable::link_code(bool bootstrapping) {
  MutexLocker ml(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
  Method** st = scratch_table();
  for (int32_t vtable_index = 0; vtable_index < (int32_t)length(); ++vtable_index) {
    Method* vtable_method = st[-vtable_index];
    if (vtable_method == NULL) {
      _table[-vtable_index] = make_entry(0, SharedRuntime::get_bad_call_stub());
    } else {
      _table[-vtable_index] = make_vtable_entry(vtable_method);
    }
  }
}

int klassVtable::length() const {
  return _length;
}

tableEntry klassVtable::entry_at(int i) const {
  assert(i >= 0 && i < (int)length(), "index out of bounds");
  return _table[-i];
}

Method* klassVtable::unchecked_method_at(int i) const {
  return entry_at(i).method();
}

Method* klassVtable::method_at(int i) const {
  Method* method = unchecked_method_at(i);
  assert(method != NULL, "should not be null");
  assert((Metadata*)method->is_method(), "should be method");
  return method;
}

inline InstanceKlass* klassVtable::ik() const {
  return InstanceKlass::cast(_klass);
}

bool klassVtable::is_preinitialized_vtable() {
  return false;
  // TODO: Should support preinitialized vtables
  //return _klass->is_shared() && !MetaspaceShared::remapped_readwrite();
}


// this function computes the vtable size (including the size needed for miranda
// methods) and the number of miranda methods in this class.
// Note on Miranda methods: Let's say there is a class C that implements
// interface I, and none of C's superclasses implements I.
// Let's say there is an abstract method m in I that neither C
// nor any of its super classes implement (i.e there is no method of any access,
// with the same name and signature as m), then m is a Miranda method which is
// entered as a public abstract method in C's vtable.  From then on it should
// treated as any other public method in C for method over-ride purposes.
void klassVtable::compute_vtable_size_and_num_mirandas(
    int* vtable_length_ret, int* num_new_mirandas,
    GrowableArray<Method*>* all_mirandas, const Klass* super,
    Array<Method*>* methods, AccessFlags class_flags, u2 major_version,
    Handle classloader, Symbol* classname, Array<InstanceKlass*>* local_interfaces,
    TRAPS) {
  NoSafepointVerifier nsv;

  // set up default result values
  int vtable_length = 0;

  // start off with super's vtable length
  vtable_length = super == NULL ? 1 : super->vtable_length();

  // go thru each method in the methods table to see if it needs a new entry
  int len = methods->length();
  for (int i = 0; i < len; i++) {
    assert(methods->at(i)->is_method(), "must be a Method*");
    methodHandle mh(THREAD, methods->at(i));

    if (needs_new_vtable_entry(mh, super, classloader, classname, class_flags, major_version, THREAD)) {
      assert(!methods->at(i)->is_private(), "private methods should not need a vtable entry");
      vtable_length++; // we need a new entry
    }
  }

  GrowableArray<Method*> new_mirandas(20);
  // compute the number of mirandas methods that must be added to the end
  get_mirandas(&new_mirandas, all_mirandas, super, methods, NULL, local_interfaces,
               class_flags.is_interface());
  *num_new_mirandas = new_mirandas.length();

  // Interfaces do not need interface methods in their vtables
  // This includes miranda methods and during later processing, default methods
  if (!class_flags.is_interface()) {
     vtable_length += *num_new_mirandas;
  }

  if (Universe::is_bootstrapping() && vtable_length == 1) {
    // array classes don't have their superclass set correctly during
    // bootstrapping
    vtable_length = Universe::base_vtable_size();
  }

  if (super == NULL && vtable_length != Universe::base_vtable_size()) {
    if (Universe::is_bootstrapping()) {
      // Someone is attempting to override java.lang.Object incorrectly on the
      // bootclasspath.  The JVM cannot recover from this error including throwing
      // an exception
      vm_exit_during_initialization("Incompatible definition of java.lang.Object");
    } else {
      // Someone is attempting to redefine java.lang.Object incorrectly.  The
      // only way this should happen is from
      // SystemDictionary::resolve_from_stream(), which will detect this later
      // and throw a security exception.  So don't assert here to let
      // the exception occur.
      vtable_length = Universe::base_vtable_size();
    }
  }
  assert(vtable_length >= Universe::base_vtable_size(), "vtable too small");

  *vtable_length_ret = (int)blob_size_words(vtable_length);
}

size_t klassVtable::blob_size_words(uint32_t length) {
  return length * sizeof(tableEntry) / wordSize;
}

// Copy super class's vtable to the first part (prefix) of this class's vtable,
// and return the number of entries copied.  Expects that 'super' is the Java
// super class (arrays can have "array" super classes that must be skipped).
int klassVtable::initialize_from_super(Klass* super) {
  if (super == NULL) {
    return 0;
  } else if (is_preinitialized_vtable()) {
    // A shared class' vtable is preinitialized at dump time. No need to copy
    // methods from super class for shared class, as that was already done
    // during archiving time. However, if Jvmti has redefined a class,
    // copy super class's vtable in case the super class has changed.
    return super->vtable().length();
  } else {
    // copy methods from superKlass
    klassVtable superVtable = super->vtable();
    assert(superVtable.length() <= length(), "vtable too short");
#ifdef ASSERT
    superVtable.verify(tty, true);
#endif
    superVtable.copy_vtable_to(this);
    if (log_develop_is_enabled(Trace, vtables)) {
      ResourceMark rm;
      log_develop_trace(vtables)("copy vtable from %s to %s size %d",
                                 super->internal_name(), klass()->internal_name(),
                                 length());
    }
    return superVtable.length();
  }
}

//
// Revised lookup semantics   introduced 1.3 (Kestrel beta)
void klassVtable::initialize_vtable(bool checkconstraints, TRAPS) {
  // Note:  Arrays can have intermediate array supers.  Use java_super to skip them.
  InstanceKlass* super = _klass->java_super();

  bool is_shared = _klass->is_shared();

  if (!_klass->is_array_klass()) {
    ResourceMark rm(THREAD);
    log_develop_debug(vtables)("Initializing: %s", _klass->name()->as_C_string());
  }

  if (Universe::is_bootstrapping()) {
    assert(!is_shared, "sanity");
    // just clear everything
    for (int i = 0; i < length(); i++) scratch_table()[-i] = NULL;
    return;
  }

  int super_vtable_len = initialize_from_super(super);
  if (_klass->is_array_klass()) {
    assert(super_vtable_len == length(), "arrays shouldn't introduce new methods");
  } else {
    assert(_klass->is_instance_klass(), "must be InstanceKlass");

    Array<Method*>* methods = ik()->methods();
    int len = methods->length();
    int initialized = super_vtable_len;

    // Check each of this class's methods against super;
    // if override, replace in copy of super vtable, otherwise append to end
    for (int i = 0; i < len; i++) {
      // update_inherited_vtable can stop for gc - ensure using handles
      HandleMark hm(THREAD);
      assert(methods->at(i)->is_method(), "must be a Method*");
      methodHandle mh(THREAD, methods->at(i));

      bool needs_new_entry = update_inherited_vtable(ik(), mh, super_vtable_len, -1, checkconstraints, CHECK);

      if (needs_new_entry) {
        put_method_at(mh(), initialized);
        mh()->set_vtable_index(initialized); // set primary vtable index
        initialized++;
      }
    }

    // update vtable with default_methods
    Array<Method*>* default_methods = ik()->default_methods();

    if (default_methods != NULL) {
      len = default_methods->length();
      if (len > 0) {
        Array<int>* def_vtable_indices = NULL;
        if ((def_vtable_indices = ik()->default_vtable_indices()) == NULL) {
          //assert(!is_shared, "shared class def_vtable_indices does not exist"); // TODO: Reenable assert when CDS support is done
          def_vtable_indices = ik()->create_new_default_vtable_indices(len, CHECK);
        } else {
          assert(def_vtable_indices->length() == len, "reinit vtable len?");
        }
        for (int i = 0; i < len; i++) {
          HandleMark hm(THREAD);
          assert(default_methods->at(i)->is_method(), "must be a Method*");
          methodHandle mh(THREAD, default_methods->at(i));

          assert(!mh->is_private(), "private interface method in the default method list");
          bool needs_new_entry = update_inherited_vtable(ik(), mh, super_vtable_len, i, checkconstraints, CHECK);

          // needs new entry
          if (needs_new_entry) {
            put_method_at(mh(), initialized);
            if (is_preinitialized_vtable()) {
              // At runtime initialize_vtable is rerun for a shared class
              // (loaded by the non-boot loader) as part of link_class_impl().
              // The dumptime vtable index should be the same as the runtime index.
              assert(def_vtable_indices->at(i) == initialized,
                     "dump time vtable index is different from runtime index");
            } else {
              def_vtable_indices->at_put(i, initialized); //set vtable index
            }
            initialized++;
          }
        }
      }
    }

    // add miranda methods; it will also return the updated initialized
    // Interfaces do not need interface methods in their vtables
    // This includes miranda methods and during later processing, default methods
    if (!ik()->is_interface()) {
      initialized = fill_in_mirandas(initialized, THREAD);
    }

    // In class hierarchies where the accessibility is not increasing (i.e., going from private ->
    // package_private -> public/protected), the vtable might actually be smaller than our initial
    // calculation, for classfile versions for which we do not do transitive override
    // calculations.
    if (ik()->major_version() >= VTABLE_TRANSITIVE_OVERRIDE_VERSION) {
      assert(initialized == length(), "vtable initialization failed");
    } else {
      assert(initialized <= length(), "vtable initialization failed");
      for(;initialized < length(); initialized++) {
        scratch_table()[-initialized] = NULL;
      }
    }
  }

  link_code(!checkconstraints);
  _table[1] = make_entry(0, SharedRuntime::get_bad_call_stub());

  NOT_PRODUCT(verify(tty, true));
}

// Called for cases where a method does not override its superclass' vtable entry
// For bytecodes not produced by javac together it is possible that a method does not override
// the superclass's method, but might indirectly override a super-super class's vtable entry
// If none found, return a null superk, else return the superk of the method this does override
// For public and protected methods: if they override a superclass, they will
// also be overridden themselves appropriately.
// Private methods do not override, and are not overridden and are not in the vtable.
// Package Private methods are trickier:
// e.g. P1.A, pub m
// P2.B extends A, package private m
// P1.C extends B, public m
// P1.C.m needs to override P1.A.m and can not override P2.B.m
// Therefore: all package private methods need their own vtable entries for
// them to be the root of an inheritance overriding decision
// Package private methods may also override other vtable entries
InstanceKlass* klassVtable::find_transitive_override(InstanceKlass* initialsuper, const methodHandle& target_method,
                            int vtable_index, Handle target_loader, Symbol* target_classname, Thread * THREAD) {
  InstanceKlass* superk = initialsuper;
  while (superk != NULL && superk->super() != NULL) {
    klassVtable ssVtable = (superk->super())->vtable();
    if (vtable_index < ssVtable.length()) {
      Method* super_method = ssVtable.method_at(vtable_index);
      // get the class holding the matching method
      // make sure you use that class for is_override
      InstanceKlass* supermethodholder = super_method->method_holder();
#ifndef PRODUCT
      Symbol* name= target_method()->name();
      Symbol* signature = target_method()->signature();
      assert(super_method->name() == name && super_method->signature() == signature, "vtable entry name/sig mismatch");
#endif

      if (supermethodholder->is_override(methodHandle(THREAD, super_method), target_loader, target_classname, THREAD)) {
        if (log_develop_is_enabled(Trace, vtables)) {
          ResourceMark rm(THREAD);
          LogTarget(Trace, vtables) lt;
          LogStream ls(lt);
          char* sig = target_method()->name_and_sig_as_C_string();
          ls.print("transitive overriding superclass %s with %s index %d, original flags: ",
                       supermethodholder->internal_name(),
                       sig, vtable_index);
          super_method->print_linkage_flags(&ls);
          ls.print("overriders flags: ");
          target_method->print_linkage_flags(&ls);
          ls.cr();
        }

        break; // return found superk
      }
    } else  {
      // super class has no vtable entry here, stop transitive search
      superk = (InstanceKlass*)NULL;
      break;
    }
    // if no override found yet, continue to search up
    superk = superk->super() == NULL ? NULL : InstanceKlass::cast(superk->super());
  }

  return superk;
}

static void log_vtables(int i, bool overrides, const methodHandle& target_method,
                        Klass* target_klass, Method* super_method,
                        Thread* thread) {
#ifndef PRODUCT
  if (log_develop_is_enabled(Trace, vtables)) {
    ResourceMark rm(thread);
    LogTarget(Trace, vtables) lt;
    LogStream ls(lt);
    char* sig = target_method()->name_and_sig_as_C_string();
    if (overrides) {
      ls.print("overriding with %s index %d, original flags: ",
                   sig, i);
    } else {
      ls.print("NOT overriding with %s index %d, original flags: ",
                   sig, i);
    }
    super_method->print_linkage_flags(&ls);
    ls.print("overriders flags: ");
    target_method->print_linkage_flags(&ls);
    ls.cr();
  }
#endif
}

// Update child's copy of super vtable for overrides
// OR return true if a new vtable entry is required.
// Only called for InstanceKlass's, i.e. not for arrays
// If that changed, could not use _klass as handle for klass
bool klassVtable::update_inherited_vtable(InstanceKlass* klass, const methodHandle& target_method,
                                          int super_vtable_len, int default_index,
                                          bool checkconstraints, TRAPS) {
  ResourceMark rm(THREAD);
  bool allocate_new = true;
  assert(klass->is_instance_klass(), "must be InstanceKlass");

  Array<int>* def_vtable_indices = NULL;
  bool is_default = false;

  // default methods are non-private concrete methods in superinterfaces which are added
  // to the vtable with their real method_holder.
  // Since vtable and itable indices share the same storage, don't touch
  // the default method's real vtable/itable index.
  // default_vtable_indices stores the vtable value relative to this inheritor
  if (default_index >= 0 ) {
    is_default = true;
    def_vtable_indices = klass->default_vtable_indices();
    assert(!target_method()->is_private(), "private interface method flagged as default");
    assert(def_vtable_indices != NULL, "def vtable alloc?");
    assert(default_index <= def_vtable_indices->length(), "def vtable len?");
  } else {
    assert(klass == target_method()->method_holder(), "caller resp.");
    // Initialize the method's vtable index to "nonvirtual".
    // If we allocate a vtable entry, we will update it to a non-negative number.
    target_method()->set_vtable_index(Method::nonvirtual_vtable_index);
  }

  // Private, static and <init> methods are never in
  if (target_method()->is_private() || target_method()->is_static() ||
      (target_method()->name()->fast_compare(vmSymbols::object_initializer_name()) == 0)) {
    return false;
  }

  if (target_method->is_final_method(klass->access_flags())) {
    // a final method never needs a new entry; final methods can be statically
    // resolved and they have to be present in the vtable only if they override
    // a super's method, in which case they re-use its entry
    allocate_new = false;
  } else if (klass->is_interface()) {
    allocate_new = false;  // see note below in needs_new_vtable_entry
    // An interface never allocates new vtable slots, only inherits old ones.
    // This method will either be assigned its own itable index later,
    // or be assigned an inherited vtable index in the loop below.
    // default methods inherited by classes store their vtable indices
    // in the inheritor's default_vtable_indices.
    // default methods inherited by interfaces may already have a
    // valid itable index, if so, don't change it.
    // Overpass methods in an interface will be assigned an itable index later
    // by an inheriting class.
    if (!is_default || !target_method()->has_itable_index()) {
      target_method()->set_vtable_index(Method::itable_index_max);
    }
  }

  // we need a new entry if there is no superclass
  Klass* super = klass->super();
  if (super == NULL) {
    return allocate_new;
  }

  // search through the vtable and update overridden entries
  // Since check_signature_loaders acquires SystemDictionary_lock
  // which can block for gc, once we are in this loop, use handles
  // For classfiles built with >= jdk7, we now look for transitive overrides

  Symbol* name = target_method()->name();
  Symbol* signature = target_method()->signature();

  Klass* target_klass = target_method()->method_holder();
  if (target_klass == NULL) {
    target_klass = _klass;
  }

  Handle target_loader(THREAD, target_klass->class_loader());

  Symbol* target_classname = target_klass->name();
  for(int i = 0; i < super_vtable_len; i++) {
    Method* super_method;
    if (is_preinitialized_vtable()) {
      // If this is a shared class, the vtable is already in the final state (fully
      // initialized). Need to look at the super's vtable.
      klassVtable superVtable = super->vtable();
      super_method = superVtable.method_at(i);
    } else {
      super_method = scratch_table()[-i];
    }
    // Check if method name matches.  Ignore match if klass is an interface and the
    // matching method is a non-public java.lang.Object method.  (See JVMS 5.4.3.4)
    // This is safe because the method at this slot should never get invoked.
    // (TBD: put in a method to throw NoSuchMethodError if this slot is ever used.)
    if (super_method->name() == name && super_method->signature() == signature &&
        (!_klass->is_interface() ||
         !SystemDictionary::is_nonpublic_Object_method(super_method))) {

      // get super_klass for method_holder for the found method
      InstanceKlass* super_klass =  super_method->method_holder();

      // Whether the method is being overridden
      bool overrides = false;

      // private methods are also never overridden
      if (!super_method->is_private() &&
          (is_default
          || ((super_klass->is_override(methodHandle(THREAD, super_method), target_loader, target_classname, THREAD))
          || ((klass->major_version() >= VTABLE_TRANSITIVE_OVERRIDE_VERSION)
          && ((super_klass = find_transitive_override(super_klass,
                             target_method, i, target_loader,
                             target_classname, THREAD))
                             != (InstanceKlass*)NULL)))))
        {
        // Package private methods always need a new entry to root their own
        // overriding. They may also override other methods.
        if (!target_method()->is_package_private()) {
          allocate_new = false;
        }

        // Do not check loader constraints for overpass methods because overpass
        // methods are created by the jvm to throw exceptions.
        if (checkconstraints && !target_method()->is_overpass()) {
          // Override vtable entry if passes loader constraint check
          // if loader constraint checking requested
          // No need to visit his super, since he and his super
          // have already made any needed loader constraints.
          // Since loader constraints are transitive, it is enough
          // to link to the first super, and we get all the others.
          Handle super_loader(THREAD, super_klass->class_loader());

          if (target_loader() != super_loader()) {
            ResourceMark rm(THREAD);
            Symbol* failed_type_symbol =
              SystemDictionary::check_signature_loaders(signature, _klass,
                                                        target_loader, super_loader,
                                                        true, CHECK_(false));
            if (failed_type_symbol != NULL) {
              stringStream ss;
              ss.print("loader constraint violation for class %s: when selecting "
                       "overriding method '", klass->external_name());
              target_method()->print_external_name(&ss),
              ss.print("' the class loader %s of the "
                       "selected method's type %s, and the class loader %s for its super "
                       "type %s have different Class objects for the type %s used in the signature (%s; %s)",
                       target_klass->class_loader_data()->loader_name_and_id(),
                       target_klass->external_name(),
                       super_klass->class_loader_data()->loader_name_and_id(),
                       super_klass->external_name(),
                       failed_type_symbol->as_klass_external_name(),
                       target_klass->class_in_module_of_loader(false, true),
                       super_klass->class_in_module_of_loader(false, true));
              THROW_MSG_(vmSymbols::java_lang_LinkageError(), ss.as_string(), false);
            }
          }
        }

        put_method_at(target_method(), i);
        overrides = true;
        if (!is_default) {
          target_method()->set_vtable_index(i);
        } else {
          if (def_vtable_indices != NULL) {
            if (is_preinitialized_vtable()) {
              // At runtime initialize_vtable is rerun as part of link_class_impl()
              // for a shared class loaded by the non-boot loader.
              // The dumptime vtable index should be the same as the runtime index.
              assert(def_vtable_indices->at(default_index) == i,
                     "dump time vtable index is different from runtime index");
            } else {
              def_vtable_indices->at_put(default_index, i);
            }
          }
          assert(super_method->is_default_method() || super_method->is_overpass()
                 || super_method->is_abstract(), "default override error");
        }
      } else {
        overrides = false;
      }
      log_vtables(i, overrides, target_method, target_klass, super_method, THREAD);
    }
  }
  return allocate_new;
}

void klassVtable::put_method_at(Method* m, int index) {
  assert(!m->is_private(), "private methods should not be in vtable");
  if (is_preinitialized_vtable()) {
    // At runtime initialize_vtable is rerun as part of link_class_impl()
    // for shared class loaded by the non-boot loader to obtain the loader
    // constraints based on the runtime classloaders' context. The dumptime
    // method at the vtable index should be the same as the runtime method.
    assert(unchecked_method_at(index) == m,
           "archived method is different from the runtime method");
  } else {
    if (log_develop_is_enabled(Trace, vtables)) {
      ResourceMark rm;
      LogTarget(Trace, vtables) lt;
      LogStream ls(lt);
      const char* sig = (m != NULL) ? m->name_and_sig_as_C_string() : "<NULL>";
      ls.print("adding %s at index %d, flags: ", sig, index);
      if (m != NULL) {
        m->print_linkage_flags(&ls);
      }
      ls.cr();
    }
    // Lazily initialize the code pointers as they get used by compiled calls.
    scratch_table()[-index] = m;
  }
}

// Find out if a method "m" with superclass "super", loader "classloader" and
// name "classname" needs a new vtable entry.  Let P be a class package defined
// by "classloader" and "classname".
// NOTE: The logic used here is very similar to the one used for computing
// the vtables indices for a method. We cannot directly use that function because,
// we allocate the InstanceKlass at load time, and that requires that the
// superclass has been loaded.
// However, the vtable entries are filled in at link time, and therefore
// the superclass' vtable may not yet have been filled in.
bool klassVtable::needs_new_vtable_entry(const methodHandle& target_method,
                                         const Klass* super,
                                         Handle classloader,
                                         Symbol* classname,
                                         AccessFlags class_flags,
                                         u2 major_version,
                                         TRAPS) {
  if (class_flags.is_interface()) {
    // Interfaces do not use vtables, except for java.lang.Object methods,
    // so there is no point to assigning
    // a vtable index to any of their local methods.  If we refrain from doing this,
    // we can use Method::_vtable_index to hold the itable index
    return false;
  }

  if (target_method->is_final_method(class_flags) ||
      // a final method never needs a new entry; final methods can be statically
      // resolved and they have to be present in the vtable only if they override
      // a super's method, in which case they re-use its entry
      (target_method()->is_private()) ||
      // private methods don't need to be in vtable
      (target_method()->is_static()) ||
      // static methods don't need to be in vtable
      (target_method()->name()->fast_compare(vmSymbols::object_initializer_name()) == 0)
      // <init> is never called dynamically-bound
      ) {
    return false;
  }

  // Concrete interface methods do not need new entries, they override
  // abstract method entries using default inheritance rules
  if (target_method()->method_holder() != NULL &&
      target_method()->method_holder()->is_interface()  &&
      !target_method()->is_abstract()) {
    assert(target_method()->is_default_method(),
           "unexpected interface method type");
    return false;
  }

  // we need a new entry if there is no superclass
  if (super == NULL) {
    return true;
  }

  // Package private methods always need a new entry to root their own
  // overriding. This allows transitive overriding to work.
  if (target_method()->is_package_private()) {
    return true;
  }

  // search through the super class hierarchy to see if we need
  // a new entry
  ResourceMark rm(THREAD);
  Symbol* name = target_method()->name();
  Symbol* signature = target_method()->signature();
  const Klass* k = super;
  Method* super_method = NULL;
  bool found_pkg_prvt_method = false;
  while (k != NULL) {
    // lookup through the hierarchy for a method with matching name and sign.
    super_method = InstanceKlass::cast(k)->lookup_method(name, signature);
    if (super_method == NULL) {
      break; // we still have to search for a matching miranda method
    }
    // get the class holding the matching method
    // make sure you use that class for is_override
    InstanceKlass* superk = super_method->method_holder();
    // we want only instance method matches
    // ignore private methods found via lookup_method since they do not participate in overriding,
    // and since we do override around them: e.g. a.m pub/b.m private/c.m pub,
    // ignore private, c.m pub does override a.m pub
    // For classes that were not javac'd together, we also do transitive overriding around
    // methods that have less accessibility
    if ((!super_method->is_static()) &&
       (!super_method->is_private())) {
      if (superk->is_override(methodHandle(THREAD, super_method), classloader, classname, THREAD)) {
        return false;
      // else keep looking for transitive overrides
      }
      // If we get here then one of the super classes has a package private method
      // that will not get overridden because it is in a different package.  But,
      // that package private method does "override" any matching methods in super
      // interfaces, so there will be no miranda vtable entry created.  So, set flag
      // to TRUE for use below, in case there are no methods in super classes that
      // this target method overrides.
      assert(super_method->is_package_private(), "super_method must be package private");
      assert(!superk->is_same_class_package(classloader(), classname),
             "Must be different packages");
      found_pkg_prvt_method = true;
    }

    // Start with lookup result and continue to search up, for versions supporting transitive override
    if (major_version >= VTABLE_TRANSITIVE_OVERRIDE_VERSION) {
      k = superk->super(); // haven't found an override match yet; continue to look
    } else {
      break;
    }
  }

  // If found_pkg_prvt_method is set, then the ONLY matching method in the
  // superclasses is package private in another package. That matching method will
  // prevent a miranda vtable entry from being created. Because the target method can not
  // override the package private method in another package, then it needs to be the root
  // for its own vtable entry.
  if (found_pkg_prvt_method) {
     return true;
  }

  // if the target method is public or protected it may have a matching
  // miranda method in the super, whose entry it should re-use.
  // Actually, to handle cases that javac would not generate, we need
  // this check for all access permissions.
  const InstanceKlass *sk = InstanceKlass::cast(super);
  if (sk->has_miranda_methods()) {
    if (sk->lookup_method_in_all_interfaces(name, signature, Klass::find_defaults) != NULL) {
      return false; // found a matching miranda; we do not need a new entry
    }
  }
  return true; // found no match; we need a new entry
}

// Support for miranda methods

// get the vtable index of a miranda method with matching "name" and "signature"
int klassVtable::index_of_miranda(Symbol* name, Symbol* signature) {
  // search from the bottom, might be faster
  for (int i = (length() - 1); i >= 0; i--) {
    Method* m = unchecked_method_at(i);
    if (is_miranda_entry_at(i) &&
        m->name() == name && m->signature() == signature) {
      return i;
    }
  }
  return Method::invalid_vtable_index;
}

// check if an entry at an index is miranda
// requires that method m at entry be declared ("held") by an interface.
bool klassVtable::is_miranda_entry_at(int i) {
  Method* m = method_at(i);
  Klass* method_holder = m->method_holder();
  InstanceKlass *mhk = InstanceKlass::cast(method_holder);

  // miranda methods are public abstract instance interface methods in a class's vtable
  if (mhk->is_interface()) {
    assert(m->is_public(), "should be public");
    assert(ik()->implements_interface(method_holder) , "this class should implement the interface");
    if (is_miranda(m, ik()->methods(), ik()->default_methods(), ik()->super(), klass()->is_interface())) {
      return true;
    }
  }
  return false;
}

// Check if a method is a miranda method, given a class's methods array,
// its default_method table and its super class.
// "Miranda" means an abstract non-private method that would not be
// overridden for the local class.
// A "miranda" method should only include non-private interface
// instance methods, i.e. not private methods, not static methods,
// not default methods (concrete interface methods), not overpass methods.
// If a given class already has a local (including overpass) method, a
// default method, or any of its superclasses has the same which would have
// overridden an abstract method, then this is not a miranda method.
//
// Miranda methods are checked multiple times.
// Pass 1: during class load/class file parsing: before vtable size calculation:
// include superinterface abstract and default methods (non-private instance).
// We include potential default methods to give them space in the vtable.
// During the first run, the current instanceKlass has not yet been
// created, the superclasses and superinterfaces do have instanceKlasses
// but may not have vtables, the default_methods list is empty, no overpasses.
// Default method generation uses the all_mirandas array as the starter set for
// maximally-specific default method calculation.  So, for both classes and
// interfaces, it is necessary that the first pass will find all non-private
// interface instance methods, whether or not they are concrete.
//
// Pass 2: recalculated during vtable initialization: only include abstract methods.
// The goal of pass 2 is to walk through the superinterfaces to see if any of
// the superinterface methods (which were all abstract pre-default methods)
// need to be added to the vtable.
// With the addition of default methods, we have three new challenges:
// overpasses, static interface methods and private interface methods.
// Static and private interface methods do not get added to the vtable and
// are not seen by the method resolution process, so we skip those.
// Overpass methods are already in the vtable, so vtable lookup will
// find them and we don't need to add a miranda method to the end of
// the vtable. So we look for overpass methods and if they are found we
// return false. Note that we inherit our superclasses vtable, so
// the superclass' search also needs to use find_overpass so that if
// one is found we return false.
// False means - we don't need a miranda method added to the vtable.
//
// During the second run, default_methods is set up, so concrete methods from
// superinterfaces with matching names/signatures to default_methods are already
// in the default_methods list and do not need to be appended to the vtable
// as mirandas. Abstract methods may already have been handled via
// overpasses - either local or superclass overpasses, which may be
// in the vtable already.
//
// Pass 3: They are also checked by link resolution and selection,
// for invocation on a method (not interface method) reference that
// resolves to a method with an interface as its method_holder.
// Used as part of walking from the bottom of the vtable to find
// the vtable index for the miranda method.
//
// Part of the Miranda Rights in the US mean that if you do not have
// an attorney one will be appointed for you.
bool klassVtable::is_miranda(Method* m, Array<Method*>* class_methods,
                             Array<Method*>* default_methods, const Klass* super,
                             bool is_interface) {
  if (m->is_static() || m->is_private() || m->is_overpass()) {
    return false;
  }
  Symbol* name = m->name();
  Symbol* signature = m->signature();

  // First look in local methods to see if already covered
  if (InstanceKlass::find_local_method(class_methods, name, signature,
              Klass::find_overpass, Klass::skip_static, Klass::skip_private) != NULL)
  {
    return false;
  }

  // Check local default methods
  if ((default_methods != NULL) &&
    (InstanceKlass::find_method(default_methods, name, signature) != NULL))
   {
     return false;
   }

  // Iterate on all superclasses, which should be InstanceKlasses.
  // Note that we explicitly look for overpasses at each level.
  // Overpasses may or may not exist for supers for pass 1,
  // they should have been created for pass 2 and later.

  for (const Klass* cursuper = super; cursuper != NULL; cursuper = cursuper->super())
  {
     Method* found_mth = InstanceKlass::cast(cursuper)->find_local_method(name, signature,
       Klass::find_overpass, Klass::skip_static, Klass::skip_private);
     // Ignore non-public methods in java.lang.Object if klass is an interface.
     if (found_mth != NULL && (!is_interface ||
         !SystemDictionary::is_nonpublic_Object_method(found_mth))) {
       return false;
     }
  }

  return true;
}

// Scans current_interface_methods for miranda methods that do not
// already appear in new_mirandas, or default methods,  and are also not defined-and-non-private
// in super (superclass).  These mirandas are added to all_mirandas if it is
// not null; in addition, those that are not duplicates of miranda methods
// inherited by super from its interfaces are added to new_mirandas.
// Thus, new_mirandas will be the set of mirandas that this class introduces,
// all_mirandas will be the set of all mirandas applicable to this class
// including all defined in superclasses.
void klassVtable::add_new_mirandas_to_lists(
    GrowableArray<Method*>* new_mirandas, GrowableArray<Method*>* all_mirandas,
    Array<Method*>* current_interface_methods, Array<Method*>* class_methods,
    Array<Method*>* default_methods, const Klass* super, bool is_interface) {

  // iterate thru the current interface's method to see if it a miranda
  int num_methods = current_interface_methods->length();
  for (int i = 0; i < num_methods; i++) {
    Method* im = current_interface_methods->at(i);
    bool is_duplicate = false;
    int num_of_current_mirandas = new_mirandas->length();
    // check for duplicate mirandas in different interfaces we implement
    for (int j = 0; j < num_of_current_mirandas; j++) {
      Method* miranda = new_mirandas->at(j);
      if ((im->name() == miranda->name()) &&
          (im->signature() == miranda->signature())) {
        is_duplicate = true;
        break;
      }
    }

    if (!is_duplicate) { // we don't want duplicate miranda entries in the vtable
      if (is_miranda(im, class_methods, default_methods, super, is_interface)) { // is it a miranda at all?
        const InstanceKlass *sk = InstanceKlass::cast(super);
        // check if it is a duplicate of a super's miranda
        if (sk->lookup_method_in_all_interfaces(im->name(), im->signature(), Klass::find_defaults) == NULL) {
          new_mirandas->append(im);
        }
        if (all_mirandas != NULL) {
          all_mirandas->append(im);
        }
      }
    }
  }
}

void klassVtable::get_mirandas(GrowableArray<Method*>* new_mirandas,
                               GrowableArray<Method*>* all_mirandas,
                               const Klass* super,
                               Array<Method*>* class_methods,
                               Array<Method*>* default_methods,
                               Array<InstanceKlass*>* local_interfaces,
                               bool is_interface) {
  assert((new_mirandas->length() == 0) , "current mirandas must be 0");

  // iterate thru the local interfaces looking for a miranda
  int num_local_ifs = local_interfaces->length();
  for (int i = 0; i < num_local_ifs; i++) {
    InstanceKlass *ik = InstanceKlass::cast(local_interfaces->at(i));
    add_new_mirandas_to_lists(new_mirandas, all_mirandas,
                              ik->methods(), class_methods,
                              default_methods, super, is_interface);
    // iterate thru each local's super interfaces
    Array<InstanceKlass*>* super_ifs = ik->transitive_interfaces();
    int num_super_ifs = super_ifs->length();
    for (int j = 0; j < num_super_ifs; j++) {
      InstanceKlass *sik = super_ifs->at(j);
      add_new_mirandas_to_lists(new_mirandas, all_mirandas,
                                sik->methods(), class_methods,
                                default_methods, super, is_interface);
    }
  }
}

// Discover miranda methods ("miranda" = "interface abstract, no binding"),
// and append them into the vtable starting at index initialized,
// return the new value of initialized.
// Miranda methods use vtable entries, but do not get assigned a vtable_index
// The vtable_index is discovered by searching from the end of the vtable
int klassVtable::fill_in_mirandas(int initialized, TRAPS) {
  ResourceMark rm(THREAD);
  GrowableArray<Method*> mirandas(20);
  get_mirandas(&mirandas, NULL, ik()->super(), ik()->methods(),
               ik()->default_methods(), ik()->local_interfaces(),
               klass()->is_interface());
  for (int i = 0; i < mirandas.length(); i++) {
    if (log_develop_is_enabled(Trace, vtables)) {
      Method* meth = mirandas.at(i);
      LogTarget(Trace, vtables) lt;
      LogStream ls(lt);
      if (meth != NULL) {
        char* sig = meth->name_and_sig_as_C_string();
        ls.print("fill in mirandas with %s index %d, flags: ",
                     sig, initialized);
        meth->print_linkage_flags(&ls);
        ls.cr();
      }
    }
    put_method_at(mirandas.at(i), initialized);
    ++initialized;
  }
  return initialized;
}

// Copy this class's vtable to the vtable beginning at start.
// Used to copy superclass vtable to prefix of subclass's vtable.
void klassVtable::copy_vtable_to(klassVtable* target) {
  SelectorMap<Method*> method_map = SystemDictionary::method_selector_map();
  size_t size_words = length();
  for (size_t i = 0; i < size_words; ++i) {
    tableEntry* src = _table - i;
    Method** dst = target->scratch_table() - i;
    Method* method = src->selector() == 0 ? NULL : method_map.get(src->selector());
    *dst = method;
  }
}

//-----------------------------------------------------------------------------------------
// Itable code

class itableHashTableBuilder : public StackObj {
  friend class ClassFileParser;
  friend class klassItable;
  struct Entry {
    uint32_t _selector;
    Method* _target;
  };

  const static uint32_t  _initial_capacity = 8;
  const static uint32_t  _max_reshuffling_iterations = 8;
  const static uint32_t  _max_refinement_iterations = 2;
  InstanceKlass*         _ik;
  int                    _random;
  int                    _seed;
  Array<InstanceKlass*>* _transitive_interfaces;
  bool                   _resolve_methods;
  Entry*                 _table;
  uint32_t               _capacity;
  uint32_t               _size;
  uint32_t               _collisions;
  uint8_t*               _itable_blob;

  void put_code(uint32_t index, tableEntry entry);

  int random();

  uint32_t mask() const {
    return _capacity - 1;
  }

  uint64_t primary_bucket(uint32_t selector) {
    return selector & mask();
  }

  uint64_t secondary_bucket(uint32_t selector) {
    return (selector >> 16) & mask();
  }

  void resize(uint32_t capacity);
  bool set(uint32_t selector, Method* m);

  bool has_many_collisions();
  static CompiledMethod* cm(address code);
  void refine_precision();
  void populate_table();

  static uint32_t itable_capacity(uint8_t* blob) { return reinterpret_cast<uint32_t*>(blob)[0] + 1; }
  static uint32_t itable_size_bytes(uint8_t* blob) { return itable_capacity(blob) * sizeof(uint64_t) + klassItable::itable_header_size_bytes(); }
  static tableEntry* itable_table(uint8_t* blob) { return (tableEntry*)(blob + klassItable::itable_header_size_bytes()); }

public:
  itableHashTableBuilder(InstanceKlass* ik);
  itableHashTableBuilder(uint32_t seed, Array<InstanceKlass*>* transitive_interfaces);
  ~itableHashTableBuilder();
  void attach_itable(uint8_t* itable_blob);
  void create_itable();
  uint8_t* selector_map_blob();
  void link_code(uint32_t selector, address method_code);
  Method* get(uint32_t selector);
  size_t compute_itable_size_words() {
    size_t itable_size =_capacity * sizeof(tableEntry) / wordSize;
    itable_size += itable_size == 0 ? 0 : klassItable::itable_header_size_words();
    // Statistics
    klassItable::update_stats((int)itable_size * wordSize);
    return itable_size;
  }

  static ByteSize itable_mask_offset()  { return in_ByteSize(0); }
};

bool itableHashTableBuilder::has_many_collisions() {
  return _collisions > (_size >> 3);
}

void itableHashTableBuilder::refine_precision() {
  if (_resolve_methods) {
    // Too late to refine the precision when resolving methods.
    return;
  }
  for (uint32_t i = 0; i < _max_refinement_iterations && has_many_collisions(); i++) {
    resize(_capacity << 1);
    populate_table();
  }
}

itableHashTableBuilder::itableHashTableBuilder(InstanceKlass* ik)
  : _ik(ik),
    _random(_ik->itable_seed()),
    _seed(_ik->itable_seed()),
    _transitive_interfaces(ik->transitive_interfaces()),
    _resolve_methods(true),
    _table(NULL),
    _capacity(0),
    _size(0),
    _collisions(0),
    _itable_blob(NULL)
{
  log_debug(itables)("Re-populating itable");
  int itable_size_words = _ik->itable_length() - (int)klassItable::itable_header_size_words();
  uint32_t new_capacity = itable_size_words * wordSize / sizeof(tableEntry);
  assert(is_power_of_2(new_capacity), "sanity");
  resize(new_capacity);
  populate_table();
}

itableHashTableBuilder::itableHashTableBuilder(uint32_t seed, Array<InstanceKlass*>* transitive_interfaces)
  : _ik(NULL),
    _random(seed),
    _seed(seed),
    _transitive_interfaces(transitive_interfaces),
    _resolve_methods(false),
    _table(NULL),
    _capacity(0),
    _size(0),
    _collisions(0),
    _itable_blob(NULL)
{
  log_debug(itables)("Populating itable");
  populate_table();
  refine_precision();
}

itableHashTableBuilder::~itableHashTableBuilder() {
  FREE_C_HEAP_ARRAY(Entry, _table);
}

void itableHashTableBuilder::attach_itable(uint8_t* itable_blob) {
  _itable_blob = itable_blob;
}

void itableHashTableBuilder::create_itable() {
  uint32_t* blob_int32 = (uint32_t*)_ik->start_of_itable();
  tableEntry* blob_table = _ik->itable_table();
  assert (_ik->has_itable(), "Don't create itable if there is no itable");

  for (uint32_t i = 0; i < _capacity; i++) {
    Entry entry = _table[i];
    blob_table[i] = make_itable_entry(entry._selector, entry._target);
  }
  blob_table[0] = make_itable_entry(0, NULL);
  blob_int32[0] = _capacity - 1; // mask
}

void itableHashTableBuilder::resize(uint32_t size) {
  Entry* old_table = _table;

  assert(!_resolve_methods || old_table == NULL, "sanity");

  _capacity = size;
  _table = NEW_C_HEAP_ARRAY(Entry, size, mtInternal);
  _size = 0;
  _collisions = 0;
  Copy::zero_to_bytes(_table, sizeof(Entry) * size);

  if (old_table != NULL) {
    FREE_C_HEAP_ARRAY(CacheEntry, old_table);
  }
}

Method* itableHashTableBuilder::get(uint32_t selector) {
  assert(selector != 0, "sanity");

  if (_size == 0) {
    return NULL;
  }

  uint32_t pb = primary_bucket(selector);
  Entry primary_entry = _table[pb];
  if (primary_entry._selector == selector) {
    return primary_entry._target;
  }

  uint sb = secondary_bucket(selector);
  Entry secondary_entry = _table[sb];
  if (secondary_entry._selector == selector) {
    return secondary_entry._target;
  }

  return NULL;
}

int itableHashTableBuilder::random() {
  int next = random_helper(_random);
  _random = next;
  return next;
}

CompiledMethod* itableHashTableBuilder::cm(address code) {
  CodeBlob* cb = CodeCache::find_blob(code);
  if (cb == NULL) {
    return NULL;
  }
  return cb->as_compiled_method_or_null();
}

void itableHashTableBuilder::put_code(uint32_t index, tableEntry entry) {
  itable_table(_itable_blob)[index] = entry;
}

void itableHashTableBuilder::link_code(uint32_t selector, address method_code) {
  assert(CompiledMethod_lock->owned_by_self(), "sanity");
  for (;;) {
    bool is_important = cm(method_code) != NULL;
    uint32_t pb = primary_bucket(selector);
    tableEntry itable_primary = itable_table(_itable_blob)[pb];
    Entry primary_entry = _table[pb];

    uint32_t sb = secondary_bucket(selector);
    tableEntry itable_secondary = itable_table(_itable_blob)[sb];
    Entry secondary_entry = _table[sb];

    if (itable_primary.selector() == 0 || itable_primary.selector() == selector) {
      put_code(pb, make_entry(selector, method_code));
      return;
    }

    if (itable_primary.selector() != primary_entry._selector && is_important) {
      // Victimize primary code if we have priority. We want compiled code here.
      put_code(pb, make_entry(selector, method_code));
      selector = itable_primary.selector();
      method_code = itable_primary.code();
      continue;
    }

    if (itable_secondary.selector() == 0 || itable_secondary.selector() == selector) {
      // Grab secondary bucket if available
      put_code(sb, make_entry(selector, method_code));
      return;
    }

    if (itable_primary.selector() != primary_entry._selector) {
      // Victimize primary code that violates canonical order.
      put_code(pb, make_entry(selector, method_code));
      selector = itable_primary.selector();
      method_code = itable_primary.code();
      continue;
    }

    // Victimize secondary bucket which is guaranteed offender of canonical order.
    assert(itable_secondary.selector() != secondary_entry._selector, "invariant");
    put_code(sb, make_entry(selector, method_code));
    selector = itable_secondary.selector();
    method_code = itable_secondary.code();
  }
}

bool itableHashTableBuilder::set(uint32_t selector, Method* method) {
  if (_table == NULL) {
    resize(_initial_capacity);
  }

  if (_size + 1 > MAX2(_capacity - (_capacity >> 3), _initial_capacity)) {
    return false;
  }

  for (;;) {
    for (uint i = 0; i < _max_reshuffling_iterations; ++i) {
      uint32_t pb = primary_bucket(selector);
      Entry primary_entry = _table[pb];
      uint sb = secondary_bucket(selector);
      Entry secondary_entry = _table[sb];

      if (primary_entry._selector == selector || secondary_entry._selector == selector) {
        // Re-insert... ignore.
        return true;
      }

      if (primary_entry._selector == 0) {
        _table[pb]._selector = selector;
        _table[pb]._target = method;
        ++_size;
        return true;
      }

      if (secondary_entry._selector == 0) {
        _table[sb]._selector = selector;
        _table[sb]._target = method;
        ++_size;
        ++_collisions;
        return true;
      }

      // Pick a non-trivial victim bucket and re-insert it.
      if ((random() & 1) == 0) {
        // Victimize first bucket
        _table[pb]._selector = selector;
        _table[pb]._target = method;
        selector = primary_entry._selector;
        method = primary_entry._target;
      } else {
        // Victimize second bucket
        _table[sb]._selector = selector;
        _table[sb]._target = method;
        selector = secondary_entry._selector;
        method = secondary_entry._target;
      }
    }

    // Resize if out of reshuffling budget for a given size
    return false;
  }
}

klassItable::klassItable(InstanceKlass* klass) {
  _klass = klass;
}

uint32_t klassItable::target_selector_for_selector(uint32_t selector) {
  SelectorMap<uint32_t> itable_map(_klass->interpreter_itable_selector_addr());
  if (!itable_map.contains(selector)) {
    return 0;
  }
  return itable_map.get(selector);
}

Method* klassItable::target_method_for_selector(uint32_t selector) {
  uint32_t target_selector = target_selector_for_selector(selector);
  if (target_selector == 0) {
    return NULL;
  }
  SelectorMap<Method*> method_map = SystemDictionary::method_selector_map();
  return method_map.get(target_selector);
}

void klassItable::link_code(Method* method) {
  if (!_klass->has_itable()) {
    return;
  }

  assert(_klass->is_linked(), "itable has been created");

  SelectorMap<uint32_t> itable_selector_map(_klass->interpreter_itable_selector_addr());
  uint32_t target_selector = method->selector();

  uint32_t* blob_int32 = (uint32_t*)_klass->start_of_itable();
  tableEntry* blob_table = _klass->itable_table();
  uint32_t mask = *blob_int32;

  bool suboptimal_linking = false;

  for (uint32_t i = 0; !suboptimal_linking && i < itable_selector_map.capacity(); ++i) {
    uint32_t selector = itable_selector_map.selector_table()[i];
    if (selector == 0) {
      continue;
    }
    if (itable_selector_map.get(selector) == target_selector) {
      uint32_t pb = selector & mask;
      uint32_t sb = (selector >> 16) & mask;
      if (blob_table[pb].selector() == selector) {
        blob_table[pb] = make_itable_entry(selector, method);
      }
      if (blob_table[sb].selector() == selector) {
        blob_table[sb] = make_itable_entry(selector, method);
        CompiledMethod* cm = method->code();
        suboptimal_linking = cm != NULL && (cm->is_compiled_by_c2() || cm->is_compiled_by_jvmci());
      }
    }
  }

  if (suboptimal_linking) {
    itableHashTableBuilder builder(_klass);
    builder.attach_itable((uint8_t*)_klass->start_of_itable());

    for (uint32_t i = 0; i < itable_selector_map.capacity(); ++i) {
      uint32_t selector = itable_selector_map.selector_table()[i];
      if (selector == 0) {
        continue;
      }
      if (itable_selector_map.get(selector) == target_selector) {
        address method_code = tableEntry::table_entry_code(method, true /* is_itable */);
        builder.link_code(selector, method_code);
      }
    }
  }
}

void klassItable::link_table_code() {
  if (!_klass->has_itable() || !_klass->is_linked()) {
    return;
  }

  MutexLocker ml(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
  SelectorMap<Method*> method_selector_map = SystemDictionary::method_selector_map();
  SelectorMap<uint32_t> itable_selector_map(_klass->interpreter_itable_selector_addr());

  uint32_t* blob_int32 = (uint32_t*)_klass->start_of_itable();
  tableEntry* blob_table = _klass->itable_table();
  uint32_t mask = *blob_int32;
  uint32_t size = mask + 1;
  if (mask == 0) {
    return;
  }

  for (uint32_t i = 0; i < size; ++i) {
    tableEntry entry = blob_table[i];
    if (entry.selector() == 0) {
      continue;
    }
    // Get Method* associated with selected method in the itable (so use both tables)
    Method* method = method_selector_map.get(itable_selector_map.get(entry.selector()));
    blob_table[i] = make_itable_entry(entry.selector(), method);
  }
}

// Copy the selector map into metadata
void klassItable::allocate_interpreter_itable(uint8_t** _itable_selector_map_blob, TRAPS) {

  SelectorMap<uint32_t> selector_map(_itable_selector_map_blob);

  int length = 2 + selector_map.capacity() * 2;  // capacity, mask, refc[capacity], selc[capacity]
  Array<uint32_t>* interpreter_itable =
      MetadataFactory::new_array<uint32_t>(_klass->class_loader_data(), length, CHECK);
  memcpy((void*)interpreter_itable->adr_at(0), (void*)selector_map.size_addr(), length * sizeof(uint32_t));
  _klass->set_interpreter_itable(interpreter_itable);

#ifdef ASSERT
  int j = 2; // the size and mask field are first, but private, so don't verify them.
  int interpreter_itable_length = selector_map.capacity();
  for (int i = 0; i < interpreter_itable_length; i++, j++) {
    uint32_t selector = selector_map.selector_table()[i];
    assert(selector == interpreter_itable->at(j), "must match");
  }
  for (int i = 0; i < interpreter_itable_length; i++, j++) {
    uint32_t value = selector_map.value_table()[i];
    assert(value == interpreter_itable->at(j), "must match");
  }
#endif
  selector_map.free_blob();
}

static int initialize_count = 0;

// Initialization
void klassItable::initialize_itable(bool checkconstraints, TRAPS) {
  ResourceMark rm;
  // Cannot be setup doing bootstrapping, interfaces don't have
  // itables, and klass with only ones entry have empty itables
  if (Universe::is_bootstrapping() ||
      _klass->is_interface() ||
      _klass->is_abstract() ||
      !_klass->has_itable()) {
    return;
  }

  Array<uint32_t>* interpreter_itable = _klass->interpreter_itable();

  // Some classes initialize the itable twice, and shared classes have already initialized
  // the interpreter itable, as it is read-only.
  if (interpreter_itable == NULL) {

    // This creates the interpreter itable, and initializes it into a temporary blob.
    uint8_t* _itable_selector_map_blob = NULL;
    SelectorMap<uint32_t> selector_map(&_itable_selector_map_blob);

    Array<InstanceKlass*>* transitive_interfaces = _klass->transitive_interfaces();
    int num_interfaces = transitive_interfaces->length();
    if (num_interfaces > 0) {
      ResourceMark rm(THREAD);
      log_develop_debug(itables)("%3d: Initializing itables for %s", ++initialize_count,
                         _klass->name()->as_C_string());

      // Iterate through all interfaces
      for (int i = 0; i < num_interfaces; i++) {
        HandleMark hm(THREAD);
        InstanceKlass *interf = transitive_interfaces->at(i);
        initialize_itable_for_interface(interf, &_itable_selector_map_blob, checkconstraints, CHECK);
      }
    }

    // Copy the blob into metadata and free
    allocate_interpreter_itable(&_itable_selector_map_blob, CHECK);
  }

  // This fills in the compiled code itable, whose size is already calculated.
  uint32_t* blob_int32 = (uint32_t*)_klass->start_of_itable();
  tableEntry* blob_table = _klass->itable_table();
  // Still add a sentinel entry for catching incorrect calls.
  blob_int32[0] = 0; // mask
  blob_int32[1] = 0; // padding
  blob_table[0] = make_itable_entry(0, NULL);

  itableHashTableBuilder builder(_klass);
  MutexLocker pl(CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
  builder.create_itable();
}


bool klassItable::interface_method_needs_itable_index(Method* m) {
  if (m->is_static())           return false;   // e.g., Stream.empty
  if (m->is_initializer())      return false;   // <init> or <clinit>
  if (m->is_private())          return false;   // uses direct call
  if (m->is_final())            return false;   // uses direct call
  // If an interface redeclares a method from java.lang.Object,
  // it should already have a vtable index, don't touch it.
  // e.g., CharSequence.toString (from initialize_vtable)
  //if (m->has_vtable_index())  return false; // NO!...?
  return true;
}

void itableHashTableBuilder::populate_table() {
  for (;;) {
    _random = _seed;
    bool success = true;
    int num_ifs = _transitive_interfaces->length();
    for (int j = 0; success && j < num_ifs; j++) {
      InstanceKlass *interf = _transitive_interfaces->at(j);

      assert(interf->is_interface(), "must be");
      Array<Method*>* methods = interf->methods();
      int nof_methods = methods->length();

      for (int i = 0; success && i < nof_methods; i++) {
        Method* m = methods->at(i);
        if (klassItable::interface_method_needs_itable_index(m)) {
          bool inserted;
          if (_resolve_methods) {
            Method* target = _ik->itable().target_method_for_selector(m->selector());
            inserted = set(m->selector(), target);
          } else {
            // Just self-loop the method to denote something will happen here
            inserted = set(m->selector(), m);
          }
          if (!inserted) {
            resize(_capacity << 1);
            success = false;
          }
        }
      }
    }
    if (success) {
      return;
    }
  }
}

void klassItable::initialize_itable_for_interface(InstanceKlass* interf,
                                                  uint8_t** _itable_selector_map_blob,
                                                  bool checkconstraints, TRAPS) {
  assert(interf->is_interface(), "must be");
  Array<Method*>* methods = interf->methods();
  int nof_methods = methods->length();
  HandleMark hm;
  Handle interface_loader (THREAD, interf->class_loader());

  SelectorMap<uint32_t> selector_map(_itable_selector_map_blob);

  for (int i = 0; i < nof_methods; i++) {
    Method* m = methods->at(i);
    Method* target = NULL;
    if (!klassItable::interface_method_needs_itable_index(m)) {
      continue;
    }
    // This search must match the runtime resolution, i.e. selection search for invokeinterface
    // to correctly enforce loader constraints for interface method inheritance.
    // Private methods are skipped as a private class method can never be the implementation
    // of an interface method.
    // Invokespecial does not perform selection based on the receiver, so it does not use
    // the cached itable.
    target = LinkResolver::lookup_instance_method_in_klasses(_klass, m->name(), m->signature(),
                                                             Klass::skip_private, CHECK);
    if (target == NULL) {
      continue;
    }
    if (!target->is_public() || target->is_abstract() || target->is_overpass()) {
      assert(!target->is_overpass() || target->is_public(),
             "Non-public overpass method!");
      // Entry does not resolve.
      if (!target->is_public()) {
        // Stuff an IllegalAccessError throwing method in there instead.
        selector_map.set(m->selector(), Universe::throw_illegal_access_error()->selector());
      } else if (target->is_abstract()) {
        selector_map.set(m->selector(), Universe::throw_abstract_method_error()->selector());
      } else {
        selector_map.set(m->selector(), target->selector());
      }
    } else {
      // Entry did resolve, check loader constraints before initializing
      // if checkconstraints requested
      if (checkconstraints) {
        Handle method_holder_loader (THREAD, target->method_holder()->class_loader());
        InstanceKlass* method_holder = target->method_holder();
        if (method_holder_loader() != interface_loader()) {
          ResourceMark rm(THREAD);
          Symbol* failed_type_symbol =
            SystemDictionary::check_signature_loaders(m->signature(),
                                                      _klass,
                                                      method_holder_loader,
                                                      interface_loader,
                                                      true, CHECK);
          if (failed_type_symbol != NULL) {
            stringStream ss;
            ss.print("loader constraint violation in interface itable"
                     " initialization for class %s: when selecting method '",
                     _klass->external_name());
            m->print_external_name(&ss),
            ss.print("' the class loader %s for super interface %s, and the class"
                     " loader %s of the selected method's %s, %s have"
                     " different Class objects for the type %s used in the signature (%s; %s)",
                     interf->class_loader_data()->loader_name_and_id(),
                     interf->external_name(),
                     method_holder->class_loader_data()->loader_name_and_id(),
                     method_holder->external_kind(),
                     method_holder->external_name(),
                     failed_type_symbol->as_klass_external_name(),
                     interf->class_in_module_of_loader(false, true),
                     method_holder->class_in_module_of_loader(false, true));
            THROW_MSG(vmSymbols::java_lang_LinkageError(), ss.as_string());
          }
        }
      }

      selector_map.set(m->selector(), target->selector());
      if (log_develop_is_enabled(Trace, itables)) {
        ResourceMark rm(THREAD);
        if (target != NULL) {
          LogTarget(Trace, itables) lt;
          LogStream ls(lt);
          char* sig = target->name_and_sig_as_C_string();
          ls.print("interface: %s, target: %s, method_holder: %s ",
                       interf->internal_name(), sig,
                       target->method_holder()->internal_name());
          ls.print("target_method flags: ");
          target->print_linkage_flags(&ls);
          ls.cr();
        }
      }
    }
  }
}

int klassItable::compute_itable_size_words(uint32_t seed, Array<InstanceKlass*>* transitive_interfaces) {
  // This stinks that we have to compute this twice
  itableHashTableBuilder itable(seed, transitive_interfaces);
  return (int)itable.compute_itable_size_words();
}

void klassVtable::verify(outputStream* st, bool forced) {
  // make sure table is initialized
  if (!Universe::is_fully_initialized()) return;
#ifndef PRODUCT
  // avoid redundant verifies
  if (!forced && _verify_count == Universe::verify_count()) return;
  _verify_count = Universe::verify_count();
#endif

  for (int i = 0; i < _length; i++) _table[-i].verify(this, st);
  // verify consistency with superKlass vtable
  Klass* super = _klass->super();
  if (super != NULL) {
    klassVtable vt = super->vtable();
    for (int i = 0; i < vt.length(); i++) {
      verify_against(st, &vt, i);
    }
  }
}

void klassVtable::verify_against(outputStream* st, klassVtable* vt, int index) {
  tableEntry* vte = &vt->_table[-index];
  if (vte->method()->name()      != _table[-index].method()->name() ||
      vte->method()->signature() != _table[-index].method()->signature()) {
    fatal("mismatched name/signature of vtable entries");
  }
}

#ifndef PRODUCT
void klassVtable::print() const { print_on(tty); }

void klassVtable::print_on(outputStream* st) const {
  st->print("klassVtable for klass %s (length %d):\n", _klass->internal_name(), length());
  for (int i = 0; i < length(); i++) {
    _table[-i].print_on(st);
    st->cr();
  }
}


void klassItable::print() const { print_on(tty); }

void klassItable::print_on(outputStream* st) const {
  // itable_length() includes the header
  int itable_entry_words = _klass->itable_length() - (int)itable_header_size_words();

  st->print_cr("klassItable for klass %s (length %d):", _klass->internal_name(), itable_entry_words);

  SelectorMap<uint32_t> itable_selector_map(_klass->interpreter_itable_selector_addr());
  SelectorMap<Method*> method_map = SystemDictionary::method_selector_map();

  st->print_cr(" - interpreter itable:");
  int interpreter_itable_length = itable_selector_map.capacity();
  for (int i = 0; i < interpreter_itable_length; i++) {
    int32_t selector = itable_selector_map.selector_table()[i];
    Method* method = method_map.get(selector);
    st->print_cr("DEFC method %d %s", selector, method == NULL ? "NULL" : method->external_name());
  }
  for (int i = 0; i < interpreter_itable_length; i++) {
    int32_t value = itable_selector_map.value_table()[i];
    Method* method = method_map.get(value);
    st->print_cr("SELC method %d %s", value, method == NULL ? "NULL" : method->external_name());
  }

  st->print_cr(" - compiler itable:");
  // Print the itable appended to the InstanceKlass
  tableEntry* blob_table = _klass->itable_table();

  for (int i = 0; i < itable_entry_words; i++) {
    blob_table[i].print_on(st);
    st->cr();
  }
}
#endif

uint32_t tableEntry::selector() const {
  uint64_t selector_mask = ~uint64_t(0u) >> 32;
  return uint32_t(_entry & selector_mask);
}

address tableEntry::code() const {
  uint64_t code_64 = _entry >> (32 - CodeCache::code_pointer_shift());
  uintptr_t code_intptr = static_cast<uintptr_t>(code_64);

  if (!CodeCache::supports_32_bit_code_pointers()) {
    uintptr_t code_base = (uintptr_t)CodeCache::low_bound();
    code_intptr += code_base;
  }

  return reinterpret_cast<address>(code_intptr);
}

Method* tableEntry::method() const {
  SelectorMap<Method*> method_map = SystemDictionary::method_selector_map();
  return method_map.get(selector());
}

void tableEntry::verify(klassVtable* vt, outputStream* st) {
  Klass* vtklass = vt->klass();
  Method* m = method();
  if (vtklass->is_instance_klass() &&
     (InstanceKlass::cast(vtklass)->major_version() >= klassVtable::VTABLE_TRANSITIVE_OVERRIDE_VERSION)) {
    assert(m != NULL, "must have set method");
  }
  if (m != NULL) {
    m->verify();
    // we sub_type, because it could be a miranda method
    if (!vtklass->is_subtype_of(m->method_holder())) {
#ifndef PRODUCT
      ResourceMark rm;
      print_on(st);
#endif
      fatal("tableEntry " PTR_FORMAT ": method is from subclass", p2i(this));
    }
  }
}

#ifndef PRODUCT

void tableEntry::print() const { print_on(tty); }

void tableEntry::print_on(outputStream* st) const {
  st->print("tableEntry %s:    " PTR_FORMAT, method() == NULL ? "NULL" : method()->external_name(), p2i(code()));
}

class VtableStats : AllStatic {
 public:
  static int no_klasses;                // # classes with vtables
  static int no_array_klasses;          // # array classes
  static int no_instance_klasses;       // # instanceKlasses
  static int sum_of_vtable_len;         // total # of vtable entries
  static int sum_of_array_vtable_len;   // total # of vtable entries in array klasses only
  static int fixed;                     // total fixed overhead in bytes
  static int filler;                    // overhead caused by filler bytes
  static int entries;                   // total bytes consumed by vtable entries
  static int array_entries;             // total bytes consumed by array vtable entries

  static void do_class(Klass* k) {
    Klass* kl = k;
    klassVtable vt = kl->vtable();
    no_klasses++;
    if (kl->is_instance_klass()) {
      no_instance_klasses++;
      kl->array_klasses_do(do_class);
    }
    if (kl->is_array_klass()) {
      no_array_klasses++;
      sum_of_array_vtable_len += vt.length();
    }
    sum_of_vtable_len += vt.length();
  }

  static void compute() {
    LockedClassesDo locked_do_class(&do_class);
    ClassLoaderDataGraph::classes_do(&locked_do_class);
    fixed  = no_klasses * oopSize;      // vtable length
    // filler size is a conservative approximation
    filler = oopSize * (no_klasses - no_instance_klasses) * (sizeof(InstanceKlass) - sizeof(ArrayKlass) - 1);
    entries = sizeof(tableEntry) * sum_of_vtable_len;
    array_entries = sizeof(tableEntry) * sum_of_array_vtable_len;
  }
};

int VtableStats::no_klasses = 0;
int VtableStats::no_array_klasses = 0;
int VtableStats::no_instance_klasses = 0;
int VtableStats::sum_of_vtable_len = 0;
int VtableStats::sum_of_array_vtable_len = 0;
int VtableStats::fixed = 0;
int VtableStats::filler = 0;
int VtableStats::entries = 0;
int VtableStats::array_entries = 0;

void klassVtable::print_statistics() {
  ResourceMark rm;
  HandleMark hm;
  VtableStats::compute();
  tty->print_cr("vtable statistics:");
  tty->print_cr("%6d classes (%d instance, %d array)", VtableStats::no_klasses, VtableStats::no_instance_klasses, VtableStats::no_array_klasses);
  int total = VtableStats::fixed + VtableStats::filler + VtableStats::entries;
  tty->print_cr("%6d bytes fixed overhead (refs + vtable object header)", VtableStats::fixed);
  tty->print_cr("%6d bytes filler overhead", VtableStats::filler);
  tty->print_cr("%6d bytes for vtable entries (%d for arrays)", VtableStats::entries, VtableStats::array_entries);
  tty->print_cr("%6d bytes total", total);
}

int  klassItable::_total_classes;   // Total no. of classes with itables
long klassItable::_total_size;      // Total no. of bytes used for itables

void klassItable::print_statistics() {
 tty->print_cr("itable statistics:");
 tty->print_cr("%6d classes with itables", _total_classes);
 tty->print_cr("%6lu K uses for itables (average by class: %ld bytes)", _total_size / K, _total_size / _total_classes);
}

#endif // PRODUCT

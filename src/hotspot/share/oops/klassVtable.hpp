/*
 * Copyright (c) 1997, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_KLASSVTABLE_HPP
#define SHARE_OOPS_KLASSVTABLE_HPP

#include "runtime/handles.hpp"
#include "utilities/growableArray.hpp"

// A klassVtable abstracts the variable-length vtable that is embedded in InstanceKlass
// and ArrayKlass.  klassVtable objects are used just as convenient transient accessors to the vtable,
// not to actually hold the vtable data.
// Note: the klassVtable should not be accessed before the class has been verified
// (until that point, the vtable is uninitialized).

// Currently a klassVtable contains a direct reference to the vtable data, and is therefore
// not preserved across GCs.

class klassVtable;
class outputStream;
template <typename V> class SelectorMap;

struct tableEntry {
  uint64_t _entry;

  Method* method() const;
  void print() const;
  void print_on(outputStream* st) const;
  void verify(klassVtable* vt, outputStream* st);

  uint32_t selector() const;
  address code() const;

  static int selector_offset_in_bytes() { return 4; }
  static address table_entry_code(Method* method, bool is_itable);
};

class klassVtable {
  Klass*      _klass;            // my klass
  tableEntry* _table;
  Method**    _scratch_table;
  int         _length;
#ifndef PRODUCT
  int         _verify_count;     // to make verify faster
#endif

  // Ordering important, so greater_than (>) can be used as an merge operator.
  enum AccessType {
    acc_private         = 0,
    acc_package_private = 1,
    acc_publicprotected = 2
  };

  int* length_addr() const;
  void copy_vtable_to(klassVtable* dst);
  static size_t blob_size_words(uint32_t length);
  void link_code(bool bootstrapping);
  Method** scratch_table();

 public:
  klassVtable(Klass* klass)
    : _klass(klass),
      _table(((tableEntry*)klass) - 2),
      _scratch_table(NULL),
      _length(klass->vtable_length() / (sizeof(tableEntry) / wordSize) - 1)
  { }

  // accessors
  Klass* klass() const            { return _klass;  }
  tableEntry entry_at(int i) const;
  Method* method_at(int i) const;
  Method* unchecked_method_at(int i) const;

  void remove_unshareable_info();

  // flat vtable support
  int length() const;
  void link_table_code();
  bool link_code(Method* method);
  void link_code(int vtable_index, Method* method);

  // searching; all methods return -1 if not found
  int index_of_miranda(Symbol* name, Symbol* signature);

  void initialize_vtable(bool checkconstraints, TRAPS);   // initialize vtable of a new klass

  // computes vtable length (in words) and the number of miranda methods
  static void compute_vtable_size_and_num_mirandas(int* vtable_length,
                                                   int* num_new_mirandas,
                                                   GrowableArray<Method*>* all_mirandas,
                                                   const Klass* super,
                                                   Array<Method*>* methods,
                                                   AccessFlags class_flags,
                                                   u2 major_version,
                                                   Handle classloader,
                                                   Symbol* classname,
                                                   Array<InstanceKlass*>* local_interfaces,
                                                   TRAPS);

  void clear();

  // Debugging code
  void print() const                                        PRODUCT_RETURN;
  void print_on(outputStream* st) const                     PRODUCT_RETURN;
  void verify(outputStream* st, bool force = false);
  static void print_statistics()                            PRODUCT_RETURN;

 protected:
  friend struct tableEntry;

 public:
  // Transitive overridng rules for class files < JDK1_7 use the older JVMS rules.
  // Overriding is determined as we create the vtable, so we use the class file version
  // of the class whose vtable we are calculating.
  enum { VTABLE_TRANSITIVE_OVERRIDE_VERSION = 51 } ;

 private:
  int  initialize_from_super(Klass* super);
  void put_method_at(Method* m, int index);
  static bool needs_new_vtable_entry(const methodHandle& m,
                                     const Klass* super,
                                     Handle classloader,
                                     Symbol* classname,
                                     AccessFlags access_flags,
                                     u2 major_version,
                                     TRAPS);

  bool update_inherited_vtable(InstanceKlass* klass, const methodHandle& target_method, int super_vtable_len, int default_index, bool checkconstraints, TRAPS);
 InstanceKlass* find_transitive_override(InstanceKlass* initialsuper, const methodHandle& target_method, int vtable_index,
                                         Handle target_loader, Symbol* target_classname, Thread* THREAD);

  // support for miranda methods
  bool is_miranda_entry_at(int i);
  int fill_in_mirandas(int initialized, TRAPS);
  static bool is_miranda(Method* m, Array<Method*>* class_methods,
                         Array<Method*>* default_methods, const Klass* super,
                         bool is_interface);
  static void add_new_mirandas_to_lists(
      GrowableArray<Method*>* new_mirandas,
      GrowableArray<Method*>* all_mirandas,
      Array<Method*>* current_interface_methods,
      Array<Method*>* class_methods,
      Array<Method*>* default_methods,
      const Klass* super,
      bool is_interface);
  static void get_mirandas(
      GrowableArray<Method*>* new_mirandas,
      GrowableArray<Method*>* all_mirandas,
      const Klass* super,
      Array<Method*>* class_methods,
      Array<Method*>* default_methods,
      Array<InstanceKlass*>* local_interfaces,
      bool is_interface);
  void verify_against(outputStream* st, klassVtable* vt, int index);
  inline InstanceKlass* ik() const;
  // When loading a class from CDS archive at run time, and no class redefintion
  // has happened, it is expected that the class's itable/vtables are
  // laid out exactly the same way as they had been during dump time.
  // Therefore, in klassVtable::initialize_[iv]table, we do not layout the
  // tables again. Instead, we only rerun the process to create/check
  // the class loader constraints. In non-product builds, we add asserts to
  // guarantee that the table's layout would be the same as at dump time.
  //
  // If JVMTI redefines any class, the read-only shared memory are remapped
  // as read-write. A shared class' vtable/itable are re-initialized and
  // might have different layout due to class redefinition of the shared class
  // or its super types.
  bool is_preinitialized_vtable();
};

class klassItable {
  friend class itableHashTableBuilder;
 private:
  static const size_t _empty_table_size_words = 1 + sizeof(tableEntry) / wordSize;
  InstanceKlass* _klass;

  void initialize_itable_for_interface(InstanceKlass* interf_h,
                                       uint8_t** _itable_selector_map_blob,
                                       bool checkconstraints, TRAPS);

  static uint32_t itable_header_size_bytes() { return 8; }
 public:
  klassItable(InstanceKlass* klass);

  static bool interface_method_needs_itable_index(Method* m);

  uint32_t target_selector_for_selector(uint32_t selector);
  Method* target_method_for_selector(uint32_t selector);

  static int compute_itable_size_words(uint32_t seed, Array<InstanceKlass*>* transitive_interfaces);

  // Layout in InstanceKlass
  static size_t itable_header_size_words() { return itable_header_size_bytes() / wordSize; }
  static ByteSize itable_table_offset()    { return in_ByteSize(itable_header_size_bytes()); }

  // Initialization
  void initialize_itable(bool checkconstraints, TRAPS);
  void allocate_interpreter_itable(uint8_t** _itable_selector_map_blob, TRAPS);

  void link_table_code();
  void link_code(Method* method);

  void remove_unshareable_info();

  // Debugging/Statistics
  static void print_statistics() PRODUCT_RETURN;

  void print() const                      PRODUCT_RETURN;
  void print_on(outputStream* st) const   PRODUCT_RETURN;
 private:

  // Statistics
  NOT_PRODUCT(static int  _total_classes;)   // Total no. of classes with itables
  NOT_PRODUCT(static long _total_size;)      // Total no. of bytes used for itables

  static void update_stats(int size) PRODUCT_RETURN NOT_PRODUCT({ _total_classes++; _total_size += size; })
};

#endif // SHARE_OOPS_KLASSVTABLE_HPP

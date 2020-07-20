/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_SELECTORMAP_HPP
#define SHARE_OOPS_SELECTORMAP_HPP

#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"

// --------------------------------------------------------------------------------
// Memory layout of a selector map blob:
//            ----------------
//           |   purge_next   |
//            ----------------
//           |  next_version  |
//            ----------------
//           | size  |  mask  |
// blob --->  ----------------
//           | selector table |
//           |                |
//            ----------------
//           |   value table  |
//           |                |
//            ----------------
template<typename V>
class SelectorMap {
public:
  class EntryBoolClosure;

private:
  uint8_t* volatile* _blob_ptr;
  uint8_t* volatile* _free_list_ptr;
  uint8_t*           _blob;
  uint32_t           _capacity;
  uint32_t           _initial_size;
  uint32_t volatile* _selector_table;  // resolved method selector
  V volatile*        _value_table;     // selected method selector
  EntryBoolClosure*  _is_alive;

  static const ptrdiff_t _mask_blob_offset = -4;
  static const ptrdiff_t _size_blob_offset = _mask_blob_offset - 4;
  static const ptrdiff_t _next_version_blob_offset = _size_blob_offset - (ptrdiff_t)sizeof(void*);
  static const ptrdiff_t _purge_next_blob_offset = _next_version_blob_offset - (ptrdiff_t)sizeof(void*);
  static const size_t    _blob_header_size = size_t(-_size_blob_offset);
  static const size_t    _concurrent_blob_header_size = size_t(-_purge_next_blob_offset);
  static const uint32_t  _invalid_selector_sentinel = 0xFFFFFFFF;
  static const uint32_t  _target_residency_percent = 90;

  // blob accessors
  static uint32_t size_from_blob(uint8_t* blob) {
    return Atomic::load((uint32_t volatile*)(blob + _size_blob_offset));
  }

  static void inc_size_for_blob(uint8_t* blob) {
    Atomic::inc((uint32_t volatile*)(blob + _size_blob_offset));
  }

  static uint8_t* next_version_for_blob(uint8_t* blob) {
    return Atomic::load_acquire((uint8_t* volatile*)(blob + _next_version_blob_offset));
  }

  static bool try_set_next_version_for_blob(uint8_t* blob, uint8_t* new_version_blob) {
    return Atomic::replace_if_null((uint8_t* volatile*)(blob + _next_version_blob_offset), new_version_blob);
  }

  static uint8_t* purge_next_for_blob(uint8_t* blob) {
    return Atomic::load((uint8_t* volatile*)(blob + _purge_next_blob_offset));
  }

  static void set_purge_next_for_blob(uint8_t* blob, uint8_t* purge_next_blob) {
    Atomic::store((uint8_t* volatile*)(blob + _purge_next_blob_offset), purge_next_blob);
  }

  static uint32_t capacity_from_blob(uint8_t* blob) {
    return 1 + *(uint32_t*)(blob + _mask_blob_offset);
  }

  static uint32_t volatile* selector_table_from_blob(uint8_t* blob) {
    assert(blob != NULL, "must not be null");
    return (uint32_t volatile*)blob;
  }

  static V volatile* value_table_from_blob(uint8_t* blob) {
    assert(blob != NULL, "must not be null");
    uint32_t capacity = capacity_from_blob(blob);
    return (V*)(((uint32_t*)blob) + capacity);
  }

  uint32_t size() {
    return size_from_blob(_blob);
  }

  void attach_to_blob(uint8_t* blob) {
    assert(blob != NULL, "must not be null");
    _blob = blob;
    _capacity = capacity_from_blob(blob);
    _selector_table = selector_table_from_blob(blob);
    _value_table = value_table_from_blob(blob);
  }

  uint32_t mask() const {
    return _capacity - 1;
  }

  bool should_rebuild() {
    if ((size() + 1) * 100 / _capacity > _target_residency_percent) {
      return true;
    }
    return false;
  }

  bool is_concurrent() const {
    return _free_list_ptr != NULL;
  }

  void initialize() {
    attach_to_blob(create_blob(_initial_size));
    Atomic::release_store(_blob_ptr, _blob);
  }

  uint8_t* create_blob(uint32_t capacity);

  // After freezing, inserts will fail and re-try in a newer version.
  void freeze_table(uint32_t volatile* selector_table, uint32_t capacity);
  void finish_rebuild(uint32_t volatile* selector_table, uint32_t capacity);
  void update_blob_ptr();
  uint32_t calculate_dead_selectors();
  uint32_t calculate_new_table_capacity();
  void rebuild();
  void destroy(uint8_t* blob, uint8_t* volatile* free_list_ptr);
  V volatile* get_impl(uint32_t selector);

public:
  SelectorMap(uint8_t* volatile* blob_ptr,
              uint8_t* volatile* free_list_ptr = NULL,
              EntryBoolClosure* is_alive = NULL,
              uint32_t initial_size = 2)
    : _blob_ptr(blob_ptr),
      _free_list_ptr(free_list_ptr),
      _blob(NULL),
      _capacity(0),
      _initial_size(initial_size),
      _selector_table(NULL),
      _value_table(NULL),
      _is_alive(is_alive)
  {
    uint8_t* blob = Atomic::load_acquire(blob_ptr);
    if (blob == NULL) {
      initialize();
    } else {
      attach_to_blob(blob);
    }
  }

  SelectorMap(uint8_t* blob)
    : _blob_ptr(NULL),
      _free_list_ptr(NULL),
      _blob(NULL),
      _capacity(0),
      _initial_size(0),
      _selector_table(NULL),
      _value_table(NULL),
      _is_alive(NULL)
  {
    // There is no blob_ptr. This table is never rebuilt.
    assert (blob != NULL, "Attach to existing blob.");
    attach_to_blob(blob);
  }

  class EntryBoolClosure: public Closure {
  public:
    virtual bool do_entry_b(uint32_t selector, V value) = 0;
  };

  // New closure for cleaning out unshareable entries for CDS.
  void set_alive_closure(EntryBoolClosure* is_alive) { _is_alive = is_alive; }

  uint32_t* size_addr() {
    return (uint32_t *)(_blob + _size_blob_offset);
  }

  uint8_t* unlink();
  static void purge(uint8_t* free_list);

  uint8_t* blob() {
    return _blob;
  }

  uint32_t volatile* selector_table() {
    return _selector_table;
  }

  uint32_t capacity() {
    return _capacity;
  }

  bool set(uint32_t selector, V value);

  bool contains(uint32_t selector) {
    return get_impl(selector) != NULL;
  }

  bool contains_value(V value);

  V get(uint32_t selector) {
    return Atomic::load(get_impl(selector));
  }

  V volatile* try_get(uint32_t selector) {
    return get_impl(selector);
  }

  void remap(uint32_t selector, V value) {
    for (;;) {
      V volatile* addr = get_impl(selector);
      if (addr != NULL) {
        Atomic::store(addr, value);
      }
      OrderAccess::storeload();
      if (is_concurrent() && next_version_for_blob(_blob)) {
        attach_to_blob(next_version_for_blob(_blob));
      } else {
        break;
      }
    }
  }

  V* value_table() {
    return const_cast<V*>(_value_table);
  }

  void free_blob();
};

#endif // SHARE_OOPS_SELECTORMAP_HPP

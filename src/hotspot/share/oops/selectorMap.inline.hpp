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

#ifndef SHARE_OOPS_SELECTORMAP_INLINE_HPP
#define SHARE_OOPS_SELECTORMAP_INLINE_HPP

#include "oops/selectorMap.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/powerOfTwo.hpp"

template <typename V>
uint8_t* SelectorMap<V>::create_blob(uint32_t capacity) {
  size_t header_size = is_concurrent() ? _concurrent_blob_header_size : _blob_header_size;
  size_t blob_size = header_size + capacity * sizeof(uint32_t) + capacity * sizeof(V);
  uint8_t* blob = NEW_C_HEAP_ARRAY(uint8_t, blob_size, mtClass);
  memset(blob, 0, blob_size);
  blob += header_size;
  ((uint32_t*)blob)[-1] = capacity - 1; // mask
  assert(capacity_from_blob(blob) == capacity, "must be");
  return blob;
}

template <typename V>
void SelectorMap<V>::free_blob() {
  assert(!is_concurrent(), "Not concurrent");
  uint8_t* blob = _blob - _blob_header_size;
  FREE_C_HEAP_ARRAY(uint8_t, blob);
}

// After freezing, inserts will fail and re-try in a newer version.
template <typename V>
void SelectorMap<V>::freeze_table(uint32_t volatile* selector_table, uint32_t capacity) {
  if (!is_concurrent()) {
    return;
  }
  for (uint32_t volatile* curr = selector_table; curr != selector_table + capacity; ++curr)  {
    uint32_t selector = Atomic::load(curr);
    if (selector == 0) {
      Atomic::cmpxchg(curr, 0u, _invalid_selector_sentinel, memory_order_relaxed);
    }
  }
}

template <typename V>
void SelectorMap<V>::finish_rebuild(uint32_t volatile* selector_table, uint32_t capacity) {
  uint32_t index = 0;
  V volatile* value_table = reinterpret_cast<V volatile*>(selector_table + capacity);
  for (uint32_t volatile* curr = selector_table; curr != selector_table + capacity; ++curr, ++index)  {
    uint32_t selector = Atomic::load(curr);
    if (selector == 0 || selector == _invalid_selector_sentinel) {
      continue;
    }

    V dead_value = 0;
    V value = Atomic::load(&value_table[index]);
    if (value == dead_value) {
      // Make the acquire conditional as this is a rare case.
      OrderAccess::acquire();
      if (Atomic::load(&value_table[index]) == dead_value) {
        // The entry is dead because the value is deleted.
        continue;
      }
    }
    if (_is_alive != NULL && !_is_alive->do_entry_b(selector, value)) {
      // The entry is dead because it is not alive.
      continue;
    }

    // Relocate selector to the next table. This can't fail, because relocation
    // is guaranteed to fit, and inserts can't continue without finishing rebuilding.
    uint32_t index = selector & mask();
    uint32_t start_index = index;
    do {
      // Try to insert the selector
      uint32_t prev_selector = Atomic::load(&_selector_table[index]);
      if (prev_selector == selector) {
        break;
      }
      if (prev_selector == 0) {
        // Good candidate bucket for insert, let's try it.
        V expected_value = 0;
        Atomic::cmpxchg(&_value_table[index], expected_value, value);
        if (Atomic::cmpxchg(&_selector_table[index], prev_selector, selector) == prev_selector) {
          inc_size_for_blob(_blob);
        }
        break;
      }
      index = (index + 1) & mask();
    } while(index != start_index);
  }
}

template <typename V>
void SelectorMap<V>::update_blob_ptr() {
  if (is_concurrent()) {
    // Check for monotonicity of _blob_ptr update.
    uint8_t* top = Atomic::load(_blob_ptr);
    if (top == _blob) {
      // Already updated by a different thread.
      return;
    }
    for (uint8_t* current = top;
         current != _blob;
         current = next_version_for_blob(current)) {
      if (current == NULL) {
        // If we reach the end of the list, another thread updated the blob_ptr
        // to an even newer blob.
        return;
      }
    }
    // Reroute reads once relocation to the new table has completed.
    Atomic::cmpxchg(_blob_ptr, top, _blob);
  } else {
    *_blob_ptr = _blob;
  }
}

template <typename V>
uint32_t SelectorMap<V>::calculate_dead_selectors() {
  if (_is_alive == NULL) {
    return 0;
  }
  // Count dead entries
  uint32_t dead = 0;
  V dead_value = 0;
  for (uint32_t i = 0; i < _capacity; ++i) {
    uint32_t selector = Atomic::load(&_selector_table[i]);
    if (selector == 0 || selector == _invalid_selector_sentinel) {
      continue;
    }
    V value = Atomic::load(&_value_table[i]);
    if (value == dead_value) {
      // Make the the acquire conditional as this is a rare case.
      OrderAccess::acquire();
      if (Atomic::load(&_value_table[i]) == dead_value) {
        // The entry is dead because the value is deleted.
        ++dead;
        continue;
      }
    }
    if (!_is_alive->do_entry_b(selector, value)) {
      // The entry is dead because it is not alive.
      ++dead;
    }
  }
  return dead;
}

template <typename V>
uint32_t SelectorMap<V>::calculate_new_table_capacity() {
  uint32_t dead = calculate_dead_selectors();
  uint32_t size = size_from_blob(_blob);

  if (dead != 0) {
    // Try sizing table to something reasonable after things died.
    return MAX2(round_up_power_of_2((size - dead) * 100 / _target_residency_percent), 2u);
  } else {
    // Otherwise double the size.
    return _capacity << 1;
  }
}

template <typename V>
void SelectorMap<V>::rebuild() {
  uint32_t volatile* selector_table = _selector_table;
  uint8_t* old_blob = _blob;
  uint32_t old_capacity = _capacity;
  uint8_t* new_blob = NULL;
  uint8_t* obsolete_blob = NULL;
  uint8_t* volatile* obsolete_free_list;

  if (is_concurrent()) {
    new_blob = next_version_for_blob(old_blob);
  }

  if (new_blob == NULL) {
    new_blob = create_blob(calculate_new_table_capacity());

    if (old_blob != NULL) {
      if (!is_concurrent() || try_set_next_version_for_blob(old_blob, new_blob)) {
        obsolete_blob = old_blob;
        obsolete_free_list = _free_list_ptr;
      } else {
        // We lost concurrent destroying; someone else initiated table rebuilding; the new table is garbage now.
        obsolete_blob = new_blob;
        obsolete_free_list = NULL;
        new_blob = next_version_for_blob(_blob);
      }
    }
  }

  attach_to_blob(new_blob);
  freeze_table(selector_table, old_capacity);
  finish_rebuild(selector_table, old_capacity);

  update_blob_ptr();

  if (obsolete_blob != NULL) {
    destroy(obsolete_blob, obsolete_free_list);
  }
}

template <typename V>
void SelectorMap<V>::destroy(uint8_t* blob, uint8_t* volatile* free_list_ptr) {
  if (free_list_ptr == NULL) {
    uint32_t blob_header_size = is_concurrent() ? _concurrent_blob_header_size : _blob_header_size;
    blob -= blob_header_size;
    FREE_C_HEAP_ARRAY(uint8_t, blob);
  } else {
    // Defer free until it is safe. Must be after thread-local handshake or safepoint.
    for (;;) {
      uint8_t* free_list_head = Atomic::load(free_list_ptr);
      set_purge_next_for_blob(blob, free_list_head);
      if (Atomic::cmpxchg(free_list_ptr, free_list_head, blob) == free_list_head) {
        return;
      }
    }
  }
}

template <typename V>
V volatile* SelectorMap<V>::get_impl(uint32_t selector) {
  for (;;) {
    uint32_t index = selector & mask();
    uint32_t start_index = index;
    do {
      uint32_t current = Atomic::load(&_selector_table[index]);
      if (current == selector) {
        OrderAccess::acquire();
        return &_value_table[index];
      }
      if (current == 0) {
        break;
      }
      index = (index + 1) & mask();
    } while (index != start_index);
    // Didn't find any entry. Check if table is being rebuilt.
    if (is_concurrent()) {
      OrderAccess::acquire();
      uint8_t* blob = next_version_for_blob(_blob);
      if (blob != NULL) {
        uint32_t volatile* selector_table = _selector_table;
        uint32_t capacity = _capacity;
        attach_to_blob(blob);
        freeze_table(selector_table, capacity);
        finish_rebuild(selector_table, capacity);
        continue;
      }
    }
    return NULL;
  }
}

template <typename V>
bool SelectorMap<V>::set(uint32_t selector, V value) {
  if (should_rebuild()) {
    rebuild();
  }
  for (;;) {
    uint32_t index = selector & mask();
    uint32_t start_index = index;
    do {
      // Try to insert the selector
      uint32_t prev_selector = Atomic::load(&_selector_table[index]);
      if (prev_selector == selector) {
        return false;
      }
      if (prev_selector == 0) {
        // Good candidate bucket for insert, let's try it.
        if (is_concurrent()) {
          V expected_value = 0;
          if (Atomic::cmpxchg(&_value_table[index], expected_value, value) == expected_value) {
            // When we win insertion of value, publishing the key can only fail because of
            // concurrent rebuilding. Concurrent inserts will back off.
            if (Atomic::cmpxchg(&_selector_table[index], prev_selector, selector) == prev_selector) {
              inc_size_for_blob(_blob);
              return true;
            } else {
              // We can only get here due to concurrent rebuilding freezing
              // the bucket. So finish rebuilding and retry.
              break;
            }
          }
        } else {
          _value_table[index] = value;
          _selector_table[index] = selector;
          inc_size_for_blob(_blob);
          return true;
        }
      }
      index = (index + 1) & mask();
    } while(index != start_index);
    // Nowhere to insert in current table. So get a new one and try again
    rebuild();
  }
}

template <typename V>
bool SelectorMap<V>::contains_value(V value) {
  for (uint32_t index = 0; index < capacity(); ++index) {
    if (_value_table[index] == value) {
      return true;
    }
  }
  return false;
}

template <typename V>
uint8_t* SelectorMap<V>::unlink() {
  assert(is_concurrent(), "sanity");
  if (calculate_dead_selectors() > 0) {
    rebuild();
  }

  // Grab free list
  for (;;) {
    uint8_t* free_list = Atomic::load(_free_list_ptr);
    if (Atomic::cmpxchg(_free_list_ptr, free_list, (uint8_t*)NULL) == free_list) {
      return free_list;
    }
  }
}

template <typename V>
void SelectorMap<V>::purge(uint8_t* free_list) {
  // Purge free list
  while (free_list != NULL) {
    uint8_t* next = purge_next_for_blob(free_list);
    free_list -= _concurrent_blob_header_size;
    FREE_C_HEAP_ARRAY(uint8_t, free_list);
    free_list = next;
  }
}

#endif // SHARE_OOPS_SELECTORMAP_INLINE_HPP

/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_CONTINUATIONGCSUPPORT_INLINE_HPP
#define SHARE_GC_SHARED_CONTINUATIONGCSUPPORT_INLINE_HPP

#include "gc/shared/continuationGCSupport.hpp"
#include "oops/instanceStackChunkKlass.hpp"
#include "oops/oop.inline.hpp"

inline void ContinuationGCSupport::relativize_chunk(oop obj) {
  if (!obj->is_stackChunk()) {
    return;
  }
  stackChunkOop chunk = (stackChunkOop)obj;
  InstanceStackChunkKlass::relativize_chunk(chunk);
}

inline void ContinuationGCSupport::shrink_stack_chunk(oop obj) {
  if (!obj->is_stackChunk()) {
    return;
  }
  HeapWord* to_space = cast_from_oop<HeapWord*>(obj);
  size_t uncompressed_size = static_cast<InstanceStackChunkKlass*>(obj->klass())->uncompressed_oop_size(obj);
  size_t compressed_size = obj->copy_conjoint(to_space, obj->size());
  HeapWord* filler_addr = to_space + compressed_size;
  size_t filler_size = uncompressed_size - compressed_size;
  if (filler_size >= CollectedHeap::min_fill_size()) {
    Universe::heap()->fill_with_dummy_object(filler_addr, filler_addr + filler_size, true);
  }
}

inline void HeapIterateObjectClosure::do_object(oop obj) {
  ContinuationGCSupport::shrink_stack_chunk(obj);
  _cl->do_object(obj);
}

#endif // SHARE_GC_SHARED_CONTINUATIONGCSUPPORT_INLINE_HPP

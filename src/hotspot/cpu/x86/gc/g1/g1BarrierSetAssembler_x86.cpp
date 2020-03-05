/*
 * Copyright (c) 2018, 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "asm/macroAssembler.inline.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/debug.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif // COMPILER1
#ifdef COMPILER2
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#endif // COMPILER2

#define __ masm->

void G1BarrierSetAssembler::gen_write_ref_array_pre_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                            Register addr, Register count) {
  bool dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;

  if (!dest_uninitialized) {
    Register thread = NOT_LP64(rax) LP64_ONLY(r15_thread);
#ifndef _LP64
    __ push(thread);
    __ get_thread(thread);
#endif

    Label filtered;
    Address in_progress(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
    // Is marking active?
    if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
      __ cmpl(in_progress, 0);
    } else {
      assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
      __ cmpb(in_progress, 0);
    }

    NOT_LP64(__ pop(thread);)

    __ jcc(Assembler::equal, filtered);

    __ push_call_clobbered_registers(false /* save_fpu */);
#ifdef _LP64
    if (count == c_rarg0) {
      if (addr == c_rarg1) {
        // exactly backwards!!
        __ xchgptr(c_rarg1, c_rarg0);
      } else {
        __ movptr(c_rarg1, count);
        __ movptr(c_rarg0, addr);
      }
    } else {
      __ movptr(c_rarg0, addr);
      __ movptr(c_rarg1, count);
    }
    if (UseCompressedOops) {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_pre_narrow_oop_entry), 2);
    } else {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_pre_oop_entry), 2);
    }
#else
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_pre_oop_entry),
                    addr, count);
#endif
    __ pop_call_clobbered_registers(false /* save_fpu */);

    __ bind(filtered);
  }
}

void G1BarrierSetAssembler::gen_write_ref_array_post_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                             Register addr, Register count, Register tmp) {
  __ push_call_clobbered_registers(false /* save_fpu */);
#ifdef _LP64
  if (c_rarg0 == count) { // On win64 c_rarg0 == rcx
    assert_different_registers(c_rarg1, addr);
    __ mov(c_rarg1, count);
    __ mov(c_rarg0, addr);
  } else {
    assert_different_registers(c_rarg0, count);
    __ mov(c_rarg0, addr);
    __ mov(c_rarg1, count);
  }
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_post_entry), 2);
#else
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_post_entry),
                  addr, count);
#endif
  __ pop_call_clobbered_registers(false /* save_fpu */);
}

void G1BarrierSetAssembler::load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                    Register dst, Address src, Register tmp1, Register tmp_thread) {
  bool on_oop = is_reference_type(type);
  bool on_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool on_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool on_reference = on_weak || on_phantom;
  ModRefBarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp_thread);
  if (on_oop && on_reference) {
    Register thread = NOT_LP64(tmp_thread) LP64_ONLY(r15_thread);

#ifndef _LP64
    // Work around the x86_32 bug that only manifests with Loom for some reason.
    // MacroAssembler::resolve_weak_handle calls this barrier with tmp_thread == noreg.
    if (thread == noreg) {
      if (dst != rcx && tmp1 != rcx) {
        thread = rcx;
      } else if (dst != rdx && tmp1 != rdx) {
        thread = rdx;
      } else if (dst != rdi && tmp1 != rdi) {
        thread = rdi;
      }
    }
    assert_different_registers(dst, tmp1, thread);
    __ push(thread);
    __ get_thread(thread);
#endif

    // Generate the G1 pre-barrier code to log the value of
    // the referent field in an SATB buffer.
    g1_write_barrier_pre(masm /* masm */,
                         noreg /* obj */,
                         dst /* pre_val */,
                         thread /* thread */,
                         tmp1 /* tmp */,
                         true /* tosca_live */,
                         true /* expand_call */);

#ifndef _LP64
    __ pop(thread);
#endif
  }
}

void G1BarrierSetAssembler::g1_write_barrier_pre(MacroAssembler* masm,
                                                 Register obj,
                                                 Register pre_val,
                                                 Register thread,
                                                 Register tmp,
                                                 bool tosca_live,
                                                 bool expand_call,
                                                 G1BarrierStubC2* c2_stub) {
  // If expand_call is true then we expand the call_VM_leaf macro
  // directly to skip generating the check by
  // InterpreterMacroAssembler::call_VM_leaf_base that checks _last_sp.

#ifdef _LP64
  assert(thread == r15_thread, "must be");
#endif // _LP64

  Label done;
  Label inplace_stub;
  Label& runtime = c2_stub != nullptr ? *c2_stub->entry() : inplace_stub;

  assert(pre_val != noreg, "check this code");

  if (obj != noreg) {
    assert_different_registers(obj, pre_val, tmp);
  }

  Address in_progress(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
  Address index(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_buffer_offset()));

  // Is marking active?
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ cmpl(in_progress, 0);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ cmpb(in_progress, 0);
  }
  __ jcc(Assembler::equal, done);

  // Do we need to load the previous value?
  if (obj != noreg) {
    __ load_heap_oop(pre_val, Address(obj, 0), noreg, noreg, AS_RAW);
  }

  // Is the previous value null?
  __ cmpptr(pre_val, NULL_WORD);
  __ jcc(Assembler::equal, done);

  // Can we store original value in the thread's buffer?
  // Is index == 0?
  // (The index field is typed as size_t.)

  __ movptr(tmp, index);                   // tmp := *index_adr
  __ cmpptr(tmp, 0);                       // tmp == 0?
  __ jcc(Assembler::equal, runtime);       // If yes, goto runtime

  __ subptr(tmp, wordSize);                // tmp := tmp - wordSize
  __ movptr(index, tmp);                   // *index_adr := tmp
  __ addptr(tmp, buffer);                  // tmp := tmp + *buffer_adr

  // Record the previous value
  __ movptr(Address(tmp, 0), pre_val);

  if (c2_stub == nullptr) {
    __ jmp(done);
  }

  __ bind(inplace_stub);

  if (c2_stub == nullptr) {
    // Determine and save the live input values
    __ push_call_clobbered_registers();

    // Calling the runtime using the regular call_VM_leaf mechanism generates
    // code (generated by InterpreterMacroAssember::call_VM_leaf_base)
    // that checks that the *(ebp+frame::interpreter_frame_last_sp) == NULL.
    //
    // If we care generating the pre-barrier without a frame (e.g. in the
    // intrinsified Reference.get() routine) then ebp might be pointing to
    // the caller frame and so this check will most likely fail at runtime.
    //
    // Expanding the call directly bypasses the generation of the check.
    // So when we do not have have a full interpreter frame on the stack
    // expand_call should be passed true.

    if (expand_call) {
      LP64_ONLY( assert(pre_val != c_rarg1, "smashed arg"); )
#ifdef _LP64
      if (c_rarg1 != thread) {
        __ mov(c_rarg1, thread);
      }
      if (c_rarg0 != pre_val) {
        __ mov(c_rarg0, pre_val);
      }
#else
      __ push(thread);
      __ push(pre_val);
#endif
      __ MacroAssembler::call_VM_leaf_base(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), 2);
    } else {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
    }

    __ pop_call_clobbered_registers();
  }

  __ bind(done);
}

void G1BarrierSetAssembler::g1_write_barrier_post(MacroAssembler* masm,
                                                  Register store_addr,
                                                  Register new_val,
                                                  Register thread,
                                                  Register tmp,
                                                  Register tmp2,
                                                  G1BarrierStubC2* c2_stub) {
#ifdef _LP64
  assert(thread == r15_thread, "must be");
#endif // _LP64

  Address queue_index(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_buffer_offset()));

  CardTableBarrierSet* ct =
    barrier_set_cast<CardTableBarrierSet>(BarrierSet::barrier_set());

  Label done;
  Label inplace_stub;
  Label& runtime = c2_stub != nullptr ? *c2_stub->entry() : inplace_stub;

  // Does store cross heap regions?

  __ movptr(tmp, store_addr);
  __ xorptr(tmp, new_val);
  __ shrptr(tmp, HeapRegion::LogOfHRGrainBytes);
  __ jcc(Assembler::equal, done);

  // crosses regions, storing NULL?

  __ cmpptr(new_val, NULL_WORD);
  __ jcc(Assembler::equal, done);

  // storing region crossing non-NULL, is card already dirty?

  const Register card_addr = tmp;
  const Register cardtable = tmp2;

  __ movptr(card_addr, store_addr);
  __ shrptr(card_addr, CardTable::card_shift());
  // Do not use ExternalAddress to load 'byte_map_base', since 'byte_map_base' is NOT
  // a valid address and therefore is not properly handled by the relocation code.
  __ movptr(cardtable, (intptr_t)ct->card_table()->byte_map_base());
  __ addptr(card_addr, cardtable);

  __ cmpb(Address(card_addr, 0), G1CardTable::g1_young_card_val());
  __ jcc(Assembler::equal, done);

  __ membar(Assembler::Membar_mask_bits(Assembler::StoreLoad));
  __ cmpb(Address(card_addr, 0), G1CardTable::dirty_card_val());
  __ jcc(Assembler::equal, done);

  // storing a region crossing, non-NULL oop, card is clean.
  // dirty card and log.

  __ movb(Address(card_addr, 0), G1CardTable::dirty_card_val());

  __ movptr(tmp2, queue_index);
  __ testptr(tmp2, tmp2);
  __ jcc(Assembler::zero, runtime);
  __ subptr(tmp2, wordSize);
  __ movptr(queue_index, tmp2);
  __ addptr(tmp2, buffer);
  __ movptr(Address(tmp2, 0), card_addr);

  if (c2_stub == nullptr) {
    __ jmp(done);
  }

  __ bind(inplace_stub);

  if (c2_stub == nullptr) {
    // save the live input values
    RegSet saved = RegSet::of(store_addr NOT_LP64(COMMA thread));
    __ push_set(saved);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry), card_addr, thread);
    __ pop_set(saved);
  }

  __ bind(done);
}

#if defined(COMPILER2) && defined(_LP64)

#undef __
#define __ _masm->

// We use the vec_spill_helper from the x86.ad file to avoid reinventing this wheel
extern void vec_spill_helper(CodeBuffer *cbuf, bool is_load,
                             int stack_offset, int reg, uint ireg, outputStream* st);
class G1SaveLiveRegisters {
private:
  struct XMMRegisterData {
    XMMRegister _reg;
    int         _size;

    // Used by GrowableArray::find()
    bool operator == (const XMMRegisterData& other) {
      return _reg == other._reg;
    }
  };

  MacroAssembler* const          _masm;
  GrowableArray<Register>        _gp_registers;
  GrowableArray<KRegister>       _opmask_registers;
  GrowableArray<XMMRegisterData> _xmm_registers;
  int                            _spill_size;
  int                            _spill_offset;

  static int xmm_compare_register_size(XMMRegisterData* left, XMMRegisterData* right) {
    if (left->_size == right->_size) {
      return 0;
    }

    return (left->_size < right->_size) ? -1 : 1;
  }

  static int xmm_slot_size(OptoReg::Name opto_reg) {
    // The low order 4 bytes denote what size of the XMM register is live
    return (opto_reg & 15) << 3;
  }

  static uint xmm_ideal_reg_for_size(int reg_size) {
    switch (reg_size) {
    case 8:
      return Op_VecD;
    case 16:
      return Op_VecX;
    case 32:
      return Op_VecY;
    case 64:
      return Op_VecZ;
    default:
      fatal("Invalid register size %d", reg_size);
      return 0;
    }
  }

  bool xmm_needs_vzeroupper() const {
    return _xmm_registers.is_nonempty() && _xmm_registers.at(0)._size > 16;
  }

  void xmm_register_save(const XMMRegisterData& reg_data) {
    const OptoReg::Name opto_reg = OptoReg::as_OptoReg(reg_data._reg->as_VMReg());
    const uint ideal_reg = xmm_ideal_reg_for_size(reg_data._size);
    _spill_offset -= reg_data._size;
    vec_spill_helper(__ code(), false /* is_load */, _spill_offset, opto_reg, ideal_reg, tty);
  }

  void xmm_register_restore(const XMMRegisterData& reg_data) {
    const OptoReg::Name opto_reg = OptoReg::as_OptoReg(reg_data._reg->as_VMReg());
    const uint ideal_reg = xmm_ideal_reg_for_size(reg_data._size);
    vec_spill_helper(__ code(), true /* is_load */, _spill_offset, opto_reg, ideal_reg, tty);
    _spill_offset += reg_data._size;
  }

  void gp_register_save(Register reg) {
    _spill_offset -= 8;
    __ movq(Address(rsp, _spill_offset), reg);
  }

  void opmask_register_save(KRegister reg) {
    _spill_offset -= 8;
    __ kmov(Address(rsp, _spill_offset), reg);
  }

  void gp_register_restore(Register reg) {
    __ movq(reg, Address(rsp, _spill_offset));
    _spill_offset += 8;
  }

  void opmask_register_restore(KRegister reg) {
    __ kmov(reg, Address(rsp, _spill_offset));
    _spill_offset += 8;
  }

  void initialize(G1BarrierStubC2* stub) {
    // Create mask of caller saved registers that need to
    // be saved/restored if live
    RegMask caller_saved;
    caller_saved.Insert(OptoReg::as_OptoReg(rax->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(rcx->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(rdx->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(rsi->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(rdi->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(r8->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(r9->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(r10->as_VMReg()));
    caller_saved.Insert(OptoReg::as_OptoReg(r11->as_VMReg()));

    // Create mask of live registers
    RegMask live = stub->live();
    live.Insert(OptoReg::as_OptoReg(stub->tmp1()->as_VMReg()));
    live.Insert(OptoReg::as_OptoReg(stub->tmp2()->as_VMReg()));
    live.Insert(OptoReg::as_OptoReg(stub->tmp3()->as_VMReg()));

    int gp_spill_size = 0;
    int opmask_spill_size = 0;
    int xmm_spill_size = 0;

    // Record registers that needs to be saved/restored
    RegMaskIterator rmi(live);
    while (rmi.has_next()) {
      const OptoReg::Name opto_reg = rmi.next();
      const VMReg vm_reg = OptoReg::as_VMReg(opto_reg);

      if (vm_reg->is_Register()) {
        if (caller_saved.Member(opto_reg)) {
          _gp_registers.append(vm_reg->as_Register());
          gp_spill_size += 8;
        }
      } else if (vm_reg->is_KRegister()) {
        // All opmask registers are caller saved, thus spill the ones
        // which are live.
        if (_opmask_registers.find(vm_reg->as_KRegister()) == -1) {
          _opmask_registers.append(vm_reg->as_KRegister());
          opmask_spill_size += 8;
        }
      } else if (vm_reg->is_XMMRegister()) {
        // We encode in the low order 4 bits of the opto_reg, how large part of the register is live
        const VMReg vm_reg_base = OptoReg::as_VMReg(opto_reg & ~15);
        const int reg_size = xmm_slot_size(opto_reg);
        const XMMRegisterData reg_data = { vm_reg_base->as_XMMRegister(), reg_size };
        const int reg_index = _xmm_registers.find(reg_data);
        if (reg_index == -1) {
          // Not previously appended
          _xmm_registers.append(reg_data);
          xmm_spill_size += reg_size;
        } else {
          // Previously appended, update size
          const int reg_size_prev = _xmm_registers.at(reg_index)._size;
          if (reg_size > reg_size_prev) {
            _xmm_registers.at_put(reg_index, reg_data);
            xmm_spill_size += reg_size - reg_size_prev;
          }
        }
      } else {
        fatal("Unexpected register type");
      }
    }

    // Sort by size, largest first
    _xmm_registers.sort(xmm_compare_register_size);

    // On Windows, the caller reserves stack space for spilling register arguments
    const int arg_spill_size = frame::arg_reg_save_area_bytes;

    // Stack pointer must be 16 bytes aligned for the call
    _spill_offset = _spill_size = align_up(xmm_spill_size + gp_spill_size + opmask_spill_size + arg_spill_size, 16);
  }

public:
  G1SaveLiveRegisters(MacroAssembler* masm, G1BarrierStubC2* stub) :
      _masm(masm),
      _gp_registers(),
      _opmask_registers(),
      _xmm_registers(),
      _spill_size(0),
      _spill_offset(0) {

    //
    // Stack layout after registers have been spilled:
    //
    // | ...            | original rsp, 16 bytes aligned
    // ------------------
    // | zmm0 high      |
    // | ...            |
    // | zmm0 low       | 16 bytes aligned
    // | ...            |
    // | ymm1 high      |
    // | ...            |
    // | ymm1 low       | 16 bytes aligned
    // | ...            |
    // | xmmN high      |
    // | ...            |
    // | xmmN low       | 8 bytes aligned
    // | reg0           | 8 bytes aligned
    // | reg1           |
    // | ...            |
    // | regN           | new rsp, if 16 bytes aligned
    // | <padding>      | else new rsp, 16 bytes aligned
    // ------------------
    //

    // Figure out what registers to save/restore
    initialize(stub);

    // Allocate stack space
    if (_spill_size > 0) {
      __ subptr(rsp, _spill_size);
    }

    // Save XMM/YMM/ZMM registers
    for (int i = 0; i < _xmm_registers.length(); i++) {
      xmm_register_save(_xmm_registers.at(i));
    }

    if (xmm_needs_vzeroupper()) {
      __ vzeroupper();
    }

    // Save general purpose registers
    for (int i = 0; i < _gp_registers.length(); i++) {
      gp_register_save(_gp_registers.at(i));
    }

    // Save opmask registers
    for (int i = 0; i < _opmask_registers.length(); i++) {
      opmask_register_save(_opmask_registers.at(i));
    }
  }

  ~G1SaveLiveRegisters() {
    // Restore opmask registers
    for (int i = _opmask_registers.length() - 1; i >= 0; i--) {
      opmask_register_restore(_opmask_registers.at(i));
    }

    // Restore general purpose registers
    for (int i = _gp_registers.length() - 1; i >= 0; i--) {
      gp_register_restore(_gp_registers.at(i));
    }

    __ vzeroupper();

    // Restore XMM/YMM/ZMM registers
    for (int i = _xmm_registers.length() - 1; i >= 0; i--) {
      xmm_register_restore(_xmm_registers.at(i));
    }

    // Free stack space
    if (_spill_size > 0) {
      __ addptr(rsp, _spill_size);
    }
  }
};

#undef __
#define __ masm->

void G1BarrierSetAssembler::emit_c2_barrier_stub(MacroAssembler* masm, G1BarrierStubC2* stub) {
  Assembler::InlineSkippedInstructionsCounter skip_counter(masm);
  __ bind(*stub->entry());
  {
    G1SaveLiveRegisters save_registers(masm, stub);
    if (c_rarg0 != stub->arg()) {
      __ mov(c_rarg0, stub->arg());
    }
    __ mov(c_rarg1, r15_thread);
    __ call(RuntimeAddress(stub->slow_path()), stub->tmp1());
  }
  __ jmp(*stub->continuation());
}

#endif // COMPILER2 && _LP64

void G1BarrierSetAssembler::oop_store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                         Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) {
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool as_normal = (decorators & AS_NORMAL) != 0;

  bool needs_pre_barrier = as_normal;
  bool needs_post_barrier = val != noreg && in_heap;

  Register rthread = LP64_ONLY(r15_thread) NOT_LP64(rcx);
  // flatten object address if needed
  // We do it regardless of precise because we need the registers
  if (dst.index() == noreg && dst.disp() == 0) {
    if (dst.base() != tmp1) {
      __ movptr(tmp1, dst.base());
    }
  } else {
    __ lea(tmp1, dst);
  }

#ifndef _LP64
  InterpreterMacroAssembler *imasm = static_cast<InterpreterMacroAssembler*>(masm);
#endif

  NOT_LP64(__ get_thread(rcx));
  NOT_LP64(imasm->save_bcp());

  if (needs_pre_barrier) {
    g1_write_barrier_pre(masm /*masm*/,
                         tmp1 /* obj */,
                         tmp2 /* pre_val */,
                         rthread /* thread */,
                         tmp3  /* tmp */,
                         val != noreg /* tosca_live */,
                         false /* expand_call */);
  }
  if (val == noreg) {
    BarrierSetAssembler::store_at(masm, decorators, type, Address(tmp1, 0), val, noreg, noreg, noreg);
  } else {
    Register new_val = val;
    if (needs_post_barrier) {
      // G1 barrier needs uncompressed oop for region cross check.
      if (UseCompressedOops) {
        new_val = tmp2;
        __ movptr(new_val, val);
      }
    }
    BarrierSetAssembler::store_at(masm, decorators, type, Address(tmp1, 0), val, noreg, noreg, noreg);
    if (needs_post_barrier) {
      g1_write_barrier_post(masm /*masm*/,
                            tmp1 /* store_adr */,
                            new_val /* new_val */,
                            rthread /* thread */,
                            tmp3 /* tmp */,
                            tmp2 /* tmp2 */);
    }
  }
  NOT_LP64(imasm->restore_bcp());
}

#ifdef COMPILER1

#undef __
#define __ ce->masm()->

void G1BarrierSetAssembler::gen_pre_barrier_stub(LIR_Assembler* ce, G1PreBarrierStub* stub) {
  G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  // At this point we know that marking is in progress.
  // If do_load() is true then we have to emit the
  // load of the previous value; otherwise it has already
  // been loaded into _pre_val.

  __ bind(*stub->entry());
  assert(stub->pre_val()->is_register(), "Precondition.");

  Register pre_val_reg = stub->pre_val()->as_register();

  if (stub->do_load()) {
    ce->mem2reg(stub->addr(), stub->pre_val(), T_OBJECT, stub->patch_code(), stub->info(), false /*wide*/);
  }

  __ cmpptr(pre_val_reg, NULL_WORD);
  __ jcc(Assembler::equal, *stub->continuation());
  ce->store_parameter(stub->pre_val()->as_register(), 0);
  __ call(RuntimeAddress(bs->pre_barrier_c1_runtime_code_blob()->code_begin()));
  __ jmp(*stub->continuation());

}

void G1BarrierSetAssembler::gen_post_barrier_stub(LIR_Assembler* ce, G1PostBarrierStub* stub) {
  G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  __ bind(*stub->entry());
  assert(stub->addr()->is_register(), "Precondition.");
  assert(stub->new_val()->is_register(), "Precondition.");
  Register new_val_reg = stub->new_val()->as_register();
  __ cmpptr(new_val_reg, NULL_WORD);
  __ jcc(Assembler::equal, *stub->continuation());
  ce->store_parameter(stub->addr()->as_pointer_register(), 0);
  __ call(RuntimeAddress(bs->post_barrier_c1_runtime_code_blob()->code_begin()));
  __ jmp(*stub->continuation());
}

#undef __

#define __ sasm->

void G1BarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  // Generated code assumes that buffer index is pointer sized.
  STATIC_ASSERT(in_bytes(SATBMarkQueue::byte_width_of_index()) == sizeof(intptr_t));

  __ prologue("g1_pre_barrier", false);
  // arg0 : previous value of memory

  __ push(rax);
  __ push(rdx);

  const Register pre_val = rax;
  const Register thread = NOT_LP64(rax) LP64_ONLY(r15_thread);
  const Register tmp = rdx;

  NOT_LP64(__ get_thread(thread);)

  Address queue_active(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
  Address queue_index(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_buffer_offset()));

  Label done;
  Label runtime;

  // Is marking still active?
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ cmpl(queue_active, 0);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ cmpb(queue_active, 0);
  }
  __ jcc(Assembler::equal, done);

  // Can we store original value in the thread's buffer?

  __ movptr(tmp, queue_index);
  __ testptr(tmp, tmp);
  __ jcc(Assembler::zero, runtime);
  __ subptr(tmp, wordSize);
  __ movptr(queue_index, tmp);
  __ addptr(tmp, buffer);

  // prev_val (rax)
  __ load_parameter(0, pre_val);
  __ movptr(Address(tmp, 0), pre_val);
  __ jmp(done);

  __ bind(runtime);

  __ push_call_clobbered_registers();

  // load the pre-value
  __ load_parameter(0, rcx);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), rcx, thread);

  __ pop_call_clobbered_registers();

  __ bind(done);

  __ pop(rdx);
  __ pop(rax);

  __ epilogue();
}

void G1BarrierSetAssembler::generate_c1_post_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("g1_post_barrier", false);

  CardTableBarrierSet* ct =
    barrier_set_cast<CardTableBarrierSet>(BarrierSet::barrier_set());

  Label done;
  Label enqueued;
  Label runtime;

  // At this point we know new_value is non-NULL and the new_value crosses regions.
  // Must check to see if card is already dirty

  const Register thread = NOT_LP64(rax) LP64_ONLY(r15_thread);

  Address queue_index(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_buffer_offset()));

  __ push(rax);
  __ push(rcx);

  const Register cardtable = rax;
  const Register card_addr = rcx;

  __ load_parameter(0, card_addr);
  __ shrptr(card_addr, CardTable::card_shift());
  // Do not use ExternalAddress to load 'byte_map_base', since 'byte_map_base' is NOT
  // a valid address and therefore is not properly handled by the relocation code.
  __ movptr(cardtable, (intptr_t)ct->card_table()->byte_map_base());
  __ addptr(card_addr, cardtable);

  NOT_LP64(__ get_thread(thread);)

  __ cmpb(Address(card_addr, 0), G1CardTable::g1_young_card_val());
  __ jcc(Assembler::equal, done);

  __ membar(Assembler::Membar_mask_bits(Assembler::StoreLoad));
  __ cmpb(Address(card_addr, 0), CardTable::dirty_card_val());
  __ jcc(Assembler::equal, done);

  // storing region crossing non-NULL, card is clean.
  // dirty card and log.

  __ movb(Address(card_addr, 0), CardTable::dirty_card_val());

  const Register tmp = rdx;
  __ push(rdx);

  __ movptr(tmp, queue_index);
  __ testptr(tmp, tmp);
  __ jcc(Assembler::zero, runtime);
  __ subptr(tmp, wordSize);
  __ movptr(queue_index, tmp);
  __ addptr(tmp, buffer);
  __ movptr(Address(tmp, 0), card_addr);
  __ jmp(enqueued);

  __ bind(runtime);
  __ push_call_clobbered_registers();

  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry), card_addr, thread);

  __ pop_call_clobbered_registers();

  __ bind(enqueued);
  __ pop(rdx);

  __ bind(done);
  __ pop(rcx);
  __ pop(rax);

  __ epilogue();
}

#undef __

#endif // COMPILER1

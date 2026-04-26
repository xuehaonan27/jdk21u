/*
 * Copyright (c) 2018, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1RemoteOop.hpp"
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
#endif

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

  // ================================================================
  // Disaggregated memory: assembler-level load barrier (Phase 4).
  // After the actual load, check if the oop is tagged (bit 63 set).
  // Valid heap pointers have bit 63 = 0 (heap below 2^47).
  // Tagged oops (Managed/Shared/Remote) have bit 63 = 1 ("negative").
  // Fast path: test + js = 2 instructions, 5 bytes. Same as ZGC.
  // ================================================================
  if (on_oop) {
    Label done;
    // Phase 1: test sign bit. Clean oops / null skip entirely.
    __ testptr(dst, dst);
    __ jcc(Assembler::positive, done);

    // Phase 2: resolve tagged oop via JRT_LEAF call with register preservation.
    //
    // Must save/restore ALL caller-saved GP registers — load_at is called
    // from interpreter templates (getfield, aaload, resolved reference load)
    // and other contexts where any register may be live.
    //
    // Use MacroAssembler::call_VM_leaf_base (qualified to bypass the
    // InterpreterMacroAssembler override that checks last_sp).
    //
    // Register save strategy: 9 GP caller-saved regs (rax,rcx,rdx,rsi,rdi,
    // r8-r11) + rbx for result stash = 10 pushes = 80 bytes. Even number of
    // pushes means RSP alignment parity is preserved → call_VM_leaf_base
    // handles final alignment internally.
    __ push(rbx);  // callee-saved scratch for result stash
    // Save all caller-saved GP registers
    __ push(rax); __ push(rcx); __ push(rdx); __ push(rsi); __ push(rdi);
    __ push(r8);  __ push(r9);  __ push(r10); __ push(r11);
    // 10 pushes total. Set up argument and call.
    if (dst != c_rarg0) __ mov(c_rarg0, dst);
    masm->MacroAssembler::call_VM_leaf_base(
        CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::resolve_tagged_oop), 1);
    // rax = resolved oop. Check if still tagged (REMOTE handle).
    {
      Label leaf_resolved;
      __ testptr(rax, rax);
      __ jcc(Assembler::positive, leaf_resolved);
      // Still tagged — REMOTE object needs fetch. Call non-safepointing
      // slow path (no thread transitions, so saved registers are safe).
      __ mov(c_rarg0, rax);
      masm->MacroAssembler::call_VM_leaf_base(
          CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::resolve_tagged_oop_no_safepoint), 1);
      __ bind(leaf_resolved);
    }
    // rax = fully resolved oop. Stash in rbx (callee-saved, survives pops).
    __ mov(rbx, rax);
    // Restore caller-saved GP registers (reverse order)
    __ pop(r11); __ pop(r10); __ pop(r9);  __ pop(r8);
    __ pop(rdi); __ pop(rsi); __ pop(rdx); __ pop(rcx); __ pop(rax);
    // Move resolved oop from rbx to dst, restore original rbx
    if (dst == rbx) {
      // dst IS rbx — result already there, just discard saved rbx
      __ addptr(rsp, wordSize);
    } else {
      __ mov(dst, rbx);
      __ pop(rbx);
    }

    __ bind(done);
  }

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

// Disaggregated memory: arraycopy load barrier for tagged oops.
// After the base copy_load_at loads the value into dst, check if it's tagged
// (bit 63 set) and resolve it.  This is the same barrier as in load_at but
// applied to individual element loads during oop arraycopy.
// Note: arraycopy stubs use rscratch1 (r10) as tmp when type is T_OBJECT.
// We must preserve all registers except dst across the runtime call.
void G1BarrierSetAssembler::copy_load_at(MacroAssembler* masm, DecoratorSet decorators,
                                         BasicType type, size_t bytes,
                                         Register dst, Address src, Register tmp) {
  // Delegate to the base implementation for the actual load.
  ModRefBarrierSetAssembler::copy_load_at(masm, decorators, type, bytes, dst, src, tmp);

  // Only apply the tag-resolve barrier to reference types.
  if (is_reference_type(type)) {
    Label done, leaf_ok;
    __ testptr(dst, dst);
    __ jcc(Assembler::positive, done);
    // Save state for two-phase resolution
    __ push(rbx);
    __ push_call_clobbered_registers(false /* save_fpu */);
    if (dst != c_rarg0) __ mov(c_rarg0, dst);
    // Phase 1: Leaf call
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::resolve_tagged_oop), c_rarg0);
    __ testptr(rax, rax);
    __ jcc(Assembler::positive, leaf_ok);
    // Phase 2: Non-safepointing slow path (saved registers not in OopMap).
    __ movptr(rbx, rax);   // rbx = still-tagged (callee-saved)
    __ pop_call_clobbered_registers(false);
    __ movptr(c_rarg0, rbx);
    // push(rbx) = 8 bytes on stack. Pad RSP for x86-64 ABI alignment.
    __ subptr(rsp, wordSize);
    __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::resolve_tagged_oop_no_safepoint)));
    __ addptr(rsp, wordSize);
    __ movptr(dst, rax);
    __ pop(rbx);
    __ jmp(done);
    // Leaf resolved
    __ bind(leaf_ok);
    __ movptr(rbx, rax);
    __ pop_call_clobbered_registers(false);
    __ movptr(dst, rbx);
    __ pop(rbx);
    __ bind(done);
  }
}

void G1BarrierSetAssembler::g1_write_barrier_pre(MacroAssembler* masm,
                                                 Register obj,
                                                 Register pre_val,
                                                 Register thread,
                                                 Register tmp,
                                                 bool tosca_live,
                                                 bool expand_call) {
  // If expand_call is true then we expand the call_VM_leaf macro
  // directly to skip generating the check by
  // InterpreterMacroAssembler::call_VM_leaf_base that checks _last_sp.

#ifdef _LP64
  assert(thread == r15_thread, "must be");
#endif // _LP64

  Label done;
  Label runtime;

  assert(pre_val != noreg, "check this code");

  if (obj != noreg) {
    assert_different_registers(obj, pre_val, tmp);
    assert(pre_val != rax, "check this code");
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
  __ jmp(done);

  __ bind(runtime);

  // Determine and save the live input values
  __ push_call_clobbered_registers();

  // Calling the runtime using the regular call_VM_leaf mechanism generates
  // code (generated by InterpreterMacroAssember::call_VM_leaf_base)
  // that checks that the *(ebp+frame::interpreter_frame_last_sp) == nullptr.
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

  __ bind(done);
}

void G1BarrierSetAssembler::g1_write_barrier_post(MacroAssembler* masm,
                                                  Register store_addr,
                                                  Register new_val,
                                                  Register thread,
                                                  Register tmp,
                                                  Register tmp2) {
#ifdef _LP64
  assert(thread == r15_thread, "must be");
#endif // _LP64

  Address queue_index(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_buffer_offset()));

  CardTableBarrierSet* ct =
    barrier_set_cast<CardTableBarrierSet>(BarrierSet::barrier_set());

  Label done;
  Label runtime;

  // Does store cross heap regions?

  __ movptr(tmp, store_addr);
  __ xorptr(tmp, new_val);
  __ shrptr(tmp, HeapRegion::LogOfHRGrainBytes);
  __ jcc(Assembler::equal, done);

  // crosses regions, storing null?

  __ cmpptr(new_val, NULL_WORD);
  __ jcc(Assembler::equal, done);

  // storing region crossing non-null, is card already dirty?

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


  // storing a region crossing, non-null oop, card is clean.
  // dirty card and log.

  __ movb(Address(card_addr, 0), G1CardTable::dirty_card_val());

  // The code below assumes that buffer index is pointer sized.
  STATIC_ASSERT(in_bytes(G1DirtyCardQueue::byte_width_of_index()) == sizeof(intptr_t));

  __ movptr(tmp2, queue_index);
  __ testptr(tmp2, tmp2);
  __ jcc(Assembler::zero, runtime);
  __ subptr(tmp2, wordSize);
  __ movptr(queue_index, tmp2);
  __ addptr(tmp2, buffer);
  __ movptr(Address(tmp2, 0), card_addr);
  __ jmp(done);

  __ bind(runtime);
  // save the live input values
  RegSet saved = RegSet::of(store_addr NOT_LP64(COMMA thread));
  __ push_set(saved);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry), card_addr, thread);
  __ pop_set(saved);

  __ bind(done);
}

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

// Disaggregated memory: C1 out-of-line stub for resolving tagged oops.
// This is the out-of-line code emitted after the main C1 instruction stream.
// The inline fast path (testptr + jcc positive → continuation) is emitted by
// load_at_resolved in g1BarrierSetC1.cpp; this stub handles the slow path.
void G1BarrierSetAssembler::generate_c1_tag_resolve_stub(LIR_Assembler* ce, G1TagResolveStub* stub) {
  G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  __ bind(*stub->entry());

  Register ref = stub->ref()->as_register();
  Register ref_addr = noreg;
  Register tmp = noreg;

  // ZGC pattern: re-read the oop from the original memory address.
  // The inline fast-path's testptr may have used a different physical register
  // (due to C1 register allocator interval splitting), so we can't rely on
  // the register value. Re-reading from memory is always correct.
  __ movptr(ref, ce->as_Address(stub->ref_addr()->as_address_ptr()));

  // Compute the effective address for the runtime call argument
  if (stub->tmp()->is_valid()) {
    ce->leal(stub->ref_addr(), stub->tmp());
    ref_addr = tmp = stub->tmp()->as_pointer_register();
  } else {
    ref_addr = stub->ref_addr()->as_address_ptr()->base()->as_pointer_register();
  }

  // Save rax unless it is the result or tmp register
  if (ref != rax && tmp != rax) {
    __ push(rax);
  }

  // Pass tagged oop to the runtime blob via parameter area
  ce->store_parameter(ref, 0);
  __ call(RuntimeAddress(bs->tag_resolve_c1_runtime_code_blob()->code_begin()));

  // Move result into ref register
  if (ref != rax) {
    __ movptr(ref, rax);
  }

  // Restore rax if saved
  if (ref != rax && tmp != rax) {
    __ pop(rax);
  }

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

  // At this point we know new_value is non-null and the new_value crosses regions.
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

  // storing region crossing non-null, card is clean.
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

// Disaggregated memory: C1 runtime blob for resolving tagged oops.
// Called from generate_c1_tag_resolve_stub (the out-of-line CodeStub).
// Receives the tagged oop via the parameter area (offset 0), calls the
// JRT_LEAF resolve_tagged_oop, and returns the clean oop in rax.
//
// We must save/restore all call-clobbered registers because C1 may have
// live values in them at the point of the tag-resolve barrier.  The result
// is returned in rax: we poke it into the saved-rax slot on the stack
// before popping so that pop_call_clobbered_registers restores the resolved
// oop into rax instead of the original tagged value.
void G1BarrierSetAssembler::generate_c1_tag_resolve_runtime_stub(StubAssembler* sasm) {
  __ prologue("g1_tag_resolve", false);

  // Two-phase resolve for C1 load barrier.
  //
  // Phase 1 (leaf): resolve_tagged_oop handles LOCAL Handles + Unique OOPs.
  //   For REMOTE/FETCHING, returns the tagged oop unchanged.
  //   JRT_LEAF: no safepoint, no thread transition, no GC.
  //
  // Phase 2 (slow): resolve_tagged_oop_slow handles REMOTE fetch with
  //   ThreadInVMfromJava + ThreadBlockInVM for blocking I/O.
  //   Requires set_last_Java_frame for GC stack walking.
  //
  // Register protocol:
  //   - rbx (callee-saved) stashes results across push/pop
  //   - r12 (callee-saved) saves original tagged oop for phase 2
  //   - All call-clobbered registers saved/restored around both calls

  Label leaf_resolved;

  __ push(rbx);
  __ push(r12);
  __ push_call_clobbered_registers();

  // Load tagged oop from parameter area: [rbp + (0+2)*8] = [rbp+16]
  __ load_parameter(0, c_rarg0);
  __ movptr(r12, c_rarg0);  // save original tagged oop for phase 2

  // Phase 1: leaf call
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::resolve_tagged_oop), c_rarg0);

  // Check: did leaf resolve? (bit 63 clear = resolved)
  __ testptr(rax, rax);
  __ jcc(Assembler::positive, leaf_resolved);

  // Phase 2: slow path — still tagged (REMOTE/FETCHING).
  // Non-safepointing: stays in _thread_in_Java, no OopMap needed.
  __ movptr(rbx, r12);  // tagged oop (for the slow call argument)
  __ pop_call_clobbered_registers();

  __ movptr(c_rarg0, rbx);  // tagged oop argument
  __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::resolve_tagged_oop_no_safepoint)));

  // Result in rax.
  __ pop(r12);
  __ pop(rbx);
  __ epilogue();

  // --- Leaf resolved fast path ---
  __ bind(leaf_resolved);
  __ movptr(rbx, rax);  // stash resolved oop
  __ pop_call_clobbered_registers();
  __ movptr(rax, rbx);
  __ pop(r12);
  __ pop(rbx);
  __ epilogue();
}

#undef __

#endif // COMPILER1

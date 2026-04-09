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
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_FrameMap.hpp"
#include "c1/c1_LIRGenerator.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "c1/c1_CodeStubs.hpp"
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.hpp"
#include "utilities/macros.hpp"

#ifdef ASSERT
#define __ gen->lir(__FILE__, __LINE__)->
#else
#define __ gen->lir()->
#endif

void G1PreBarrierStub::emit_code(LIR_Assembler* ce) {
  G1BarrierSetAssembler* bs = (G1BarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
  bs->gen_pre_barrier_stub(ce, this);
}

void G1PostBarrierStub::emit_code(LIR_Assembler* ce) {
  G1BarrierSetAssembler* bs = (G1BarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
  bs->gen_post_barrier_stub(ce, this);
}

void G1TagResolveStub::emit_code(LIR_Assembler* ce) {
  // Out-of-line slow path only (call runtime blob, jump back to continuation)
  G1BarrierSetAssembler* bs = (G1BarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
  bs->generate_c1_tag_resolve_stub(ce, this);
}

// Custom LIR op for the disaggregated memory load barrier.
// Emits raw testptr + jcc during codegen, bypassing C1's type-aware
// comparison optimizations (which would eliminate "oop < 0" as always-false).
//
// Uses SEPARATE input and output operand fields (Codex-found bug: a single
// shared field gets overwritten by C1's assign_reg_nums which colors input
// before output). Also explicitly appends the stub via ce->append_code_stub
// (Codex-found bug: without this, the stub's entry label is never bound,
// making the jcc branch a no-op).
class LIR_OpG1TagResolve : public LIR_Op {
private:
  LIR_Opr                 _ref_in;   // input: the loaded (possibly tagged) oop
  LIR_Opr                 _ref_out;  // output: the resolved (clean) oop
  G1TagResolveStub* const _stub;

public:
  LIR_OpG1TagResolve(LIR_Opr ref, G1TagResolveStub* stub)
    : LIR_Op(), _ref_in(ref), _ref_out(ref), _stub(stub) {}

  virtual void visit(LIR_OpVisitState* state) {
    state->do_input(_ref_in);
    state->do_output(_ref_out);
    state->do_stub(_stub);
  }

  virtual void emit_code(LIR_Assembler* ce) {
    auto* masm = ce->masm();
    Register in_reg  = _ref_in->as_register();
    Register out_reg = _ref_out->as_register();
    // Copy input to output if register allocator assigned different registers
    if (in_reg != out_reg) {
      masm->movptr(out_reg, in_reg);
    }
    // Update stub to use the output register (after register allocation)
    _stub->set_ref(_ref_out);
    // Inline fast path: test bit 63 (sign bit). Clean oops are positive.
    masm->testptr(out_reg, out_reg);
    masm->jcc(Assembler::negative, *_stub->entry());
    masm->bind(*_stub->continuation());
    // CRITICAL: append the stub so it gets emitted out-of-line.
    // Without this, the stub's entry label is never bound and the jcc
    // branches to garbage (effectively a no-op).
    ce->append_code_stub(_stub);
  }

  virtual void print_instr(outputStream* out) const { _ref_out->print(out); }
#ifndef PRODUCT
  virtual const char* name() const { return "g1_tag_resolve"; }
#endif
};

void G1BarrierSetC1::pre_barrier(LIRAccess& access, LIR_Opr addr_opr,
                                 LIR_Opr pre_val, CodeEmitInfo* info) {
  LIRGenerator* gen = access.gen();
  DecoratorSet decorators = access.decorators();

  // First we test whether marking is in progress.
  BasicType flag_type;
  bool patch = (decorators & C1_NEEDS_PATCHING) != 0;
  bool do_load = pre_val == LIR_OprFact::illegalOpr;
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    flag_type = T_INT;
  } else {
    guarantee(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1,
              "Assumption");
    // Use unsigned type T_BOOLEAN here rather than signed T_BYTE since some platforms, eg. ARM,
    // need to use unsigned instructions to use the large offset to load the satb_mark_queue.
    flag_type = T_BOOLEAN;
  }
  LIR_Opr thrd = gen->getThreadPointer();
  LIR_Address* mark_active_flag_addr =
    new LIR_Address(thrd,
                    in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()),
                    flag_type);
  // Read the marking-in-progress flag.
  // Note: When loading pre_val requires patching, i.e. do_load == true &&
  // patch == true, a safepoint can occur while patching. This makes the
  // pre-barrier non-atomic and invalidates the marking-in-progress check.
  // Therefore, in the presence of patching, we must repeat the same
  // marking-in-progress checking before calling into the Runtime. For
  // simplicity, we do this check unconditionally (regardless of the presence
  // of patching) in the runtime stub
  // (G1BarrierSetAssembler::generate_c1_pre_barrier_runtime_stub).
  LIR_Opr flag_val = gen->new_register(T_INT);
  __ load(mark_active_flag_addr, flag_val);
  __ cmp(lir_cond_notEqual, flag_val, LIR_OprFact::intConst(0));

  LIR_PatchCode pre_val_patch_code = lir_patch_none;

  CodeStub* slow;

  if (do_load) {
    assert(pre_val == LIR_OprFact::illegalOpr, "sanity");
    assert(addr_opr != LIR_OprFact::illegalOpr, "sanity");

    if (patch)
      pre_val_patch_code = lir_patch_normal;

    pre_val = gen->new_register(T_OBJECT);

    if (!addr_opr->is_address()) {
      assert(addr_opr->is_register(), "must be");
      addr_opr = LIR_OprFact::address(new LIR_Address(addr_opr, T_OBJECT));
    }
    slow = new G1PreBarrierStub(addr_opr, pre_val, pre_val_patch_code, info);
  } else {
    assert(addr_opr == LIR_OprFact::illegalOpr, "sanity");
    assert(pre_val->is_register(), "must be");
    assert(pre_val->type() == T_OBJECT, "must be an object");
    assert(info == nullptr, "sanity");

    slow = new G1PreBarrierStub(pre_val);
  }

  __ branch(lir_cond_notEqual, slow);
  __ branch_destination(slow->continuation());
}

void G1BarrierSetC1::post_barrier(LIRAccess& access, LIR_Opr addr, LIR_Opr new_val) {
  LIRGenerator* gen = access.gen();
  DecoratorSet decorators = access.decorators();
  bool in_heap = (decorators & IN_HEAP) != 0;
  if (!in_heap) {
    return;
  }

  // If the "new_val" is a constant null, no barrier is necessary.
  if (new_val->is_constant() &&
      new_val->as_constant_ptr()->as_jobject() == nullptr) return;

  if (!new_val->is_register()) {
    LIR_Opr new_val_reg = gen->new_register(T_OBJECT);
    if (new_val->is_constant()) {
      __ move(new_val, new_val_reg);
    } else {
      __ leal(new_val, new_val_reg);
    }
    new_val = new_val_reg;
  }
  assert(new_val->is_register(), "must be a register at this point");

  if (addr->is_address()) {
    LIR_Address* address = addr->as_address_ptr();
    LIR_Opr ptr = gen->new_pointer_register();
    if (!address->index()->is_valid() && address->disp() == 0) {
      __ move(address->base(), ptr);
    } else {
      assert(address->disp() != max_jint, "lea doesn't support patched addresses!");
      __ leal(addr, ptr);
    }
    addr = ptr;
  }
  assert(addr->is_register(), "must be a register at this point");

  LIR_Opr xor_res = gen->new_pointer_register();
  LIR_Opr xor_shift_res = gen->new_pointer_register();
  if (two_operand_lir_form) {
    __ move(addr, xor_res);
    __ logical_xor(xor_res, new_val, xor_res);
    __ move(xor_res, xor_shift_res);
    __ unsigned_shift_right(xor_shift_res,
                            LIR_OprFact::intConst(HeapRegion::LogOfHRGrainBytes),
                            xor_shift_res,
                            LIR_Opr::illegalOpr());
  } else {
    __ logical_xor(addr, new_val, xor_res);
    __ unsigned_shift_right(xor_res,
                            LIR_OprFact::intConst(HeapRegion::LogOfHRGrainBytes),
                            xor_shift_res,
                            LIR_Opr::illegalOpr());
  }

  __ cmp(lir_cond_notEqual, xor_shift_res, LIR_OprFact::intptrConst(NULL_WORD));

  CodeStub* slow = new G1PostBarrierStub(addr, new_val);
  __ branch(lir_cond_notEqual, slow);
  __ branch_destination(slow->continuation());
}

void G1BarrierSetC1::load_at_resolved(LIRAccess& access, LIR_Opr result) {
  DecoratorSet decorators = access.decorators();
  bool is_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool is_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool is_anonymous = (decorators & ON_UNKNOWN_OOP_REF) != 0;
  LIRGenerator *gen = access.gen();

  // Step 1: emit the raw load via the base class
  BarrierSetC1::load_at_resolved(access, result);

  // Step 2: disaggregated memory tag-resolve barrier (uncompressed oops only).
  //
  // Unconditional call to the two-phase runtime blob via call_runtime_leaf.
  // The blob handles: clean oops (fast return), LOCAL/Unique (leaf resolve),
  // and REMOTE (slow path with set_last_Java_frame for GC safety).
  //
  // ZGC-shaped inline test (LIR_OpG1TagResolve) was attempted but fails due to
  // C1's register allocator splitting the loaded value's interval, causing the
  // testptr to check the wrong register. The unconditional call avoids this by
  // having no barrier-specific LIR op (the call is part of every load's processing).
  //
  // Performance: ~5ns overhead per clean oop load (function call + immediate return).
  // The runtime blob does testptr internally — no resolve work for clean oops.
  if (access.is_oop() && !UseCompressedOops) {
    BasicTypeArray sig;
    sig.append(T_OBJECT);
    LIR_OprList args;
    args.append(result);
    // Call resolve_tagged_oop_slow which has two-phase logic internally:
    // Phase 1 (leaf): handles clean/LOCAL/Unique
    // Phase 2 (slow): set_last_Java_frame + REMOTE fetch
    address entry = CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::resolve_tagged_oop_slow);
    CallingConvention* cc = gen->frame_map()->c_calling_convention(&sig);
    for (int i = 0; i < args.length(); i++) {
      LIR_Opr loc = cc->at(i);
      if (loc->is_register()) __ move(args.at(i), loc);
    }
    LIR_Opr phys_result = FrameMap::as_oop_opr(rax);
    __ call_runtime_leaf(entry, gen->getThreadTemp(), phys_result, cc->args());
    __ move(phys_result, result);
  }

  // Step 3: SATB keepalive for weak/phantom/anonymous refs
  if (access.is_oop() && (is_weak || is_phantom || is_anonymous)) {
    // Register the value in the referent field with the pre-barrier
    LabelObj *Lcont_anonymous;
    if (is_anonymous) {
      Lcont_anonymous = new LabelObj();
      generate_referent_check(access, Lcont_anonymous);
    }
    pre_barrier(access, LIR_OprFact::illegalOpr /* addr_opr */,
                result /* pre_val */, access.patch_emit_info() /* info */);
    if (is_anonymous) {
      __ branch_destination(Lcont_anonymous->label());
    }
  }
}

class C1G1PreBarrierCodeGenClosure : public StubAssemblerCodeGenClosure {
  virtual OopMapSet* generate_code(StubAssembler* sasm) {
    G1BarrierSetAssembler* bs = (G1BarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
    bs->generate_c1_pre_barrier_runtime_stub(sasm);
    return nullptr;
  }
};

class C1G1PostBarrierCodeGenClosure : public StubAssemblerCodeGenClosure {
  virtual OopMapSet* generate_code(StubAssembler* sasm) {
    G1BarrierSetAssembler* bs = (G1BarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
    bs->generate_c1_post_barrier_runtime_stub(sasm);
    return nullptr;
  }
};

class C1G1TagResolveCodeGenClosure : public StubAssemblerCodeGenClosure {
  virtual OopMapSet* generate_code(StubAssembler* sasm) {
    G1BarrierSetAssembler* bs = (G1BarrierSetAssembler*)BarrierSet::barrier_set()->barrier_set_assembler();
    bs->generate_c1_tag_resolve_runtime_stub(sasm);
    return nullptr;
  }
};

void G1BarrierSetC1::generate_c1_runtime_stubs(BufferBlob* buffer_blob) {
  C1G1PreBarrierCodeGenClosure pre_code_gen_cl;
  C1G1PostBarrierCodeGenClosure post_code_gen_cl;
  C1G1TagResolveCodeGenClosure tag_resolve_code_gen_cl;
  _pre_barrier_c1_runtime_code_blob = Runtime1::generate_blob(buffer_blob, -1, "g1_pre_barrier_slow",
                                                              false, &pre_code_gen_cl);
  _post_barrier_c1_runtime_code_blob = Runtime1::generate_blob(buffer_blob, -1, "g1_post_barrier_slow",
                                                               false, &post_code_gen_cl);
  _tag_resolve_c1_runtime_code_blob = Runtime1::generate_blob(buffer_blob, -1, "g1_tag_resolve_slow",
                                                              false, &tag_resolve_code_gen_cl);
}

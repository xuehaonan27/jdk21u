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

#ifndef SHARE_GC_G1_C1_G1BARRIERSETC1_HPP
#define SHARE_GC_G1_C1_G1BARRIERSETC1_HPP

#include "c1/c1_CodeStubs.hpp"
#include "c1/c1_Compilation.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/shared/c1/modRefBarrierSetC1.hpp"

class G1PreBarrierStub: public CodeStub {
  friend class G1BarrierSetC1;
 private:
  bool _do_load;
  LIR_Opr _addr;
  LIR_Opr _pre_val;
  LIR_PatchCode _patch_code;
  CodeEmitInfo* _info;

 public:
  // Version that _does_ generate a load of the previous value from addr.
  // addr (the address of the field to be read) must be a LIR_Address
  // pre_val (a temporary register) must be a register;
  G1PreBarrierStub(LIR_Opr addr, LIR_Opr pre_val, LIR_PatchCode patch_code, CodeEmitInfo* info) :
    _do_load(true), _addr(addr), _pre_val(pre_val),
    _patch_code(patch_code), _info(info)
  {
    assert(_pre_val->is_register(), "should be temporary register");
    assert(_addr->is_address(), "should be the address of the field");
    FrameMap* f = Compilation::current()->frame_map();
    f->update_reserved_argument_area_size(2 * BytesPerWord);
  }

  // Version that _does not_ generate load of the previous value; the
  // previous value is assumed to have already been loaded into pre_val.
  G1PreBarrierStub(LIR_Opr pre_val) :
    _do_load(false), _addr(LIR_OprFact::illegalOpr), _pre_val(pre_val),
    _patch_code(lir_patch_none), _info(nullptr)
  {
    assert(_pre_val->is_register(), "should be a register");
    FrameMap* f = Compilation::current()->frame_map();
    f->update_reserved_argument_area_size(2 * BytesPerWord);
  }

  LIR_Opr addr() const { return _addr; }
  LIR_Opr pre_val() const { return _pre_val; }
  LIR_PatchCode patch_code() const { return _patch_code; }
  CodeEmitInfo* info() const { return _info; }
  bool do_load() const { return _do_load; }

  virtual void emit_code(LIR_Assembler* e);
  virtual void visit(LIR_OpVisitState* visitor) {
    if (_do_load) {
      // don't pass in the code emit info since it's processed in the fast
      // path
      if (_info != nullptr)
        visitor->do_slow_case(_info);
      else
        visitor->do_slow_case();

      visitor->do_input(_addr);
      visitor->do_temp(_pre_val);
    } else {
      visitor->do_slow_case();
      visitor->do_input(_pre_val);
    }
  }
#ifndef PRODUCT
  virtual void print_name(outputStream* out) const { out->print("G1PreBarrierStub"); }
#endif // PRODUCT
};

class G1PostBarrierStub: public CodeStub {
  friend class G1BarrierSetC1;
 private:
  LIR_Opr _addr;
  LIR_Opr _new_val;

 public:
  // addr (the address of the object head) and new_val must be registers.
  G1PostBarrierStub(LIR_Opr addr, LIR_Opr new_val): _addr(addr), _new_val(new_val) {
    FrameMap* f = Compilation::current()->frame_map();
    f->update_reserved_argument_area_size(2 * BytesPerWord);
  }

  LIR_Opr addr() const { return _addr; }
  LIR_Opr new_val() const { return _new_val; }

  virtual void emit_code(LIR_Assembler* e);
  virtual void visit(LIR_OpVisitState* visitor) {
    // don't pass in the code emit info since it's processed in the fast path
    visitor->do_slow_case();
    visitor->do_input(_addr);
    visitor->do_input(_new_val);
  }
#ifndef PRODUCT
  virtual void print_name(outputStream* out) const { out->print("G1PostBarrierStub"); }
#endif // PRODUCT
};

// Disaggregated memory: C1 out-of-line stub for resolving tagged oops.
// ZGC-shaped: carries both the result register AND the memory address.
// The stub re-reads the oop from memory to avoid register allocator
// interval splitting issues.
//
// Inline fast path: testptr(ref, ref) + jcc(negative, stub)
// Out-of-line stub: re-reads from ref_addr, calls runtime blob
class G1TagResolveStub: public CodeStub {
  friend class G1BarrierSetC1;
 private:
  LIR_Opr _ref;       // result register (loaded oop / resolved oop)
  LIR_Opr _ref_addr;  // memory address to re-read from (ZGC pattern)
  LIR_Opr _tmp;       // temp register for complex addresses
  uint32_t _access_hint;

 public:
  G1TagResolveStub(LIRAccess& access, LIR_Opr ref, uint32_t access_hint)
    : _ref(ref),
      _ref_addr(access.resolved_addr()),
      _tmp(LIR_OprFact::illegalOpr),
      _access_hint(access_hint) {
    assert(_ref->is_register(), "must be a register");
    assert(_ref_addr->is_address(), "must be an address");

    // Allocate tmp register if address has index or displacement
    if (_ref_addr->as_address_ptr()->index()->is_valid() ||
        _ref_addr->as_address_ptr()->disp() != 0) {
      _tmp = access.gen()->new_pointer_register();
    }

    FrameMap* f = Compilation::current()->frame_map();
    f->update_reserved_argument_area_size(2 * BytesPerWord);
  }

  LIR_Opr ref() const { return _ref; }
  LIR_Opr ref_addr() const { return _ref_addr; }
  LIR_Opr tmp() const { return _tmp; }
  uint32_t access_hint() const { return _access_hint; }
  void set_ref(LIR_Opr ref) { _ref = ref; }

  virtual void emit_code(LIR_Assembler* e);
  virtual void visit(LIR_OpVisitState* visitor) {
    visitor->do_slow_case();
    visitor->do_input(_ref_addr);  // keep address live for re-read
    visitor->do_input(_ref);       // input: loaded (possibly tagged) oop
    visitor->do_output(_ref);      // output: resolved (clean) oop
    if (_tmp->is_valid()) {
      visitor->do_temp(_tmp);
    }
  }
#ifndef PRODUCT
  virtual void print_name(outputStream* out) const { out->print("G1TagResolveStub"); }
#endif // PRODUCT
};

class CodeBlob;

class G1BarrierSetC1 : public ModRefBarrierSetC1 {
 protected:
  CodeBlob* _pre_barrier_c1_runtime_code_blob;
  CodeBlob* _post_barrier_c1_runtime_code_blob;
  CodeBlob* _tag_resolve_c1_runtime_code_blob;

  virtual void pre_barrier(LIRAccess& access, LIR_Opr addr_opr,
                           LIR_Opr pre_val, CodeEmitInfo* info);
  virtual void post_barrier(LIRAccess& access, LIR_Opr addr, LIR_Opr new_val);

  virtual void load_at_resolved(LIRAccess& access, LIR_Opr result);

 public:
  G1BarrierSetC1()
    : _pre_barrier_c1_runtime_code_blob(nullptr),
      _post_barrier_c1_runtime_code_blob(nullptr),
      _tag_resolve_c1_runtime_code_blob(nullptr) {}

  CodeBlob* pre_barrier_c1_runtime_code_blob() { return _pre_barrier_c1_runtime_code_blob; }
  CodeBlob* post_barrier_c1_runtime_code_blob() { return _post_barrier_c1_runtime_code_blob; }
  CodeBlob* tag_resolve_c1_runtime_code_blob() { return _tag_resolve_c1_runtime_code_blob; }

  virtual void generate_c1_runtime_stubs(BufferBlob* buffer_blob);
};

#endif // SHARE_GC_G1_C1_G1BARRIERSETC1_HPP

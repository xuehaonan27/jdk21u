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

#ifndef SHARE_GC_G1_C2_G1BARRIERSETC2_HPP
#define SHARE_GC_G1_C2_G1BARRIERSETC2_HPP

#include "gc/shared/c2/cardTableBarrierSetC2.hpp"
#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"

// G1 disaggregated-memory barrier_data bits for C2 load barriers.
// Low byte keeps the historical "barrier present + access hint" encoding.
// Upper bits carry a compile-time site id into the emitted Mach stub, so the
// runtime slow path can use bytecode/IR semantics instead of guessing from
// arbitrary registers after code generation.
const C2BarrierData G1BarrierTag = 1;
const uint G1BarrierAccessHintShift = 1;
const C2BarrierData G1BarrierAccessHintMask = 0x7f;
const uint G1BarrierSiteIdShift = 8;
const C2BarrierData G1BarrierSiteIdMask = 0x00ffffff;

inline C2BarrierData g1_barrier_data_with_access_hint(uint32_t access_hint,
                                                      uint32_t site_id = 0) {
  return G1BarrierTag |
      ((access_hint & G1BarrierAccessHintMask) << G1BarrierAccessHintShift) |
      ((site_id & G1BarrierSiteIdMask) << G1BarrierSiteIdShift);
}

inline uint32_t g1_access_hint_from_barrier_data(C2BarrierData barrier_data) {
  return (uint32_t)((barrier_data >> G1BarrierAccessHintShift) &
                    G1BarrierAccessHintMask);
}

inline uint32_t g1_site_id_from_barrier_data(C2BarrierData barrier_data) {
  return (uint32_t)((barrier_data >> G1BarrierSiteIdShift) &
                    G1BarrierSiteIdMask);
}

class MacroAssembler;
class MachNode;
class PhaseTransform;
class Type;
class TypeFunc;
class ciMethod;

class G1C2RemoteAccessSite {
public:
  enum SemanticKind {
    SemanticNone,
    SemanticField,
    SemanticArray
  };

private:
  uint32_t      _id;
  uint32_t      _access_hint;
  SemanticKind  _kind;
  int           _offset;
  int           _bci;
  ciMethod*     _method;

public:
  G1C2RemoteAccessSite()
    : _id(0),
      _access_hint(0),
      _kind(SemanticNone),
      _offset(-1),
      _bci(-1),
      _method(nullptr) {}

  G1C2RemoteAccessSite(uint32_t id, uint32_t access_hint, SemanticKind kind,
                       int offset, int bci, ciMethod* method)
    : _id(id),
      _access_hint(access_hint),
      _kind(kind),
      _offset(offset),
      _bci(bci),
      _method(method) {}

  uint32_t id() const { return _id; }
  uint32_t access_hint() const { return _access_hint; }
  SemanticKind kind() const { return _kind; }
  int offset() const { return _offset; }
  int bci() const { return _bci; }
  ciMethod* method() const { return _method; }
};

// ============================================================
// G1 disaggregated-memory C2 load-barrier stub (out-of-line)
// ============================================================
// One instance per oop load that may encounter a tagged pointer (bit 63 set).
// The slow-path calls G1BarrierSetRuntime::resolve_tagged_oop().

class G1TagResolveStubC2 : public ArenaObj {
private:
  const MachNode* _node;
  Address         _ref_addr;
  Register        _ref;
  uint32_t        _access_hint;
  uint32_t        _site_id;
  G1C2RemoteAccessSite::SemanticKind _semantic_kind;
  int             _semantic_offset;
  Label           _entry;
  Label           _continuation;

public:
  G1TagResolveStubC2(const MachNode* node, Address ref_addr, Register ref);

  Label* entry();
  Label* continuation();
  Register ref() const;

  void emit_code(MacroAssembler& masm);
};

// Compilation-scoped state holding the list of G1 tag-resolve stubs.
class G1BarrierSetC2State : public ArenaObj {
private:
  GrowableArray<G1TagResolveStubC2*>* _stubs;
  GrowableArray<G1C2RemoteAccessSite>* _access_sites;

public:
  G1BarrierSetC2State(Arena* arena);
  GrowableArray<G1TagResolveStubC2*>* stubs();
  uint32_t add_access_site(uint32_t access_hint,
                           G1C2RemoteAccessSite::SemanticKind kind,
                           int offset,
                           int bci,
                           ciMethod* method);
  const G1C2RemoteAccessSite* access_site(uint32_t id) const;
};

class G1BarrierSetC2: public CardTableBarrierSetC2 {
protected:
  virtual void pre_barrier(GraphKit* kit,
                           bool do_load,
                           Node* ctl,
                           Node* obj,
                           Node* adr,
                           uint adr_idx,
                           Node* val,
                           const TypeOopPtr* val_type,
                           Node* pre_val,
                           BasicType bt) const;

  virtual void post_barrier(GraphKit* kit,
                            Node* ctl,
                            Node* store,
                            Node* obj,
                            Node* adr,
                            uint adr_idx,
                            Node* val,
                            BasicType bt,
                            bool use_precise) const;

  bool g1_can_remove_pre_barrier(GraphKit* kit,
                                 PhaseValues* phase,
                                 Node* adr,
                                 BasicType bt,
                                 uint adr_idx) const;

  bool g1_can_remove_post_barrier(GraphKit* kit,
                                  PhaseValues* phase, Node* store,
                                  Node* adr) const;

  void g1_mark_card(GraphKit* kit,
                    IdealKit& ideal,
                    Node* card_adr,
                    Node* oop_store,
                    uint oop_alias_idx,
                    Node* index,
                    Node* index_adr,
                    Node* buffer,
                    const TypeFunc* tf) const;

  // Helper for unsafe accesses, that may or may not be on the referent field.
  // Generates the guards that check whether the result of
  // Unsafe.getReference should be recorded in an SATB log buffer.
  void insert_pre_barrier(GraphKit* kit, Node* base_oop, Node* offset, Node* pre_val, bool need_mem_bar) const;

  static const TypeFunc* write_ref_field_pre_entry_Type();
  static const TypeFunc* write_ref_field_post_entry_Type();

  virtual Node* store_at_resolved(C2Access& access, C2AccessValue& val) const;
  virtual Node* load_at_resolved(C2Access& access, const Type* val_type) const;

#ifdef ASSERT
  bool has_cas_in_use_chain(Node* x) const;
  void verify_pre_load(Node* marking_check_if, Unique_Node_List& loads /*output*/) const;
  void verify_no_safepoints(Compile* compile, Node* marking_load, const Unique_Node_List& loads) const;
#endif

  static bool is_g1_pre_val_load(Node* n);
public:
  virtual void* create_barrier_state(Arena* comp_arena) const;
  virtual void emit_stubs(CodeBuffer& cb) const;
  virtual int estimate_stub_size() const;
  virtual Node* atomic_cmpxchg_val_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                Node* new_val, const Type* val_type) const;
  virtual Node* atomic_cmpxchg_bool_at_resolved(C2AtomicParseAccess& access, Node* expected_val,
                                                Node* new_val, const Type* value_type) const;
  virtual Node* atomic_xchg_at_resolved(C2AtomicParseAccess& access, Node* new_val, const Type* val_type) const;

  virtual bool is_gc_pre_barrier_node(Node* node) const;
  virtual bool is_gc_barrier_node(Node* node) const;
  virtual void eliminate_gc_barrier(PhaseMacroExpand* macro, Node* node) const;
  virtual Node* step_over_gc_barrier(Node* c) const;

#ifdef ASSERT
  virtual void verify_gc_barriers(Compile* compile, CompilePhase phase) const;
#endif

  virtual bool escape_add_to_con_graph(ConnectionGraph* conn_graph, PhaseGVN* gvn, Unique_Node_List* delayed_worklist, Node* n, uint opcode) const;
};

#endif // SHARE_GC_G1_C2_G1BARRIERSETC2_HPP

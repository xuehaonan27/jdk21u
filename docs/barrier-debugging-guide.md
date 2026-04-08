# Debugging Guide: G1 Disaggregated Memory Load Barriers

## Overview

This document records the bugs found and techniques used while implementing
load barriers for tagged oops (bit 63 set) in OpenJDK 21's G1 GC. The goal
was to write tagged oop values into ~272K heap oop-field slots and have all
oop access paths resolve them transparently.

**Final result: 300/300 StressTest passes** (100-round × 3 independent runs)
in release build with full C1+C2 tiered compilation.

---

## Bug Catalog (in discovery order)

### Bug 1: Interpreter `last_sp` stale state

**Symptom**: Fastdebug assert `InterpreterMacroAssembler::call_VM_leaf_base: last_sp != null`.

**Root cause**: Our barrier in `G1BarrierSetAssembler::load_at()` used `call_VM_leaf`
which goes through `InterpreterMacroAssembler::call_VM_leaf_base`. This override
asserts `last_sp == null`. But `load_at` is called after the interpreter has already
done a `call_VM` (e.g., for invokedynamic resolution), which sets `last_sp`.

**Fix**: Use `masm->MacroAssembler::call_VM_leaf_base(entry, nargs)` — a qualified
call that bypasses the InterpreterMacroAssembler override's `last_sp` check.

**Lesson**: `load_at()` is called from BOTH interpreter templates AND non-interpreter
contexts (stubs, runtime code, MethodHandle adapters). Any code in `load_at` must
work in ALL contexts, not just the interpreter.

### Bug 2: Interpreter `[rbp-16]` stack corruption

**Symptom**: Release build SIGSEGV with corrupted RSP (RSP=0x10).

**Root cause**: We tried to clear `last_sp` by writing to
`Address(rbp, frame::interpreter_frame_last_sp_offset * wordSize)` = `[rbp-16]`.
When `load_at` is called from a non-interpreter context (e.g., a BufferBlob
stub), `rbp` does NOT point to an interpreter frame. Writing to `[rbp-16]`
corrupts a local variable or saved register.

**Fix**: Removed all `last_sp` manipulation from `load_at`. The qualified
`MacroAssembler::call_VM_leaf_base` call makes `last_sp` checking unnecessary.

**Lesson**: Never assume `rbp` points to an interpreter frame in `load_at`.

### Bug 3: `System.arraycopy` raw oop copy

**Symptom**: `ArrayIndexOutOfBoundsException` with corrupted index values.

**Root cause**: `ObjArrayKlass::do_copy` → `ModRefBarrierSet::oop_arraycopy_in_heap`
→ `Raw::oop_arraycopy` copies tagged oop bytes without any barrier resolution.
`Arrays.copyOfRange` in BootstrapMethodInvoker propagates tagged bytes to
destination arrays.

**Fix**: Override `G1BarrierSet::AccessBarrier::oop_arraycopy_in_heap` to resolve
tagged oops in source elements before the bulk copy.

**Lesson**: `System.arraycopy` for Object[] bypasses ALL load barriers. Any GC
that uses tagged/colored oops must override the arraycopy path.

### Bug 4: `CompressedOops::decode_not_null` assert

**Symptom**: Fastdebug assert `Universe::is_in_heap(v)` for tagged oop value.

**Root cause**: GC closures called `CompressedOops::decode_not_null(heap_oop)`
which asserts the value is in the heap. Tagged oops (bit 63 set) are outside
the heap address range.

**Fix**: Convert all GC closures to use `g1_resolved_load(p)` which loads as
raw `*(uintptr_t*)p` and resolves via `resolve_oop_raw()` before creating an
`oop` object.

**Lesson**: In debug builds, `CompressedOops::decode_not_null(oop v)` validates
heap range. Any code that handles tagged oops must avoid this function for
wide oops.

### Bug 5: `oop` constructor `check_oop` assert

**Symptom**: Fastdebug assert at `oopsHierarchy.hpp:95` — `check_oop` in `oop`
constructor.

**Root cause**: In debug builds, every `oop` object creation (`oop(oopDesc* o)`)
calls `on_construction()` → `check_oop()` → `is_oop()` which checks heap range.
Creating an `oop` from a tagged pointer triggers this check.

**Fix**: `g1_resolved_load` loads via `*(uintptr_t*)p` (raw integer, no `oop`
construction), resolves the tagged value, THEN creates the `oop` from the
clean result.

**Lesson**: In debug builds, the `oop` TYPE ITSELF is a checked type. You cannot
create an `oop` from a tagged value even temporarily. Always resolve first.

### Bug 6: Stack alignment failure

**Symptom**: Fastdebug assert `os::verify_stack_alignment()` inside
`resolve_tagged_oop`.

**Root cause**: `push_call_clobbered_registers()` adjusts RSP by a variable
amount (includes XMM register save area). Our manual alignment calculation
(`andptr(rsp, -16) + subptr(rsp, 8)`) was wrong because the XMM area size
is not a simple multiple of 8.

**Fix**: Use simple GP register push/pop (10 pushes = 80 bytes, even number
preserves alignment parity) and let `MacroAssembler::call_VM_leaf_base`
handle final alignment internally. Do NOT use `push_call_clobbered_registers`
which includes variable-size XMM area.

**Lesson**: Avoid `push_call_clobbered_registers` for manual calling convention
management. Use explicit `push`/`pop` of individual GP registers.

### Bug 7: Caller-saved register clobber

**Symptom**: `HandleArea::allocate_handle: not an oop` — stack address where
oop expected.

**Root cause**: `call_VM_leaf_base` (even the MacroAssembler version) clobbers
all caller-saved GP registers (rax, rcx, rdx, rsi, rdi, r8-r11). Callers of
`load_at` (getfield template, aaload template, MethodHandle adapter loads)
may have live values in these registers.

**Fix**: Push all 9 caller-saved GP registers before the call, pop after.
Use rbx (callee-saved) to stash the result across the pop sequence.

**Lesson**: A load barrier must be TRANSPARENT — only `dst` may change. All
other registers must be preserved. This is the same contract as ZGC/Shenandoah
barriers (they save/restore around runtime calls too).

### Bug 8: Interpreter `dst==rbx` overwrite

**Symptom**: Fastdebug `stop` fires: "tagged oop leaked (bit 63 set after resolve)".

**Root cause**: The result stash pattern uses rbx (callee-saved) to hold the
resolved oop across `pop_call_clobbered_registers`. But if `dst == rbx`, the
final `pop(rbx)` overwrites the resolved result with the original saved rbx
value (the tagged oop).

**Fix**: When `dst == rbx`, skip the final `pop(rbx)` and discard the saved
value with `addptr(rsp, wordSize)`.

**Lesson**: ALWAYS check the `dst == scratch_register` case in any
stash-and-restore pattern. This is easy to miss because rbx is rarely
used as dst, but it DOES happen in some template contexts.

### Bug 9: C2 `_ref==rbx` overwrite (same bug, different location)

**Symptom**: Release build SIGSEGV at `MethodTypeForm.cachedLambdaForm+12`.
Levels 1-3 (C1) pass, level 4 (C2) fails.

**Root cause**: The C2 out-of-line tag-resolve stub (`G1TagResolveStubC2`)
has the EXACT same rbx stash pattern and the EXACT same overwrite bug.
When the C2 register allocator assigns `_ref` to rbx, the stub's
`pop(rbx)` overwrites the resolved result.

**Fix**: Same as Bug 8 — check `_ref == rbx` and skip the pop.

**Lesson**: When fixing a pattern bug, search for ALL instances of the
same pattern. The C2 stub was written independently from the interpreter
barrier but used the same rbx-stash approach.

### Bug 10: GC verification closures

**Symptom**: Fastdebug guarantee at `VerifyOopClosure`, `G1VerifyLiveAndRemSetClosure`,
code root verification.

**Root cause**: Verification closures in `heapRegion.cpp`, `g1HeapVerifier.cpp`,
`g1CollectedHeap.cpp`, and `oop.cpp` use `RawAccess::oop_load` +
`CompressedOops::decode_not_null` which fail for tagged oops.

**Fix**: Convert all verification closures to use `g1_resolved_load()`.
For `VerifyOopClosure`, skip verification for tagged values (bits 47+ set).

**Lesson**: Heap verification runs during `System.gc()` and at shutdown.
ALL closures that iterate heap oops must handle tagged values.

---

## Debugging Techniques That Worked

### 1. Fastdebug build with asserts
The single most valuable technique. Debug builds catch bugs immediately
instead of letting corruption cascade. Each assert failure pointed directly
to the bug.

### 2. Graduated testing
Test each mode independently:
- `-Xint` (interpreter only)
- `-XX:TieredStopAtLevel=1` (C1 only)
- `-XX:TieredStopAtLevel=3` (C1 all levels)
- `-XX:TieredStopAtLevel=4` or default (C2)
- `-XX:-TieredCompilation` (C2 direct)
This isolates which compiler tier has the bug.

### 3. `stop` instruction in generated code
Adding `__ stop("message")` after the barrier (inside `#ifdef ASSERT`) catches
leaked tagged oops at the exact point of escape. Much more informative than
chasing corruption downstream.

### 4. Guarantee in runtime functions
Adding `guarantee(!G1TagRefSites || ...)` in `resolve_tagged_oop` catches
impossible Handle states. Even if the guarantee doesn't fire, its absence
is diagnostic (proves the runtime function works correctly).

### 5. Stale-site validation
Checking `resolve_oop_raw(*p) == expected_obj` before writing a tagged oop
proves the write target is correct. `stale_skip=0` proved all writes were
to the right addresses — the bug was in how the values were READ back, not
how they were WRITTEN.

### 6. 100-round testing
Single-pass tests can miss timing-dependent bugs. 100-round tests catch
rare race conditions and alignment-dependent failures.

### 7. Asking Codex GPT 5.4 for code review
External AI review identified `System.arraycopy` as a concrete bypass path
and `Handle::raw_resolve` as a potential leak — both confirmed by source code.
Also identified the missing stale validation in existing-class classification
branches.

---

## Barrier Architecture (Final Working Version)

### Interpreter (g1BarrierSetAssembler_x86.cpp load_at)
```
testptr dst, dst              ; bit 63 test (sign bit)
jcc positive, done            ; clean oop → skip (5 bytes total)
push rbx                      ; save callee-saved scratch
push rax,rcx,rdx,rsi,rdi,r8-r11  ; save ALL caller-saved GP regs
mov c_rarg0, dst              ; argument = tagged oop
MacroAssembler::call_VM_leaf_base(resolve_tagged_oop, 1)  ; qualified call
mov rbx, rax                  ; stash result in rbx (callee-saved)
pop r11,r10,...,rax            ; restore caller-saved GP regs
mov dst, rbx / addptr rsp,8   ; result to dst (handle dst==rbx case)
pop rbx                       ; restore rbx (if dst != rbx)
done:
```

### C1 (g1BarrierSetC1.cpp)
Single `call_runtime_leaf(resolve_tagged_oop_slow)` per oop load.
Safe because `call_runtime_leaf` preserves registers and handles alignment.
The `resolve_tagged_oop_slow` function does `ThreadInVMfromJava` internally.

### C2 (g1BarrierSetC2.cpp + g1_x86_64.ad)
Out-of-line stub with two-phase resolve:
- Phase 1: push_call_clobbered + call resolve_tagged_oop (leaf)
- Phase 2: set_last_Java_frame + call resolve_tagged_oop_slow
Both phases handle `_ref == rbx` case.

### C++ Runtime (g1BarrierSet.inline.hpp)
`oop_load_in_heap` / `oop_load_in_heap_at` check tag bits and resolve inline.
`oop_arraycopy_in_heap` resolves source elements before bulk copy.

### GC Closures (g1RemoteOop.hpp)
`g1_resolved_load(T* p)` loads as raw `*(uintptr_t*)p` to avoid debug oop
constructor, then resolves via `resolve_oop_raw`.

---

## Complete List of Hooked OOP Access Paths

| Path | Mechanism | Status |
|------|-----------|--------|
| Interpreter `getfield` (atos) | `load_heap_oop` → `load_at` | ✓ Barriered |
| Interpreter `aaload` | `load_heap_oop` → `load_at` | ✓ Barriered |
| Interpreter `getstatic` (atos) | `load_heap_oop` → `load_at` | ✓ Barriered |
| Interpreter `ldc` (object) | `load_resolved_reference_at_index` → `load_heap_oop` | ✓ Barriered |
| Interpreter `invokedynamic` appendix | `load_resolved_reference_at_index` → `load_heap_oop` | ✓ Barriered |
| Interpreter `invokehandle` appendix | `load_resolved_reference_at_index` → `load_heap_oop` | ✓ Barriered |
| MethodHandle `jump_to_lambda_form` | `load_heap_oop` (form, vmentry, method) | ✓ Barriered |
| MethodHandle `linkTo*` stubs | `load_heap_oop` (clazz, method) | ✓ Barriered |
| C1 `LoadField` | `BarrierSetC1::load_at_resolved` | ✓ Barriered |
| C2 `LoadP` (oop) | `g1LoadP` .ad pattern → stub | ✓ Barriered |
| C2 `CompareAndExchangeP` | `g1CompareAndExchangeP` → stub | ✓ Barriered |
| C2 `CompareAndSwapP` | `g1CompareAndSwapP` → stub | ✓ Barriered |
| C2 `GetAndSetP` | `g1GetAndSetP` → stub | ✓ Barriered |
| C++ `obj_field()` | `HeapAccess<>::oop_load_at` → `oop_load_in_heap_at` | ✓ Barriered |
| C++ `obj_at()` (ObjArrayOop) | `HeapAccess<IS_ARRAY>::oop_load_at` | ✓ Barriered |
| C++ `Unsafe.getReference` | `HeapAccess<ON_UNKNOWN_OOP_REF>::oop_load_at` | ✓ Barriered |
| `System.arraycopy` (Object[]) | `G1BarrierSet::oop_arraycopy_in_heap` | ✓ Barriered |
| Arraycopy stub (JIT) | `copy_load_at` in assembler | ✓ Barriered |
| GC evacuation closures | `g1_resolved_load()` | ✓ Converted |
| GC concurrent marking | `g1_resolved_load()` | ✓ Converted |
| GC Full GC closures | `g1_resolved_load()` | ✓ Converted |
| GC verification closures | `g1_resolved_load()` / skip tagged | ✓ Converted |
| SATB pre-barrier (array) | `resolve_oop_raw` before enqueue | ✓ Converted |
| Code root scanning | `g1_resolved_load()` | ✓ Converted |

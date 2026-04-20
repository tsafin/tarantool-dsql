# Copy-and-Patch for VDBE: Implementation Plan

**Branch baseline:** `tsafin/llvm_jit` of `tsafin/tarantool-dsql`
**Related reference:** Xu & Kjolstad, *Copy-and-Patch Compilation* (OOPSLA 2021, arXiv:2011.13127)
**Target file layout:** `src/box/sql/vdbe_cnp.{c,h}`, stencil library under `src/box/sql/cnp_stencils/`

---

## 1. Why copy-and-patch, and why now

The current `vdbe_jit.c` LLVM MCJIT path has a known, measured problem that is structural, not a tuning issue. `SQL_JIT_BENCHMARK.md` on the branch records it plainly:

| Workload | Interpreter prepare | MCJIT prepare | Slowdown |
| --- | --- | --- | --- |
| `tiny_const` | 2.78 µs | 7378 µs | 2650x |
| `hot_expr` | 3.88 µs | 8314 µs | 2144x |
| `point_lookup` | 10.11 µs | 9277 µs | 918x |

Break-even for a reused prepared statement lands at **13,000–31,000 executions**. Anything short of that — which includes almost every `automatic_execute` path and the entire SQL TAP suite — is net-negative against the interpreter.

Copy-and-patch (CnP) is designed to close exactly that gap. Xu & Kjolstad report CnP compile times on the order of **5–50 ns per AST node / bytecode op**, producing code roughly comparable to LLVM `-O0`. At that speed, compilation is essentially free relative to VDBE prepare, and JIT stops being gated behind a reuse heuristic — we can compile every prepared statement unconditionally.

The branch is unusually well positioned to adopt CnP because the handlers have already been extracted into standalone files with stable ABI (`vdbe_ops_*.c`, 8 files, 141/142 opcodes covered), and the DSL (`tools/vdbe_dsl/opcodes.yaml`) already classifies every opcode by how it interacts with dispatcher state. That is precisely the metadata a stencil generator needs. We can reuse the same handlers for a third execution backend without rewriting them.

## 2. The one-paragraph mental model

Stencils are short, patchable chunks of machine code, one per opcode variant. They are generated **at build time** by compiling each handler with a fixed calling convention, then post-processing the object file to extract the function body and record every relocation as a "hole." At **prepare time**, compiling a VDBE program becomes: for each `Op`, copy the appropriate stencil bytes into a code buffer, patch the holes with the actual operands (`P1`, `P2`, `P3`, `pMem` pointers, branch targets), and append. No LLVM, no IR, no optimizer. The output is native code, roughly -O0 quality, generated in microseconds.

## 3. What we reuse vs. what is new

**Reused unchanged:**
- All 8 `vdbe_ops_*.c` handler files. The `vdbe_op_<name>_inline(Vdbe *p, VdbeOp *pOp, Mem *aMem)` signature is already the canonical one and is what stencils will wrap.
- `opcodes.yaml` DSL and `vdbe_codegen.py` as the single source of truth for opcode classification. Extended, not forked.
- Runtime selector: the existing `VDBE_DISPATCHER=old|generated|parallel|auto` env var pattern. Add `cnp` as a fourth option.
- `box.stat.sql()` counters and `SQL_VDBE_OP_PROFILE` machinery. Per-opcode timing already works for both interpreter and MCJIT; CnP just needs its own counter set that mirrors them.
- `sqlVdbeMakeReady` / `sqlVdbeExec` / `sqlVdbeClearObject` integration points that `vdbe_jit.c` already uses. CnP plugs into the same three call sites.

**New, to be added:**
- A build-time stencil extraction pipeline.
- A runtime stencil loader and patcher (`vdbe_cnp.c`).
- A tiny number of CnP-only handler variants where the inline form cannot be used verbatim (see §6).

## 4. Architecture

```
                        BUILD TIME                                    PREPARE TIME              EXEC TIME
                                                                                          
  vdbe_ops_arith.c ──┐                                                                    
  vdbe_ops_*.c ──────┤──► clang -O2 ──► *.o ──► stencil_extract.py ──► stencils.h ─┐     
                     │                                                              │      
  opcodes.yaml ──────┴──► vdbe_codegen.py ──► op_to_stencil.inc ──┐                 │      
                                                                   │                 │     
                                                                   └──► libbox ──────┼──► vdbe_cnp_compile(p) ─► native buffer ─► call f()
                                                                                     │      (µs scale)                              
                                                                  stencil table ─────┘                                              
```

The critical compile-time piece is **`stencil_extract.py`**. For each handler, it:
1. Reads the ELF/Mach-O `.text` section for `vdbe_op_<n>_inline`.
2. Trims the prologue/epilogue (or arranges the stencil to be a tail-call, see §5).
3. Walks the relocation table, classifying each relocation into a "hole kind" (operand value, operand pointer, branch target, external call target).
4. Emits a C header with the stencil bytes, the hole table, and a symbol-to-stencil lookup.

This is the same post-processing pattern used by CPython's 3.13+ copy-and-patch tier-2 JIT and by Xu & Kjolstad's MetaVar tool, so there is a working reference for every tricky bit.

## 5. Stencil calling convention and the tail-call trick

A naive copy-and-patch scheme copies function bodies minus their prologues/epilogues. This works but is fragile on every platform. The standard solution, used by CPython and by the paper, is:

1. Compile each handler with the `[[clang::musttail]]` (or GCC equivalent) tail call to the next opcode handler.
2. Force a fixed register convention for the arguments (`p`, `pOp`, `aMem`, `pc`) by declaring them as `preserve_none` or explicit register parameters.
3. The compiler then emits the handler as a "function that ends in `jmp <next_handler>`." The final jump is exactly the hole we need to patch at runtime to chain to the next opcode's stencil.

For our branch, the per-opcode signature is:

```c
// Current inline handler
int vdbe_op_Add_inline(Vdbe *p, VdbeOp *pOp, Mem *aMem);

// CnP-shaped wrapper, generated once per opcode
[[clang::musttail]] static void
vdbe_cnp_Add(Vdbe *p, Mem *aMem, int pc)
{
    VdbeOp op = HOLE_OP; // patched: embedded P1/P2/P3/P4/P5 constants
    int rc = vdbe_op_Add_inline(p, &op, aMem);
    if (rc < 0) return HOLE_ERROR_EXIT(p, pc, rc);
    // For external_inline: rc > 0 means "jump to P2"
    if (rc > 0) return HOLE_BRANCH_TAKEN(p, aMem, HOLE_P2);
    return HOLE_BRANCH_FALLTHROUGH(p, aMem, pc + 1);
}
```

`HOLE_OP`, `HOLE_P2`, `HOLE_BRANCH_TAKEN`, `HOLE_BRANCH_FALLTHROUGH`, `HOLE_ERROR_EXIT` are all placeholders that generate specific relocations, which `stencil_extract.py` recognizes by symbol name pattern. The runtime patcher knows how to fill each.

**Key insight:** because our `external_inline` handlers return a three-way code (rc<0 / rc=0 / rc>0), the control flow between stencils is already normalized. The DSL `handler_type` is a direct map to a stencil shape:

| DSL `handler_type` | Stencil shape |
| --- | --- |
| `external_inline` | 3-way branch (error / jump P2 / fallthrough) |
| `external` | 1-way tail call into next PC |
| `inline` | needs per-opcode custom stencil (small set, see §7) |
| `fallthrough` | degenerate: empty stencil, only the tail jump |
| `control_flow` | deferred, see §10 |

## 6. Build-time pipeline in detail

Add one CMake target, `cnp_stencils`, that runs after `make vdbe_codegen`.

1. **Stencil source generation** (`tools/vdbe_cnp_genstubs.py`):
   Reads `opcodes.yaml`, emits one `vdbe_cnp_stubs.c` containing a `vdbe_cnp_<OP>` function per opcode, shaped as in §5. External calls to `vdbe_op_<n>_inline` remain unresolved — they will be resolved once at library load time and baked into the stencil patch table.

2. **Stencil compilation**:
   `clang -O2 -fno-pic -fno-asynchronous-unwind-tables -fno-stack-protector -c vdbe_cnp_stubs.c -o vdbe_cnp_stubs.o`

   - `-O2` matters: we get real register allocation, instruction scheduling, and constant folding within the stencil.
   - `-fno-pic` simplifies relocations to the small number of kinds CnP handles natively.
   - `-fno-asynchronous-unwind-tables` removes `.eh_frame` noise; we emit our own minimal unwind info per §8.
   - Architecture-gated: start with `x86_64-linux` and `aarch64-linux`, which covers both production Tarantool targets. macOS Mach-O support is a second pass; the Mach-O relocation encoding is different but the algorithm is identical.

3. **Stencil extraction** (`tools/vdbe_cnp_extract.py`):
   Uses `pyelftools` (already a reasonable dep; alternative is `llvm-objdump --reloc --disassemble`). For each `vdbe_cnp_<OP>` symbol:
   - Extract the raw bytes of the function body (not prologue/epilogue — we compile as `[[gnu::naked]]`-adjacent so there aren't any).
   - For each relocation targeting the function's bytes, record `{offset, size, kind, symbol}`.
   - Map `symbol` to a `HoleKind` enum (`HOLE_P1`, `HOLE_P2`, `HOLE_P3`, `HOLE_BRANCH_TARGET`, `HOLE_NEXT_PC`, `HOLE_MEM_PTR`, `HOLE_CALL_HANDLER`, `HOLE_EXIT`, ...).
   - Emit a C array: `static const struct cnp_stencil stencils[] = { [OP_Add] = { .bytes = ..., .size = ..., .holes = { ... } }, ... };`

4. **Packaging**:
   The generated `vdbe_cnp_stencils.h` is built into `libbox.a` directly. Unlike MCJIT, **no `.bc` files ship at install time** — the stencil table is just data in the binary. This also means CnP has no LLVM runtime dependency, only a build-time one (clang for producing the stencil table, same as current `make vdbe_codegen`).

## 7. The `inline` holdouts

`CLAUDE.md` lists 9 opcodes that still embed `inline_code` blobs rather than calling `_inline` helpers: `OP_OffsetLimit`, `OP_SetSession`, `OP_ShowCreateTable`, `OP_SorterSort`, `OP_Program`, `OP_Compare`, `OP_Permutation`, `OP_TTransaction`, `OP_IteratorOpen`, `OP_SorterOpen`.

**The interpreter track already plans to extract these to `_inline` handlers** ("Remaining Work (Interpreter Track), item 2"). CnP directly benefits from that work — do not duplicate it in a CnP-specific form. Priority order for the CnP project:

1. If a handler is already `external_inline`, it gets a stencil automatically.
2. If it is `inline`, help push the interpreter-track extraction so the stencil falls out.
3. Only `OP_Program` needs truly custom treatment, and only because it swaps `aOp`/`aMem` at runtime — same reason it is the sole MCJIT holdout today. For CnP, handle it the same way MCJIT handles it: mark as `CNP_MODE_UNSUPPORTED`, fall back to the generated dispatcher for that single op. The cost is negligible because CnP is free to mix: a CnP-compiled program can contain an interpreter-fallback "trampoline" for any op that isn't covered.

## 8. Runtime: what `vdbe_cnp.c` does

```c
struct CnpProgram {
    uint8_t *code;          // mmap'd RWX (or W^X on Apple Silicon/OpenBSD)
    size_t   code_size;
    uint32_t *pc_to_offset; // pc → offset into code, for branch patching
};

int vdbe_cnp_compile(Vdbe *p);
int vdbe_cnp_exec(Vdbe *p);
void vdbe_cnp_release(Vdbe *p);
```

### `vdbe_cnp_compile(Vdbe *p)` — the hot path

Single pass, O(nOp):

1. Size the code buffer: `sum(stencils[aOp[i].opcode].size for i in 0..nOp)` plus per-op padding. Allocate with `mmap(PROT_READ|PROT_WRITE, MAP_ANON)`, or reuse a per-fiber arena for very short programs to avoid `mmap` overhead (one of the wins vs MCJIT is *not* paying allocator cost).
2. Copy each stencil's bytes into the buffer at a computed offset. Record `pc_to_offset[i]`.
3. Second sub-pass: for each hole, compute the patch value and `memcpy` it into the buffer. Branch-target holes use `pc_to_offset[aOp[i].p2]` to resolve.
4. Flip protection to `PROT_READ|PROT_EXEC`. Call `__builtin___clear_cache` (ARM) or emit `sfence`/serializing instruction (x86) as needed.
5. Cache `CnpProgram` on `struct Vdbe` next to the existing `jit_func` field.

Expected cost, scaling with Xu & Kjolstad's reported numbers:
- Per-op work: ~30 ns (copy) + ~20 ns (patch) = **~50 ns/op**.
- `tiny_const` has ~5 ops → ~250 ns, versus 7378 µs for MCJIT. That is a ~30,000x prepare-time reduction, not a typo.
- `point_lookup` is ~30 ops → ~1.5 µs.

### `vdbe_cnp_exec(Vdbe *p)`

Same entry convention as the MCJIT function today. Calls `code(p, aMem, 0)` and handles the return. Keep the existing `vdbe_jit.c` return-PC-for-unsupported-op protocol — when we hit `OP_Program` or any uncovered op, the stencil for that op is the "fallback trampoline," which returns the PC so the interpreter can resume, identical to the current MCJIT design. This is the single most important cross-track decision: **the CnP track inherits all the fallback plumbing MCJIT already built.**

### Unwinding and debugging

CnP code has no compiler-generated unwind info. Two levels of support:

- **Phase 1 (must have):** install a signal-safe handler or set a stack guard so that a fault inside CnP code produces a clear error message pointing to a PC. This is sufficient for a feature-flagged rollout.
- **Phase 2 (nice to have):** emit synthetic `.eh_frame` entries via `__register_frame` for each compiled program, and register `__jit_debug_register_code` so `gdb` / `perf` see frames. Copy the CPython 3.13 approach — they solved this and their implementation is BSD-compatible enough to study.

## 9. Testing strategy

We inherit an enormous test asset from the interpreter and MCJIT tracks. Reuse, don't duplicate.

1. **Conformance against the generated dispatcher.** Every SQL TAP suite run with `VDBE_DISPATCHER=cnp SQL_JIT_ENABLE=1`, compared row-by-row with `VDBE_DISPATCHER=generated SQL_JIT_ENABLE=0`. Both paths share the same handler bodies, so any divergence is a bug in the stencil machinery itself — a very tight invariant.
2. **Parallel-execute mode.** The branch already supports `VDBE_DISPATCHER=parallel`, which runs two dispatchers and diffs state. Extend to `cnp+generated` pairing. This catches stencil bugs immediately, per-op.
3. **Fuzz stencils.** For each opcode, emit a minimal synthetic VDBE program (1 op, canned `aMem` state) that exercises all three return paths (error / taken / fallthrough). Run the stencil in isolation and compare register state with the interpreter. This is the single most valuable test asset a CnP project can own; it runs in seconds and catches relocation / hole-kind bugs the TAP suite would take minutes to surface.
4. **Benchmark parity.** Extend `tools/jit_bench/sql_llvm_mcjit_benchmark.lua` to add a `cnp` mode column. Re-run the same three workloads. The break-even table should collapse: we expect CnP to win on `prepared_execute` by some smaller margin than MCJIT (because the generated code is -O0 quality), but to **also** win on `prepare_only` and `automatic_execute`, because compile cost is now measured in nanoseconds.

## 10. Scope boundaries

Not in the first shipped version, in order of likely priority for a follow-up:

- **`OP_Program`** (same exclusion as MCJIT today). Falls back to interpreter.
- **`control_flow` DSL category.** These are dispatcher-structural ops (`OP_Goto`, `OP_Gosub`, `OP_Return`, `OP_Halt`). Handle natively in `vdbe_cnp.c` as special stencils rather than going through the generic pipeline — there are only ~10 of them and their stencil shapes don't match the handler shape.
- **macOS Mach-O and Windows COFF** support for the stencil extractor. Ship Linux x86_64 + aarch64 first. Tarantool's macOS builds can fall back to MCJIT or interpreter via the `VDBE_DISPATCHER` switch until Phase 2.
- **Tiered execution** ("warm up in CnP, recompile hot statements in MCJIT"). The architecture supports it naturally because `Vdbe` can carry both a `CnpProgram` and a `jit_func`, but ship single-tier first.
- **Inline caches for dynamic type dispatch.** Xu & Kjolstad's Deegen follow-up paper shows how to add these via multiple stencil variants per op, selected by observed type. VDBE already has strong type resolution at prepare time, so this is lower priority for us than for a dynamic language VM.

## 11. Milestones and risk

| Phase | Deliverable | Risk | Gate |
| --- | --- | --- | --- |
| M1 — Extractor | `vdbe_cnp_extract.py` handles x86_64 ELF, produces stencil table for 5 handpicked opcodes (`OP_Integer`, `OP_Add`, `OP_Copy`, `OP_Goto`, `OP_Column`) | Relocation kinds we haven't seen | Stencils round-trip: disassembly matches source |
| M2 — Runtime | `vdbe_cnp.c` compiles and runs a hand-built 5-op program end to end | W^X plumbing on all targets | Sum 1+2+3+4+5 returns correct result |
| M3 — Full coverage | All 141 JIT-covered opcodes have stencils; TAP `sql-tap` suite passes | Long tail of handler quirks | Parallel-diff against generated dispatcher: zero divergence |
| M4 — Benchmark | `SQL_JIT_BENCHMARK.md` extended with `cnp` column | Worst case: CnP code slower than interpreter | `tiny_const automatic_execute` ≥ interpreter throughput |
| M5 — Unwind + debug | `perf` / `gdb` see JIT frames | Cross-platform unwind encoding | `perf record` produces symbolicated samples |
| M6 — Default on | Flip default when `ENABLE_SQL_CNP=ON` | Rollback complexity | Two consecutive weekly TAP runs clean |

The single biggest technical risk is the relocation-kind inventory: compilers occasionally emit relocations the extractor doesn't recognize, and the failure mode is a silent miscompile rather than a build error. Mitigate with a strict-mode extractor (unknown relocation → hard fail, never "best effort"), and with the per-opcode fuzz tests from §9(3), which catch any stencil whose patching semantics are wrong.

The biggest project risk is scope creep into the `inline` / `control_flow` opcodes. Resist it. The interpreter track already plans to fix `inline` holdouts, and CnP should ride that work rather than duplicate it.

## 12. One-line pitch for the team

> Same handlers, same tests, same `VDBE_DISPATCHER` selector — we just stop paying LLVM's 8-millisecond tax on prepare.

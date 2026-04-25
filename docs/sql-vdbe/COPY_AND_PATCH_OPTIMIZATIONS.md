# Copy-and-Patch: Optimization Plan

## 1. Problem statement

Current CnP compile time is good, but execution throughput is not yet
consistently better than the interpreter. The current runtime model explains
why:

- each opcode executes as a separate stencil function,
- each stencil uses the normal C ABI,
- many stencils call a generic helper function,
- the outer CnP loop decodes a tagged return value after every opcode.

This means the current CnP path still pays a substantial per-op dispatch cost.
For short programs such as `tiny_const` and `hot_expr`, this dispatch cost can
dominate the actual work and erase any benefit from native code generation.

The main optimization question is therefore not "how do we compile faster?"
but "how do we reduce per-op execution overhead and improve code quality of the
generated native path?"

## 2. Hypothesis: inline the first helper layer into stencils

One concrete idea is to force-inline the first helper level called by the
stencil, so that the actual opcode body is emitted directly into the stencil
instead of staying behind a normal call boundary.

Example today:

```text
CnP loop
  -> stencil for OP_Add
    -> call vdbe_op_add()
      -> call small helper(s)
```

Desired shape for hot opcodes:

```text
CnP loop
  -> stencil for OP_Add
    -> inlined vdbe_op_add() body
      -> maybe one or two residual helper calls only if unavoidable
```

If that works, expected wins are:

- fewer `call` / `ret` pairs per opcode,
- less ABI save/restore overhead,
- better constant folding using embedded `P1/P2/P3/P5`,
- better register allocation inside the stencil,
- removal of redundant loads of `p`, `aMem`, and `pOp` fields,
- shorter critical path for tiny arithmetic and comparison programs.

This is a good direction, but it must be implemented as a measured,
incremental optimization. It is not enough to add `always_inline` and assume
the code gets better.

## 3. Why this may help

For many handlers, the first helper layer is already written in C in a
reasonably self-contained form. Examples:

- [`vdbe_op_integer()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c#L12)
- [`vdbe_op_bool()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c#L24)
- [`vdbe_op_int64()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c#L37)
- [`vdbe_op_eq()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_compare.c#L31)

These functions are small enough that an inliner could plausibly fold:

- register address calculation from `p1/p2/p3`,
- flag checks from `p5`,
- simple `Mem` stores,
- small branch sequences for `jump / fallthrough / error`.

In the best case, the stencil becomes "real opcode code" instead of "opcode
wrapper plus helper call".

## 4. Why this may not help enough

Inlining the first helper level is not automatically sufficient.

There are three important limits:

1. Many handlers still call deep helpers.
   Examples include `mem_cmp()`, cursor navigation, sorter logic, tuple/index
   helpers, transaction helpers, and string conversion helpers.
   Inlining only the top-level handler may still leave most of the work behind
   an external call boundary.

2. The current stencil execution model still pays one function boundary per
   opcode in [`vdbe_cnp_exec()`](/home/tsafin/tarantool/src/box/sql/vdbe_cnp.c#L1455).
   Even perfect helper inlining does not remove the outer
   `indirect call -> return -> decode -> branch` cycle.

3. Aggressive inlining can harm I-cache behavior.
   If every stencil becomes large, overall instruction cache pressure may get
   worse, especially for `sql-tap` and mixed realistic programs.

So this optimization should be treated as one stage in a larger plan, not the
only answer.

## 5. Recommendation on ABI changes

Do not begin with a custom calling convention change such as a Haskell-style
register ABI for stencils.

That idea may become useful later, but it is not the right first step.
Reasons:

- the current system still depends heavily on ordinary C helpers,
- debug/unwind/perf support already works with the current ABI,
- a custom ABI complicates extraction, helper interop, and portability,
- we do not yet know whether the dominant cost is helper ABI overhead or the
  outer per-op dispatch model.

The correct order is:

1. measure the current hot costs,
2. inline first-level helper bodies for selected opcodes,
3. remove per-op call/return overhead where possible,
4. only then evaluate whether a custom internal ABI is still justified.

## 6. High-level optimization strategy

The optimization plan should proceed in four tracks:

1. Measurement
2. First-level helper inlining
3. Dispatch model reduction
4. Register-residency / ABI work if still needed

The tracks are ordered by confidence and engineering cost.

## 7. Benchmark-driven priority order

The initial workload selection should follow the measured opcode mixes from
the existing JIT benchmark harness, not intuition.

### 7.1 First target: `tiny_const / prepared_execute`

This is the best first target for both:

- first-level helper inlining,
- and later dispatch-model reduction.

Measured opcode mix:

- `Integer x2`
- `Add x1`
- `Init x1`
- `Goto x1`
- `ResultRow x1`
- `Halt x1`

Why this is first:

- no cursor activity,
- no table or index access,
- no deep storage helpers,
- almost all cost is dispatch overhead plus tiny arithmetic helpers.

If CnP loses here, the problem is structural:

- per-op call/return overhead,
- ordinary C ABI overhead,
- and helper call overhead for tiny handlers.

Recommended first helper targets:

- [`vdbe_op_integer()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c#L12)
- [`vdbe_op_bool()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c#L24)
- [`vdbe_op_int64()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c#L37)
- [`vdbe_op_real()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c#L52)
- [`vdbe_op_add()`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_arith.c)

### 7.2 Second target: `hot_expr / prepared_execute`

This is the second best target. It is still arithmetic-dominated but has
enough repeated arithmetic to show whether helper inlining scales beyond the
minimal case.

Measured opcode mix:

- `Integer x5`
- `Add x4`
- `Init x1`
- `Goto x1`
- `ResultRow x1`
- `Halt x1`

Why this is second:

- same structural story as `tiny_const`,
- larger arithmetic density,
- still avoids cursor and storage-heavy helpers.

Recommended use:

- use `tiny_const` as the first correctness and dispatch-overhead gate,
- use `hot_expr` as the second correctness and arithmetic-throughput gate.

### 7.3 Third target: `point_lookup / prepared_execute`

This should not be the first helper-inlining target. It is a later validation
workload for mixed helper-heavy paths.

Measured opcode mix includes:

- `IteratorOpen`
- `OpenSpace`
- `Variable`
- `SeekGE`
- `IsNull`
- `Column x3`
- `Add`
- `Subtract`
- `Multiply`
- `Divide`
- `Remainder`
- `Next`
- `ResultRow`
- `Halt`

Why this is third:

- dominated by cursor/data-access helpers,
- much more likely to be bottlenecked by deep helper stacks,
- less useful for isolating pure dispatch cost.

This workload should be used after the arithmetic and constant paths are
improved, to determine whether the remaining losses come from:

- `Column`,
- seek/navigation helpers,
- or the still-unfixed dispatch model.

### 7.4 Recommended implementation order

The concrete order should be:

1. `tiny_const / prepared_execute`
2. `hot_expr / prepared_execute`
3. `point_lookup / prepared_execute`

And the code order should be:

1. constant loaders in
   [`vdbe_ops_data.c`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_data.c)
2. arithmetic helpers in
   [`vdbe_ops_arith.c`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_arith.c)
3. only then cursor/data helpers:
   - [`vdbe_ops_cursor_data.c`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_cursor_data.c)
   - [`vdbe_ops_cursor_seek.c`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_cursor_seek.c)
   - [`vdbe_ops_cursor_nav.c`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_cursor_nav.c)
   - [`vdbe_ops_index.c`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_index.c)

## 8. Initial O2 results: first helper layer inlined

The first implementation wave is now in place for:

- `OP_Integer`
- `OP_Bool`
- `OP_Int64`
- `OP_Add`

Implementation shape:

- top-level helpers now forward to shared `*_impl()` bodies in
  [`vdbe_ops_cnp_impl.h`](/home/tsafin/tarantool/src/box/sql/vdbe_ops_cnp_impl.h)
- the CnP stub generator emits direct calls to these `*_impl()` bodies instead
  of loading `HOLE_HANDLER` and performing an indirect helper call

This was validated by inspecting the generated CnP stubs:

- `cnp_OP_Integer()` now calls `vdbe_op_integer_impl()` directly
- `cnp_OP_Add()` now calls `vdbe_op_add_impl()` directly
- the old `HOLE_HANDLER` load is gone for these opcodes

Measured result in the `build` profiling configuration:

- `tiny_const / prepared_execute`
  - before: about `2.914 us/op`
  - after: about `2.837 us/op`
  - effect: modest improvement, about `2.6%`
- `hot_expr / prepared_execute`
  - before: about `3.257 us/op`
  - after: about `3.328 us/op`
  - effect: no improvement in this run, slightly worse

Interpretation:

- removing the indirect handler load and call is real, but not sufficient
- the inlined stencils still call lower-level helpers such as
  `vdbe_prepare_null_out()`, `mem_set_int()`, and `mem_add()`
- so the first-wave change removes only one function boundary, not the full
  helper stack

This is an important result. It suggests the next gains are more likely to
come from one of these:

1. inline lower-level tiny helpers used by constants and arithmetic
2. convert common-case stencil transitions from `call/ret` to direct chaining
3. add fused arithmetic superinstructions before widening the inline surface

It does not justify a custom ABI change yet.

## 8.1 Second O2 wave: full scalar arithmetic family

The next implementation wave extends the same pattern to:

- `OP_Subtract`
- `OP_Multiply`
- `OP_Divide`
- `OP_Remainder`

That gives the CnP generator direct `*_impl()` calls for the full scalar
arithmetic family:

- `Add`
- `Subtract`
- `Multiply`
- `Divide`
- `Remainder`

Measured result in the `build` profiling configuration:

- `hot_expr / prepared_execute`
  - before second wave: about `3.328 us/op`
  - after second wave: about `3.261 us/op`
  - effect: recovered the first-wave regression and returned close to the
    earlier baseline
- `point_lookup / prepared_execute`
  - before second wave: about `7.784 us/op`
  - after second wave: about `7.662 us/op`
  - effect: modest improvement, about `1.6%`

Interpretation:

- widening the direct-impl surface helps the mixed arithmetic path more than
  the pure `Add` micro-benchmark
- `point_lookup` benefits because it actually executes all five arithmetic
  operators
- the overall gain is still modest because these stencils continue to call
  low-level `mem_*()` helpers and still return to the outer CnP dispatch loop

So the direction is valid, but the next larger gain is still more likely to
come from:

1. lower-level helper specialization
2. direct stencil chaining
3. fused arithmetic superinstructions

## 8.3 First O5 result: direct stencil chaining

The first direct-chaining wave is now implemented in the stencil generator for
the common straight-line cases:

- `none` handlers now chain through `HOLE_NEXT`,
- unconditional branches now chain through `HOLE_BRANCH`,
- conditional branches chain through `HOLE_BRANCH` or `HOLE_NEXT`,
- seek-style branch handlers chain on their common next/branch paths.

The generator now emits a shared helper:

- `cnp_chain_or_ret(target, p, aMem)`

Its contract is:

- if `target >= CNP_ADDR_THRESHOLD`, treat it as a patched stencil address and
  jump to it directly,
- otherwise return the tagged value to `vdbe_cnp_exec()` as before.

This preserves the existing runtime contract for:

- `SQL_ROW`,
- `SQL_DONE`,
- fatal error returns,
- coroutine PC jumps,
- special paths such as `ResultRow` and skip encodings.

The key structural difference is in the generated machine code. For example,
the `OP_Integer` and `OP_Add` stencils now end their hot path with:

- reload patched `HOLE_NEXT`,
- compare against `CNP_ADDR_THRESHOLD`,
- restore the local frame,
- `jmpq *%rax` on address targets,
- `retq` only for non-address tagged returns.

So the common straight-line path no longer returns to
[`vdbe_cnp_exec()`](/home/tsafin/tarantool/src/box/sql/vdbe_cnp.c#L1462)
between opcodes. That removes the outer:

- stencil return,
- tagged-result decode in C,
- re-entry call to the next stencil.

Measured result in the `build` profiling configuration:

- `tiny_const / prepared_execute`
  - before chaining: about `2.837 us/op`
  - after chaining: about `2.676 us/op`
  - effect: clear improvement, about `5.7%`
- `hot_expr / prepared_execute`
  - before chaining: about `3.261 us/op`
  - after chaining: about `3.225 us/op`
  - effect: modest improvement, about `1.1%`
- `point_lookup / prepared_execute`
  - before chaining: about `7.662 us/op`
  - after chaining: about `7.642 us/op`
  - effect: small improvement, about `0.3%`
- `bitwise_mix / prepared_execute`
  - before chaining: about `7.138 us/op`
  - after chaining: about `7.125 us/op`
  - effect: effectively flat, slight improvement

Interpretation:

- chaining helps most where the program is dominated by tiny straight-line
  stencils,
- the gain shrinks as deeper helpers and cursor work dominate,
- this confirms that the outer return-to-C loop was a real cost center,
- but it is not yet the only remaining cost center.

This is the first structural optimization that moves CnP in the direction of
threaded execution without rewriting the entire runtime around a new ABI or a
fully custom threaded-code interpreter.

## 8.2 Broader first-level helper split

The implementation strategy now uses two classes of shared `*_impl()` bodies:

1. stub-safe impls
2. wrapper-only impls

Stub-safe impls may be called directly from generated CnP stencils. They must
avoid pulling the stencil compiler through broad SQL frontend dependencies such
as `sqlInt.h`, error-reporting macros, and large type-definition graphs.

Wrapper-only impls are still useful even when they are not yet safe for direct
stencil inclusion. They make the extracted opcode helpers structurally ready
for later inlining and reduce duplicated logic in the first helper layer.

This split is now applied to additional helper families:

- logical and bitwise handlers
- comparison handlers
- `OffsetLimit`
- `Concat`
- simple type-conversion helpers such as `MustBeInt`, `Cast`, and `ApplyType`

Only the stub-safe subset is mapped into the CnP generator today. The broader
set is intentionally limited to shared wrapper impls for now.

That keeps the next optimization steps honest:

- expand direct stencil calls only for helpers that are actually dependency-safe
- continue moving duplicated first-level logic into shared impl bodies
- do not reintroduce the header-coupling problem that previously broke the
  stencil build

## 9. Current stencil execution schema

The current CnP schema is intentionally conservative:

- each stencil is a normal function,
- stencils use the ordinary C ABI,
- each stencil returns a tagged `int64_t`,
- the C loop in [`vdbe_cnp_exec()`](/home/tsafin/tarantool/src/box/sql/vdbe_cnp.c#L1462)
  decodes that return and dispatches the next stencil.

This schema was chosen first because it is:

- easy to generate from extracted machine-code stencils,
- easy to integrate with existing C helpers,
- easy to debug and unwind,
- easy to keep correct while CnP coverage is still expanding.

It also made mixed execution easier:

- a stencil can return a terminal status,
- a stencil can encode a coroutine PC jump,
- the runtime can save resume state for `ResultRow`,
- and the native path can fall back cleanly when needed.

The cost is that every opcode currently pays:

- one indirect function call,
- one function return,
- one tagged-result decode,
- one branch to select the next action.

That is exactly why the current schema is a good first implementation but not
the final performance model.

## 10. Preferred control-flow model

The preferred future CnP model is no longer "function-per-stencil with a
better chain helper". The preferred model is:

- use the existing threaded interpreter as the source of opcode fragments,
- extract opcode bodies from that threaded dispatch loop,
- stitch copied fragments in VDBE order,
- remove the terminal threaded dispatch jump on straight-line fallthrough,
- keep explicit exits only for true control-flow, row, coroutine, and error
  transitions.

The straight-line hot path should therefore look like:

- fragment for opcode A,
- fallthrough into fragment for opcode B,
- fallthrough into fragment for opcode C,
- exit only when semantics require it.

That directly removes the remaining structural costs still visible in the
wrapper-stencil disassembly:

- per-op prologue,
- per-op epilogue,
- per-op indirect jump between normal straight-line handlers.

This is the first design that can plausibly outperform the threaded
interpreter on tiny and arithmetic-heavy VDBE programs without introducing a
custom ABI.

## 11. Review of the generated threaded dispatcher as a CnP source

The current repository already has two relevant dispatcher representations:

1. the production threaded interpreter path in
   [`vdbe.c`](/home/tsafin/tarantool/src/box/sql/vdbe.c)
   using:
   - `dispatchtable.h`,
   - `&&Exec_OP_*` label addresses,
   - `DISPATCH()` and `JUMP_P2()` macros,
   - label-based control flow inside one large interpreter body
2. the generated reference source in
   [`generated/vdbe_dispatch_generated.c`](/home/tsafin/tarantool/src/box/sql/generated/vdbe_dispatch_generated.c)

The important conclusion is:

- the real threaded execution model is the one embedded in
  [`vdbe.c`](/home/tsafin/tarantool/src/box/sql/vdbe.c),
- the generated reference file is useful as a source-generation model,
  but it is not currently the compiled runtime artifact used directly for
  dispatch.

### 11.1 What is compatible with CnP stitching

From the CnP point of view, the threaded dispatcher is compatible in these
important ways:

- opcode handlers already have addressable labels via `&&Exec_OP_*`,
- dispatch is already expressed as label-to-label control flow,
- many hot opcodes end in a regular threaded `DISPATCH()` edge,
- operand access still uses the usual VDBE frame state:
  - `p`,
  - `aOp`,
  - `aMem`,
  - `pOp`,
  - `P1/P2/P3` aliases
- helper calls remain ordinary C calls inside the handler body

That means the threaded interpreter already has the right semantic shape for
copy-and-patch stitching:

- copy body fragments,
- patch embedded operands and helper references,
- turn straight-line dispatch into fallthrough,
- keep non-linear exits explicit.

### 11.2 What is not compatible today

The current implementation is not directly extractable by the existing CnP
toolchain for two concrete reasons:

1. The current extractor only understands ordinary ELF symbol ranges.
   It scans `.symtab` for `cnp_OP_*` function symbols in
   [`vdbe_cnp_extract.py`](/home/tsafin/tarantool/tools/vdbe_cnp_extract.py).
   It does not yet consume addressable label tables from the threaded
   interpreter.

2. The threaded handlers do not currently expose explicit extraction
   boundaries.
   For stitching we need, per opcode:
   - fragment begin,
   - terminal dispatch edge,
   - fragment end.
   The current `Exec_OP_*` labels give the start, but not yet a robust,
   explicit "cut here" label for the terminal dispatch sequence.

This is an implementation problem, not a design problem. The threaded source
is still the preferred source for CnP fragments.

### 11.3 Handler classes for the first migration

The threaded dispatcher is not equally easy to stitch across all opcode
families.

Best first candidates:

- `Integer`
- `Bool`
- `Int64`
- `Add`
- `Subtract`
- `Multiply`
- `Divide`
- `Remainder`
- `Goto`
- `IfNot`
- `Once`

These handlers have relatively regular endings:

- helper call or small inline logic,
- error branch if needed,
- `JUMP_P2()` or `DISPATCH()`.

Harder classes that should stay out of the first pilot:

- `ResultRow`
- `Halt`
- coroutine ops:
  - `Gosub`
  - `Return`
  - `InitCoroutine`
  - `Yield`
  - `EndCoroutine`
- seek/skip ops with multi-way control flow
- handlers that restore frames or mutate interpreter-global control state

These should remain explicit exits or later special cases.

## 12. Why alternative approaches are worse

The following alternatives were discussed and are intentionally not the
preferred path.

### 12.1 Keep the wrapper-stencil model and optimize chaining further

This is what the current `cnp_OP_*` wrappers do.

Why it helped:

- it removed the outer `ret -> C decode -> call next stencil` cycle,
- it produced measurable wins on `tiny_const` and modest wins elsewhere.

Why it is still not the preferred final design:

- every intermediate stencil still has a function prologue,
- every intermediate stencil still has a function epilogue,
- every intermediate stencil still performs an indirect jump,
- the "next opcode" is still the entry of another standalone function body.

So even with direct chaining, the wrapper-stencil model retains exactly the
kind of repeated boundaries that the original threaded CnP design is supposed
to remove.

Observed wrapper-stencil disassembly from
`build/src/box/sql/generated/vdbe_cnp_stubs.o` makes this concrete.

`cnp_OP_Integer`:

```asm
0000000000001570 <cnp_OP_Integer>:
    1570: push   %rbp
    1571: mov    %rsp,%rbp
    1574: push   %r15
    1576: push   %r14
    1578: push   %rbx
    1579: push   %rax
    ...
    158e: callq  vdbe_prepare_null_out
    ...
    159f: callq  mem_set_int
    ...
    15a4: movabs $0x0,%rax        # HOLE_NEXT
    15ae: cmp    $0x1000,%rax
    15b4: jb     15c8
    15b6: mov    %r14,%rdi
    15b9: mov    %rbx,%rsi
    15bc: add    $0x8,%rsp
    15c0: pop    %rbx
    15c1: pop    %r14
    15c3: pop    %r15
    15c5: pop    %rbp
    15c6: jmpq   *%rax
    15c8: add    $0x8,%rsp
    15cc: pop    %rbx
    15cd: pop    %r14
    15cf: pop    %r15
    15d1: pop    %rbp
    15d2: retq
```

`cnp_OP_Add`:

```asm
0000000000000db0 <cnp_OP_Add>:
     db0: push   %rbp
     db1: mov    %rsp,%rbp
     db4: push   %r14
     db6: push   %rbx
     ...
     de8: callq  mem_add
     ...
     dfd: movabs $0x0,%rax        # HOLE_NEXT
     e07: cmp    $0x1000,%rax
     e0d: jb     e1b
     e0f: mov    %r14,%rdi
     e12: mov    %rbx,%rsi
     e15: pop    %rbx
     e16: pop    %r14
     e18: pop    %rbp
     e19: jmpq   *%rax
     e1b: pop    %rbx
     e1c: pop    %r14
     e1e: pop    %rbp
     e1f: retq
```

`cnp_OP_Goto`:

```asm
0000000000001a00 <cnp_OP_Goto>:
    1a00: push   %rbp
    1a01: mov    %rsp,%rbp
    1a04: movabs $0x0,%rax        # HOLE_BRANCH
    1a0e: cmp    $0x1000,%rax
    1a14: jb     1a19
    1a16: pop    %rbp
    1a17: jmpq   *%rax
    1a19: pop    %rbp
    1a1a: retq
```

`cnp_OP_ResultRow`:

```asm
0000000000000240 <cnp_OP_ResultRow>:
     240: push   %rbp
     241: mov    %rsp,%rbp
     244: push   %rbx
     245: push   %rax
     ...
     260: callq  *%rax            # HOLE_HANDLER
     ...
     268: movabs $0x0,%rax        # HOLE_SIGNAL
     275: callq  *%rax
     277: movabs $0x0,%rax        # HOLE_NEXT
     ...
     28d: add    $0x8,%rsp
     291: pop    %rbx
     292: pop    %rbp
     293: retq
```

These snippets show the remaining structural problem directly:

- even on the chained hot path, each opcode still begins with a normal
  function entry sequence,
- each opcode still ends with a normal teardown sequence,
- the stitched successor is still entered as another standalone function body,
- row-producing paths still return through explicit function exit paths.

So the current chaining wave removes the return to `vdbe_cnp_exec()`, but it
does not yet remove the repeated function-shape boundaries between normal
straight-line opcodes.

### 12.2 Remove prologues and epilogues from current wrapper stencils

This idea is attractive in the abstract, but it does not fit the current
extraction model.

The current wrapper stencils are extracted as whole function symbols. As long
as the unit of extraction is "one ordinary function", prologue and epilogue
bytes are part of the extracted unit.

That means they cannot be removed safely by patching addresses alone.
Eliminating them correctly would require:

- a different extractable code unit than "whole function",
- or separate entry/body/exit fragments,
- or a dedicated fragment source.

At that point the design has already moved away from wrapper stencils and
toward threaded fragments, which is the preferred plan anyway.

### 12.3 Use `__attribute__((naked))`

`naked` is worse because it attacks the symptom at the wrong layer.

It would force the generated code to take over responsibilities that the
compiler currently handles correctly:

- stack alignment,
- register preservation,
- ABI-correct calls into helper functions,
- local spill management,
- unwind/debug behavior.

That is especially inappropriate here because opcode bodies still do normal C
work:

- compute `Mem *` addresses,
- call helpers such as `mem_add()` and `vdbe_prepare_null_out()`,
- branch on helper return values,
- interact with normal VDBE state.

So `naked` would push the generator toward handwritten ABI management and
assembly-like code emission. That is a worse engineering trade-off than
generating extractable threaded fragments from ordinary C.

### 12.4 Rely on compiler tail calls or `musttail`

This is weaker than threaded-fragment stitching for two reasons:

1. tail-call formation is compiler-sensitive and ABI-sensitive,
2. even a perfect tail-call model still treats each opcode as a standalone
   function-shaped unit.

That means tail calls can remove some return overhead, but they still do not
give the clean straight-line fallthrough model that stitched threaded
fragments provide.

Tail-call chaining was a valid incremental experiment. It is not the preferred
end state.

## 13. Preferred implementation plan

The project should now pivot from "better wrapper stencils" to
"threaded-fragment extraction and stitching".

### 13.1 Generate a dedicated fragment-production artifact

Do not use the shipping wrapper-stencil object as the primary future source.
Instead, generate a dedicated compiled artifact for fragment extraction from
the same threaded dispatch logic.

Preferred form:

- an intermediate object file dedicated to fragment extraction

This artifact should not be used as a production runtime path. Its job is only
to:

- compile the threaded handler bodies,
- expose stable extraction metadata,
- allow deterministic fragment extraction.

### 13.2 Emit explicit label metadata

For each opcode handler, emit explicit labels:

- `Exec_OP_X_begin`
- `Exec_OP_X`
- `Exec_OP_X_dispatch`
- `Exec_OP_X_end`

and companion metadata tables containing addressable labels:

- begin table
- dispatch table
- end table

Using `&&label` is sufficient for addressability. The important requirement is
that extraction metadata be explicit rather than inferred heuristically from
whole-function symbols.

### 13.3 Extract body fragments, not functions

The extractor should pivot from:

- `cnp_OP_*` function symbol extraction

to:

- threaded fragment extraction from `begin` to `dispatch` or `end`

Fragment classes:

1. fallthrough-capable
2. branch-capable
3. terminal
4. coroutine/resume

For straight-line handlers, the terminal threaded dispatch sequence becomes the
cut point.

### 13.4 Stitching rules

For stitched programs:

- if handler A is followed by handler B in straight-line execution,
  copy A's fragment body and lay B immediately after it,
- replace or drop the terminal threaded dispatch edge from A so execution
  falls through into B,
- preserve explicit control transfers for:
  - branches,
  - `ResultRow`,
  - `Halt`,
  - coroutine transitions,
  - error exits.

The practical rule is not "always nop the last jump". The rule is:

- remove only the terminal dispatch edge for contiguous straight-line
  successors,
- preserve semantic control-flow exits.

### 13.5 First pilot set

The initial threaded-fragment pilot should cover:

- `Init`
- `Integer`
- `Bool`
- `Int64`
- `Add`
- `Subtract`
- `Multiply`
- `Divide`
- `Remainder`
- `Goto`
- `ResultRow`
- `Halt`

The first stitched success criteria are:

- straight-line arithmetic fragments show no per-op prologue/epilogue in the
  final native code,
- straight-line arithmetic fragments fall through into each other,
- row and halt paths still return correctly,
- GDB/perf naming remains usable at the statement/function level,
- `tiny_const` and `hot_expr` improve beyond the wrapper-chaining baseline.

## 12. Track A: measurement before optimization

Before changing code generation, add enough instrumentation to explain where
CnP time is going.

### A.1 Runtime counters

Add CnP-only profiling counters for:

- total CnP steps,
- steps by opcode,
- CnP exec loop overhead,
- helper call count,
- helper call time,
- coroutine/pc-jump count,
- row-resume count,
- fallback count,
- average bytes per stencil for executed opcodes.

Some of these can be sampled rather than counted exactly if overhead becomes
visible.

### A.2 Cycle accounting buckets

For microbench builds, add optional timing buckets:

- `cnp_dispatch_cycles`
  time spent in the outer `vdbe_cnp_exec()` loop outside the helper body
- `cnp_helper_cycles`
  time spent inside first-level opcode helper calls
- `cnp_resume_cycles`
  time spent on row-ready save/resume path
- `cnp_pc_jump_cycles`
  time spent resolving coroutine PC jumps

This should be behind a debug or benchmark build flag so it does not pollute
normal builds.

### A.3 Hardware-counter runs

For the benchmark harness, collect `perf stat` for:

- `cycles`
- `instructions`
- `branches`
- `branch-misses`
- `icache` / `iTLB` misses where available

Record:

- interpreter,
- generated dispatcher,
- CnP,
- and later "CnP with helper inlining".

The goal is to compare:

- cycles per VDBE step,
- instructions per VDBE step,
- branch misses per VDBE step.

### A.4 Focused synthetic workloads

Add tiny one-shape benchmarks for:

- `Integer`
- `Add`
- `Eq`
- `ResultRow`
- `Next`
- `Column`

These must isolate:

- trivial pure arithmetic,
- comparison + jump,
- row production,
- cursor navigation,
- data extraction.

Without these focused cases it will be hard to tell whether a change helps
only arithmetic or the real mixed path.

## 13. Track B: first-level helper inlining

This is the first code-generation experiment to implement.

### B.1 Goal

For selected hot opcodes, compile the stencil so that the body of the handler
itself is present in the stencil machine code, not called through an external
symbol.

### B.2 Candidate opcode groups

Start with handlers most likely to benefit and least likely to explode code
size:

1. Constants and register moves
- `OP_Integer`
- `OP_Bool`
- `OP_Int64`
- `OP_Real`
- `OP_Null`
- `OP_Copy`
- `OP_SCopy`

2. Small arithmetic/logical ops
- `OP_Add`
- `OP_Subtract`
- `OP_Multiply`
- `OP_Divide`
- `OP_Remainder`
- `OP_And`
- `OP_Or`
- `OP_Not`
- `OP_BitAnd`
- `OP_BitOr`
- `OP_BitNot`

3. Small conditional ops
- `OP_IsNull`
- `OP_NotNull`
- `OP_If`
- `OP_IfNot`
- `OP_IfPos`
- `OP_IfNotZero`
- `OP_DecrJumpZero`

4. Comparison ops as a second wave
- `OP_Eq`
- `OP_Ne`
- `OP_Lt`
- `OP_Le`
- `OP_Gt`
- `OP_Ge`

Comparison ops are more complex because they may still depend on `mem_cmp()`.
They should be a second wave, not the first.

### B.3 Code organization model

Do not mark the existing public helper functions `always_inline` blindly.

Instead, split hot helpers into two layers:

1. internal inline implementation
2. stable out-of-line wrapper

Example shape:

```c
static inline int
vdbe_op_integer_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
    ...
}

int
vdbe_op_integer(Vdbe *p, Op *pOp, Mem *aMem)
{
    return vdbe_op_integer_impl(p, pOp, aMem);
}
```

The generated dispatcher and normal C code can keep calling
`vdbe_op_integer()`.

The CnP stub generator should include or reference `vdbe_op_integer_impl()`
directly when building the stencil wrapper, so the optimizer can inline it
into the stencil.

This preserves:

- readable source structure,
- stable non-CnP call sites,
- clear control over which bodies are inlinable,
- and avoids forcing all users of the helper to duplicate code.

### B.4 Do not inline everything

Mark only the implementation-layer helpers needed by the CnP stub build as
strongly inline candidates.

Acceptable options:

- `static inline`
- `static __attribute__((always_inline)) inline`

Use `always_inline` only where measurement shows the compiler otherwise
refuses to inline a clearly profitable body.

Avoid applying `always_inline` to:

- cursor-heavy handlers,
- sorter handlers,
- transaction handlers,
- handlers with large loops,
- handlers calling deep subsystems,
- anything already known to produce large code.

### B.5 Separate CnP build flavor if needed

If the normal build should not see aggressive inlining attributes, introduce a
CnP-specific internal macro, for example:

```c
#ifdef VDBE_CNP_STUB_BUILD
#define VDBE_CNP_INLINE static __attribute__((always_inline)) inline
#else
#define VDBE_CNP_INLINE static inline
#endif
```

Then use that macro for the implementation-layer helpers consumed by CnP
stub generation.

This keeps the aggressive policy local to stencil generation rather than
infecting the whole SQL build.

### B.6 Stub generator changes

The CnP stub generator should support two modes per opcode:

1. call-helper mode
2. inline-impl mode

This can be modeled with a per-opcode annotation in the generation metadata.

Suggested metadata:

```yaml
cnp_codegen:
  mode: inline_impl | call_helper | custom
  impl_symbol: vdbe_op_integer_impl
```

This avoids heuristic symbol rewriting in the extraction script.

### B.7 Validation criteria

For each migrated opcode:

- disassemble the resulting stencil,
- verify the top-level helper call is gone,
- confirm code size does not grow unreasonably,
- compare cycles/op against the baseline stencil.

Success means:

- helper call removed from the stencil,
- cycles/op reduced for the targeted microbench,
- no regression in correctness,
- no major regression in code size or i-cache behavior.

## 14. Track C: reduce outer CnP dispatch overhead

Even if helper bodies are inlined, the current `vdbe_cnp_exec()` loop still
imposes per-op dispatch overhead.

This track should begin after the first helper-inlining measurements are in.

### C.1 Replace per-op call/return chaining

Current model:

- stencil returns next address or tagged status,
- C loop decodes and calls again.

Preferred model for common case:

- stencil ends by jumping directly to the next stencil,
- only terminal and exceptional paths return to C.

That change targets the largest remaining structural cost:

- indirect call,
- function return,
- tagged decode,
- row-ready polling,

paid once per opcode today.

### C.2 Superinstructions

Once direct chaining exists, add fused forms for common short sequences:

- const load + const load + arithmetic,
- compare + branch,
- load + resultrow,
- loop-step + branch.

This reduces both:

- dispatch count,
- and repeated address/load setup.

### C.3 Row-ready signaling

Today [`vdbe_cnp_exec()`](/home/tsafin/tarantool/src/box/sql/vdbe_cnp.c#L1495)
checks `p->cnp_row_ready` on every step.

That should eventually be replaced by a dedicated return/tagged terminal path
used only by `OP_ResultRow`-like stencils, not polled globally.

## 15. Track D: ABI and register residency

Only after Tracks B and C should the project revisit ABI changes.

If CnP still loses after helper inlining and direct chaining, then measure
whether fixed-register residency is the missing piece.

Potential goals for a later internal ABI:

- keep `p` in a fixed callee-saved register,
- keep `aMem` in a fixed register,
- keep hot scratch or current-register base in another register,
- minimize reloads across fused opcode sequences.

But this must stay an internal stencil ABI only.

External helper boundaries should remain ordinary C ABI until there is strong
evidence that changing them is worth the complexity.

## 16. Concrete phased plan

### Phase O1: establish measurement baseline

Deliverables:

- CnP profiling counters and optional timing buckets
- focused microbench cases for small opcode shapes
- perf-stat collection script or note

Exit criteria:

- clear cycles/op comparison for interpreter vs CnP
- identified top 5 hottest opcode families by executed-step count and cost

### Phase O2: inline constant/move handlers

Scope:

- `Integer`, `Bool`, `Int64`, `Real`, `Null`, `Copy`, `SCopy`

Deliverables:

- internal `*_impl()` split where needed
- stub generator metadata for `inline_impl`
- disassembly proof that helper calls disappeared

Exit criteria:

- measurable win on constant-heavy synthetic benchmarks
- no code-size blow-up in ordinary query shapes

### Phase O3: inline arithmetic and small conditionals

Scope:

- arithmetic, logical, and simple branch opcodes

Exit criteria:

- `tiny_const` and `hot_expr` move closer to or below interpreter throughput
- branch misses/op do not regress badly

### Phase O4: evaluate comparison handlers

Scope:

- comparison family

Notes:

- likely partial win only
- deep helper `mem_cmp()` may remain the dominant cost

Exit criteria:

- evidence whether top-level comparison-helper inlining is worthwhile

### Phase O5: direct stencil chaining

Scope:

- remove per-op return to `vdbe_cnp_exec()` for common paths

Status:

- first wave implemented for straight-line `HOLE_NEXT` and branch transitions
- row-producing, coroutine, and special tagged-return paths still return to C

Exit criteria:

- CnP dispatch cycles/op drop substantially
- row-producing and coroutine paths remain correct

### Phase O6: superinstructions

Scope:

- 3 to 6 high-frequency fused sequences only

Exit criteria:

- reduced step count on arithmetic-heavy and row-output-heavy paths

### Phase O7: decide on internal ABI changes

Only do this if:

- helper inlining is already in place,
- direct chaining is already in place,
- and CnP is still meaningfully behind interpreter for hot reused programs.

## 17. Risks

### R.1 Code size explosion

Inlining too much can make the stencil library large and the generated program
less cache-friendly.

Mitigation:

- start with tiny helpers,
- record bytes/stencil and total program size,
- reject migrations that grow size without runtime wins.

### R.2 Divergence between helper wrapper and inline implementation

Splitting helpers into `*_impl()` plus wrapper can create maintenance drift.

Mitigation:

- wrapper should be a one-line forwarder,
- put the actual logic in one place only,
- add comments that the impl is shared by CnP codegen and normal helper path.

### R.3 Hidden deep-helper cost

Inlining the first layer may not change much for opcodes dominated by lower
layers.

Mitigation:

- measure per opcode family,
- do not generalize from arithmetic to cursor or sorter paths.

### R.4 Optimizer instability

The exact extracted stencil machine code may depend on compiler version and
flags.

Mitigation:

- keep disassembly checks in the optimization workflow,
- treat unknown relocation/codegen patterns as hard failures,
- document the supported compiler/toolchain range.

## 18. Practical conclusion

Inlining the first helper layer into stencils is a good next optimization
experiment and should be implemented.

It is likely to help for:

- constants,
- arithmetic,
- small logical ops,
- simple conditionals.

It is not, by itself, the full answer to CnP underperforming the interpreter.
The broader execution model still has too much per-op dispatch overhead.

The recommended order is:

1. measure current costs,
2. start with `tiny_const`, then `hot_expr`, then `point_lookup`,
3. inline first-level hot helpers into stencils,
4. validate codegen quality with disassembly and perf counters,
5. reduce outer dispatch overhead with direct chaining and superinstructions,
6. only then reconsider a custom internal ABI.

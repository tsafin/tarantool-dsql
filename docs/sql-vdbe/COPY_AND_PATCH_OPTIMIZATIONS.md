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

Do not begin the threaded-fragment conversion with a custom calling
convention change such as a Haskell-style register ABI.

This is more important for the stitched-fragment design than it was for the
wrapper-stencil design. A calling convention governs function boundaries, but
the preferred CnP model is specifically trying to stop treating opcode
handlers as ordinary standalone functions. The hot path should be:

- fragment A body,
- fallthrough into fragment B body,
- fallthrough into fragment C body,
- explicit exit only when semantics require it.

So the first ABI question is not "which function ABI should we use?" but:

- which values must be stable live-ins at fragment entry,
- where do those values live,
- and how do we make that stable enough for extraction and stitching?

For the threaded-fragment model, the practical candidates are:

1. explicit state in memory or globals
2. a dedicated fragment-production source with controlled live-ins
3. only later, a custom register ABI if it is still justified

A custom function calling convention does not by itself stabilize raw labels
cut from one large threaded interpreter body. The compiler still owns
register allocation, spilling, and temporary lifetime inside that body.

That is why the correct order is:

1. measure the current hot costs,
2. switch from wrapper stencils to stitched threaded fragments,
3. make fragment live-ins explicit and stable,
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

## 10.1 Clang 19 internal ABI direction: `preserve_none` and `musttail`

The current paper-style fragment pilot is now correct on `hot_expr`, but the
remaining gap against the generated interpreter suggests that the next step
should be an explicit internal ABI for native execution rather than more
incremental getter/prologue tweaks.

Recent evaluation of CPython's JIT work is relevant here. CPython moved away
from an LLVM IR rewrite for `ghccc` and now uses Clang-supported attributes:

- `__attribute__((preserve_none))`
- `__attribute__((musttail))`

That combination gives CPython a compiler-supported internal continuation ABI:

- arguments live in callee-saved registers instead of the normal SysV argument
  registers,
- tail transitions are guaranteed rather than left to normal optimization,
- the JIT can chain native targets without accumulating stack frames.

Local testing with `clang-19` confirms that `preserve_none` is now available on
our x86_64 Linux environment. For a simple integer test, Clang 19 placed
arguments in callee-saved registers (`r12`, `r13`, `r14`, `r15`) rather than in
the ordinary SysV argument registers. That makes it a realistic foundation for
an explicit CnP live-in ABI.

### Proposed internal register contract

For the first serious ABI-controlled prototype, the natural live-ins are:

- `r12 = p`
- `r13 = aOp`
- `r14 = pOp`
- `r15 = aMem`

The exact mapping may still change, but the principle is important:

- native CnP execution should enter once through a small normal-C shim,
- the hot path should then run under a stable internal ABI with fixed live-ins,
- row/done/error exits should be explicit boundaries back to ordinary C,
- straight-line opcode chains should not reload these values through helper
  getters or rediscover them from stack slots on every native entry.

This is much closer to the intended copy-and-patch model than the current
getter-based fragment prologue.

### What `preserve_none` solves

`preserve_none` is useful because it gives the compiler a stable function ABI
for internal native targets:

- entry shims,
- continuation functions,
- row/done/error bridges,
- possibly PC-jump helpers.

This is especially attractive for resume-heavy paths because it can make the
native continuation ABI explicit without relying on fragile compiler accidents
inside one large extracted object.

### What `preserve_none` does not solve by itself

`preserve_none` is still a function calling convention. By itself it does **not**
guarantee that arbitrary copied labels cut from a compiled function body can be
treated as independently valid fragment entries with the same stable live-ins.

That distinction matters for our paper-style stitched fragments:

- copied basic blocks with patched direct edges want explicit ownership of the
  live-in register contract,
- arbitrary extracted labels still depend on how the compiler shaped the
  surrounding function.

So `preserve_none` should be viewed as a strong tool for building a controlled
internal ABI, but not as a magical replacement for explicit fragment-ABI
design.

### Role of `musttail`

`musttail` is valuable, but mainly for function-shaped continuation boundaries.

It is a good fit for:

- row resume continuations,
- done/error exits that re-enter native code,
- explicit continuation trampolines,
- temporary function-per-fragment experiments built around one internal ABI.

It is less central for the final copied-fragment hot path itself, because a
stitched fragment layout already prefers:

- physical fallthrough for straight-line execution,
- direct patched jumps for internal control-flow edges.

For that final hot path, a patched local `jmp` is simpler and cheaper than a
function boundary, even a guaranteed tail-call one.

So the near-term mixed model should be:

1. use `preserve_none` to define the internal CnP ABI,
2. use `musttail` where native execution must cross explicit continuation
   boundaries,
3. keep the hottest straight-line interior in copied fragments with direct
   patched edges rather than turning everything back into ordinary functions.

### Practical build implication

Because this depends on real `preserve_none` support, the CnP build pipeline
should stop preferring `clang-16` once the ABI-controlled prototype begins.
`clang-19` (or newer) should be treated as the intended toolchain for this
phase of the work.

## 11. Review of the generated threaded dispatcher as a CnP source

The current repository has two relevant threaded-dispatch representations:

1. the generated threaded dispatcher in
   [`generated/vdbe_dispatch_generated.c`](/home/tsafin/tarantool/src/box/sql/generated/vdbe_dispatch_generated.c)
   which is the real validated threaded-dispatch source used as the basis
   for later JIT work
2. the manually maintained threaded path in
   [`vdbe.c`](/home/tsafin/tarantool/src/box/sql/vdbe.c)
   using:
   - `dispatchtable.h`,
   - `&&Exec_OP_*` label addresses,
   - `DISPATCH()` and `JUMP_P2()` macros,
   - label-based control flow inside one large interpreter body

The important conclusion is:

- the generated threaded dispatcher is the primary source of truth for the
  threaded execution model that CnP should target,
- the manually written threaded path in
  [`vdbe.c`](/home/tsafin/tarantool/src/box/sql/vdbe.c) remains useful for
  backward compatibility and verification while the migration completes,
- long term, the generated threaded dispatcher is the one we want to extract
  fragments from and keep aligned with JIT execution.

### 11.1 What is compatible with CnP stitching

From the CnP point of view, the generated threaded dispatcher is compatible in
these important ways:

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

This is an implementation problem, not a design problem. The generated
threaded dispatcher is still the preferred source for CnP fragments, and the
manual path should be treated as a verification aid rather than the extraction
target.

### 11.3 Handler classes for the first migration

An automated scan of
[`generated/vdbe_dispatch_generated.c`](/home/tsafin/tarantool/src/box/sql/generated/vdbe_dispatch_generated.c)
found `142` generated threaded handlers in total.

Their dominant control-flow shape is highly regular:

- `129` handlers use the common pattern:
  - helper call or inline body,
  - `if (handler_rc == 1) JUMP_P2();`
  - `DISPATCH();`
- `13` handlers are `DISPATCH()`-only in the generated source

The `DISPATCH()`-only set is:

- `Goto`
- `Jump`
- `If`
- `Gosub`
- `Return`
- `InitCoroutine`
- `Yield`
- `EndCoroutine`
- `MustBeInt`
- `SetDiag`
- `Halt`
- `Init`
- `Program`

This is an important result for the stitching design:

- there are not many handlers whose terminal structure is arbitrary or
  fundamentally unlike the common case,
- most handlers still have a normal straight-line path ending in `DISPATCH()`,
- the irregularity is usually one conditional `P2` transfer rather than a
  totally different dispatch model.

So the common stitched policy can be:

- preserve the normal path as straight fallthrough,
- patch one explicit branch edge for the `P2` path when needed,
- keep row/error/coroutine/terminal exits explicit.

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

This matches the measured handler-shape distribution above and is the main
reason the generated threaded dispatcher is a viable CnP fragment source
rather than only a conceptual reference.

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

## 13. Revised plan after `hot_expr` profiling

The `hot_expr / prepared_execute` RelWithDebInfo run showed that CnP is still
slower than the generated dispatcher:

- generated dispatcher median: about `0.968 us/op`
- CnP median: about `1.058 us/op`
- CnP/generate ratio: about `1.09`

The compiled statement has 13 VDBE opcodes:

```text
Init
Add
ResultRow
Halt
Integer
Integer
Add
Integer
Add
Integer
Add
Integer
Goto
```

The stitched CnP code size for this statement is `1870` bytes:

```text
common prologue + initial dispatcher: 28 bytes
opcode fragments:                    1008 bytes
3 return stubs:                       66 bytes
64 absolute call/jump thunks:         768 bytes
```

This explains the lack of speedup. The current threaded-fragment path did
remove per-opcode function prologues and epilogues, but it did not yet produce
cheap inline opcode bodies. A hot arithmetic fragment currently has this
shape:

```text
call cnp_frag_get_p
call cnp_frag_get_pOp
call cnp_frag_get_aMem
call vdbe_op_add
call cnp_frag_dispatch_fallthrough
jmp next_fragment
```

That is not cheaper than the generated interpreter, which already keeps
`p`, `pOp`, and `aMem` as local state inside one dispatcher function and calls
the same `vdbe_op_add()` helper directly.

This redesign now explicitly follows the runtime code-shape described in
*Copy-and-Patch Compilation: A fast compilation algorithm for high-level
languages and bytecode* (Haoran Xu, Fredrik Kjolstad, OOPSLA 2021,
doi:10.1145/3485513). The important takeaway for this branch is that the
extraction artifact may be compiler-friendly and relocation-heavy, but the
generated runtime code should be a statement-specific stitched layout of copied
basic blocks with concrete patched edges.

The revised plan is therefore more aggressive: remove helper calls from the
threaded fragment ABI, remove dispatch helper calls from the hot path, avoid
avoidable thunks, and optimize the scalar row-return path.

### 13.1 Inline fragment live-ins, not only opcode helpers

The current fragment-production source uses macros like:

```c
#define p cnp_frag_get_p()
#define pOp cnp_frag_get_pOp()
#define aMem cnp_frag_get_aMem()
```

This is the wrong ABI for hot stitched code. It forces every opcode body to
reload state through helper calls.

The next ABI should make the required live-ins explicit and stable:

- `p`
- `aOp`
- `pOp`
- `aMem`
- branch/exit targets when needed

There are two practical implementation choices.

First choice: fixed registers inside the stitched function. The common
prologue loads `p`, `aOp`, `pOp`, and `aMem` into fixed callee-saved registers
and every fragment is compiled to use those registers. This is the best
runtime shape, but it requires more control over compiler output.

Second choice: direct memory references to fixed globals or a per-execution
state block. This is easier to produce from C, but still removes the function
calls. It may be a good intermediate step:

```text
mov p,   [cnp_state.p]
mov pOp, [cnp_state.pOp]
mov aMem,[cnp_state.aMem]
```

That is still worse than fixed registers, but much cheaper than helper calls.

The important rule is:

- fragment code must not call `cnp_frag_get_p()`,
- fragment code must not call `cnp_frag_get_pOp()`,
- fragment code must not call `cnp_frag_get_aMem()`,
- fragment code must not call dispatch helpers on the hot fallthrough path.

### 13.2 Inline tiny opcode bodies fully

Inlining only `vdbe_op_add()` into a wrapper stencil was not enough. For
`Integer` and arithmetic opcodes the useful target is the actual small body,
including the low-level `Mem` operation when it is simple enough.

For example, `OP_Integer` should become a direct store to the output register
in the common case, not:

```text
call vdbe_op_integer
  call vdbe_prepare_null_out
  call mem_set_int
```

Likewise, `OP_Add` should inline the fast integer path directly, with a slow
exit for uncommon type/error cases.

Recommended first split:

- `Integer`: inline the common register store and flags update
- `Add`: inline integer + integer fast path
- `Subtract`: inline integer + integer fast path
- `Multiply`: inline integer + integer fast path where overflow policy allows
- `ResultRow`: keep the row materialization helper initially, but optimize the
  surrounding row/terminal control flow

The initial goal is not to inline every SQL type path. The goal is to make
the common benchmarked scalar path real native code and leave slow cases as
explicit helper exits.

### 13.3 Make fallthrough free

The generated dispatcher pays roughly one threaded dispatch per opcode. The
current CnP path is worse because `DISPATCH()` expands to:

```text
call cnp_frag_dispatch_fallthrough
jmp returned_target
```

For stitched code, fallthrough should normally cost nothing:

```text
fragment A body
fragment B body
fragment C body
```

The compiler knows the VDBE program at CnP compile time. For opcode `i`,
normal fallthrough is opcode `i + 1`. If `i + 1` is the physical next fragment
in the code buffer, there is no reason to emit a dispatch operation.

The stitching rule should be:

- for normal fallthrough, copy the next fragment immediately after the current
  one and remove the terminal dispatch edge,
- for unconditional `Goto`, emit a direct patched jump to `P2`,
- for conditional branches, emit a direct conditional jump to `P2` and let the
  non-taken path fall through,
- for error, row, done, and coroutine exits, jump to explicit local exit
  stubs.

This makes the hot arithmetic path cheaper than the generated dispatcher:

```text
generated dispatcher:
  handler body
  indirect dispatch to next opcode

desired CnP:
  handler body
  fall through to next handler body
```

### 13.4 Make branches direct, not table-dispatched

The current fragment code computes the next target through
`cnp_frag_dispatch_table`. That is unnecessary for ordinary VDBE branches
because `P2` is known while compiling the statement.

For intra-statement control flow, patch direct native branches:

```asm
jmp  rel32 target_fragment
jne  rel32 target_fragment
je   rel32 target_fragment
```

These targets are always inside the same generated code buffer, so x86-64
`rel32` is safe for these edges. There is no need for an absolute thunk for
intra-JIT branches.

Keep the dispatch table only for cases that genuinely need dynamic target
resolution, such as coroutine-style control flow if it cannot be lowered to
direct edges yet.

### 13.5 Avoid the second CnP entry for scalar row statements

Today a simple scalar `SELECT` enters CnP twice:

```text
call 1: execute Init..ResultRow, return SQL_ROW
call 2: resume at Halt, return SQL_DONE
```

That means the common prologue/epilogue is paid twice per produced row, and
the second call does almost no useful work.

For scalar or one-row statements where `ResultRow` is followed by a simple
terminal tail, compile a row-terminal fast path:

```text
ResultRow
Halt or deferred-halt marker
return SQL_ROW
next SQL step: return SQL_DONE without entering native code
```

There are two possible correctness-preserving variants.

Variant A: execute `Halt` before returning `SQL_ROW`.

This is fastest, but only valid if the row output remains readable after
`sqlVdbeHalt()` for the SQL API contract. This must be verified before use.

Variant B: defer `Halt`.

The CnP code returns `SQL_ROW` and stores a sentinel such as
`CNP_RESUME_DONE`. On the next `vdbe_cnp_exec()` call, the C wrapper observes
the sentinel and runs the minimal done/halt path without entering the stitched
native function. This preserves row lifetime semantics while avoiding the
second native prologue.

Variant B is the safer first implementation.

### 13.6 Stop generating thunks for intra-JIT targets

The `hot_expr` statement produced 64 absolute thunks, consuming `768` bytes.
Most of these exist because the fragment extractor conservatively turns
PC-relative relocations into:

```asm
movabs rax, target
jmp    rax
```

That is acceptable as a correctness fallback, but it is not an optimized JIT
strategy.

Use this relocation policy instead:

- intra-JIT control-flow edges: always patch direct `rel32` branches,
- local exit stubs: patch direct `rel32` branches,
- external helper calls: use direct `call rel32` if the target is in range,
- external helper calls outside `rel32`: use a shared literal pool or a rare
  absolute call sequence,
- hot opcodes: avoid external helper calls entirely by inlining the fast path.

PIC code is not required for generated statement code. The JIT owns the final
code buffer address and can patch exact addresses after allocation.

### 13.7 Keep PIC only where it helps extraction

The object file used for fragment extraction may still contain relocatable
code. That is fine. The final copied code does not need to preserve PIC-style
access when the JIT can resolve the target more cheaply.

The rule should be:

- use compiler relocations as extraction metadata,
- lower those relocations to the cheapest final encoding during stitching.

That means PIC-like object code is an input format detail, not the runtime
code-shape goal.

### 13.8 Revised first pilot

The next pilot should target only `hot_expr / prepared_execute` first. It has
enough repeated arithmetic to expose whether the revised model works.

Current implementation plan (April 2026):

1. **Split each extracted fragment tail into "state update" and "transfer".**

   The fragment generator should now expose four offsets per opcode:

   - `begin`
   - `dispatch`
   - `transfer`
   - `end`

   `dispatch` is the point after the opcode body where the fragment updates its
   local state for the next edge, for example:

   - `pOp += 1`
   - `pOp = &aOp[pOp->p2]`

   `transfer` is the actual terminal control transfer:

   - physical fallthrough placeholder,
   - direct branch to `P2`,
   - local jump to row/done/error exit,
   - or another explicit non-fallthrough edge.

   This split is necessary because a paper-style copy-and-patch stitcher does
   not want to copy "handler body + state update + unconditional jump" as one
   indivisible block. It wants the option to keep the state update but drop the
   terminal jump when the next copied block is already adjacent in the final
   code layout.

2. **Carry explicit fragment metadata into `vdbe_cnp_fragments.h`.**

   The extracted metadata should include:

   - `dispatch_offset`
   - `transfer_offset`
   - `tail_fallthrough`

   `tail_fallthrough` means:

   - the fragment may legally end by falling through into the next copied block,
     provided the stitcher has already applied the correct state update;
   - the final terminal jump is only a layout convenience in the extraction
     artifact, not a semantic requirement of the generated runtime code.

   This lets the runtime stitcher compute the copy size as:

   - `transfer_offset` for straight-line fallthrough edges;
   - full fragment size for terminal or non-adjacent control-flow edges.

3. **Treat the compiled object as extraction metadata, not as the desired
   runtime code shape.**

   The paper's model is:

   - the offline artifact may contain labels, relocations, helper-oriented
     jumps, and other convenient compiler-generated structure;
   - the runtime-generated code should be the cheapest specialized layout for
     the actual statement being compiled.

   In Tarantool terms, the object built by `vdbe_cnp_genfrags.py` and extracted
   by `vdbe_cnp_extract_fragments.py` is only the source of:

   - byte ranges,
   - relocation records,
   - branch/exit labels,
   - and per-fragment cut points.

   The generated code emitted by `vdbe_cnp_compile_fragments()` is the real
   optimization target.

4. **Patch intra-statement edges directly in the stitched code buffer.**

   The first paper-aligned stitcher should lower internal fragment targets to
   direct local edges:

   - normal fallthrough: no branch at all when opcode `i + 1` is placed
     immediately after opcode `i`;
   - `Goto` / `Init`: direct patched target to `P2`;
   - `IfNot` / `Once`: direct target to `P2` on the taken path, physical
     fallthrough on the non-taken path when possible;
   - row/done/error: direct branches to dedicated local exit stubs in the same
     copied code buffer.

   There should be no dispatch-table lookup for any of those ordinary
   intra-statement edges.

5. **Keep absolute thunks only for true external helper calls.**

   The fragment path still needs to resolve helper calls such as:

   - `vdbe_op_resultrow`
   - `vdbe_op_ifnot_inline`
   - `vdbe_op_once_inline`
   - other non-inlined helper bodies

   Those are genuine external targets relative to the stitched buffer and may
   still require:

   - direct `call rel32` when encodable;
   - otherwise the current absolute thunk fallback.

   However, internal labels such as:

   - fallthrough transfer,
   - `P2` transfer,
   - row target,
   - done target,
   - error target

   must no longer consume thunk slots. They are properties of the final layout,
   not external symbol references.

6. **Preserve the current entry ABI for the first transition.**

   The current fragment mode already uses a shared prologue and init region to
   load:

   - `p`
   - `aOp`
   - `pOp`
   - `aMem`
   - dispatch/exit support state

   The short-term plan is to keep that entry path stable while changing the
   copied fragment layout under it. This reduces the moving pieces in the first
   migration step:

   - entry still jumps to the first copied opcode block;
   - resume still re-enters through the shared entry path;
   - only the internal stitched control-flow graph changes.

   Once that works and is measured, the entry ABI can be optimized further.

7. **Keep row-resume semantics conservative until direct edges are stable.**

   The immediate goal is not yet to eliminate the second native entry for
   `SQL_ROW` statements. The safer sequence is:

   - first: make the stitched code follow the paper's copied-basic-block model;
   - then: re-check whether `ResultRow -> Halt` tails can use a row-done
     sentinel or another one-row fast path without violating SQL API row
     lifetime semantics.

   In practice this means:

   - preserve current `SQL_ROW` / `SQL_DONE` behavior first;
   - update resume bookkeeping only as needed for the new direct-edge layout;
   - postpone the scalar row fast path until correctness is revalidated.

8. **Use `hot_expr / prepared_execute` as the primary proof point.**

   The first success condition is not broad feature coverage. It is proving that
   the paper-style stitched layout materially improves the hot scalar arithmetic
   case that motivated the rewrite.

Detailed pilot scope:

- direct live-in access without `cnp_frag_get_*()` calls,
- free fallthrough for `Integer` and `Add`,
- direct jump for `Goto`,
- direct local exits for row/done/error,
- no intra-JIT absolute thunks,
- conservative row resume semantics at first,
- row-done sentinel only after the direct-edge model is correct and measured.

Success criteria:

- generated fragment metadata exposes separate `dispatch` and `transfer` cut
  points,
- the stitcher copies only the prefix before `transfer` for adjacent
  fallthrough edges,
- internal row/done/error/P2 edges are patched directly inside the stitched
  code buffer,
- thunk count drops because intra-JIT edges no longer use absolute
  trampolines,
- generated code size for `hot_expr` drops substantially below `1870` bytes,
- CnP beats the generated dispatcher on `hot_expr / prepared_execute`.

## 14. Track A: measurement before optimization

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

## 15. Track B: first-level helper inlining

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

## 16. Track C: reduce outer CnP dispatch overhead

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

## 17. Track D: ABI and register residency

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

## 18. Concrete phased plan

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

## 19. Risks

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

## 20. Practical conclusion

The `hot_expr` profile changes the optimization priority.

First-level helper inlining was a useful experiment, but it is not sufficient.
The current fragment path still performs too many helper calls around each
opcode and too much dispatch work between opcodes. It also emits unnecessary
absolute thunks for targets that are known while stitching the statement.

The next meaningful optimization should therefore be a total hot-path rewrite
of the threaded-fragment pilot:

1. make fragment live-ins direct,
2. inline the common `Integer` and arithmetic opcode bodies,
3. make straight-line fallthrough physical fallthrough,
4. patch intra-statement branches as direct native branches,
5. avoid intra-JIT absolute thunks,
6. avoid the second native entry for scalar `ResultRow; Halt` statements.

The first validation target should be only:

- `hot_expr / prepared_execute`

Success is not "a smaller regression". Success is:

- no `cnp_frag_get_*()` calls in the hot arithmetic fragments,
- no dispatch-helper call on straight-line fallthrough,
- no intra-JIT thunks for local statement targets,
- generated `hot_expr` code much smaller than `1870` bytes,
- CnP faster than the generated dispatcher on `hot_expr`.

Only after that should the plan widen again to:

- `tiny_const`,
- `point_lookup`,
- `bitwise_mix`,
- cursor-heavy statements.

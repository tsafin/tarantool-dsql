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

## 9. Why not threaded code today?

A direct-threaded model would have each opcode body end with a jump to the
next opcode body, instead of returning to a central C loop.

That is attractive because it removes:

- `call`,
- `ret`,
- and most of the central loop decode cost.

However, it was not used first for several practical reasons:

1. The current stencils are extracted from normal function code.
   A direct-threaded model is much easier when the code generator is designed
   around threaded control flow from the beginning.

2. Many opcode bodies still call ordinary C helpers.
   That means the first implementation still needed a safe, uniform ABI and a
   simple place to handle terminal statuses and resume behavior.

3. CnP needed debugger and unwind support early.
   A plain function-per-stencil model made it much easier to get usable GDB
   frames, `.eh_frame`, and perf naming working.

4. Coroutine and row-resume semantics are easier to get right in a central
   loop first.
   The current tagged-return model already handles:
   - next-address,
   - PC-jump,
   - `SQL_ROW`,
   - `SQL_DONE`,
   - and error returns.

So the current schema is not a claim that threaded code is wrong. It is a
staging choice.

## 10. Why not tail calls today?

Tail calls are the most plausible way to evolve the current stencil model into
direct chaining without rewriting the entire code generator around a custom
assembler.

In principle, the desired model is:

- stencil executes opcode body,
- stencil tail-jumps to the next stencil for the hot path,
- only terminal, fallback, and exceptional paths return to C.

That would preserve most of the current stencil extraction model while
removing the common-case `call/ret` chain.

The reasons it was not done first are:

1. Tail-call formation is compiler-sensitive.
   Reliable tail calls require careful control over:
   - ABI,
   - function signatures,
   - stack adjustments,
   - and helper calls inside the stencil.

2. The current stencils were extracted as ordinary functions with ordinary
   helper-call behavior.
   Guaranteeing a final tail jump is a stricter codegen contract than simply
   extracting a normal function body.

3. Row-return and coroutine semantics still need structured exits.
   Even in a tail-call model, some paths must still return to C:
   - `ResultRow`
   - terminal completion
   - errors
   - unsupported/fallback paths

4. It was more important to get a correct and observable implementation first
   than to optimize the final control-flow shape immediately.

## 11. Recommendation on control-flow evolution

The project should move toward direct chaining, but in stages.

Recommended order:

1. keep the current call/return schema while measuring and inlining the first
   helper layer,
2. once hot helper bodies are inlined, change the common-case stencil exit to
   direct jump or musttail-style chaining,
3. keep returns to C only for:
   - `SQL_ROW`,
   - `SQL_DONE`,
   - error,
   - fallback,
   - and special coroutine transitions if needed.

This should be treated as the main structural optimization after helper
inlining. It is more important than custom ABI work.

Preferred future model:

- ordinary call boundary only at entry and structured exits,
- direct chained jumps between hot stencils,
- optional superinstructions on top of that,
- custom internal register ABI only if still needed after chaining exists.

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

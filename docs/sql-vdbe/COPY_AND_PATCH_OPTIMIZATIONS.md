# Copy-and-Patch: Optimization Plan

## 1. Problem statement

The stitched-fragment pilot is no longer a compile-time experiment; it is the
current `CNP_MODE_FRAGMENTS` execution path. The remaining problem is now
runtime cost and correctness:

- the fragment path still pays too much per-op overhead on tiny programs,
- the fragment ABI is fragile enough to crash on `tiny_const/prepared_execute`,
- and non-linear fragment exits still prevent some hot paths from becoming one
  contiguous native trace.

The important distinction is that the implementation has moved away from the
older wrapper-stencil design:

- the hot path no longer executes one ordinary C function per opcode,
- the fragment object is built from a dedicated `preserve_none` entry function
  per fragment,
- runtime code stitches copied opcode fragments in VDBE order,
- straight-line fallthrough transitions now use patched local control flow
  rather than a return-to-C loop or a per-op dispatch-table jump,
- explicit exits remain only for row / done / error and other non-linear
  transitions.

So the main optimization question is no longer "how do we compile faster?" and
no longer even primarily "how do we remove the outer dispatch loop?" The
remaining question is:

- how do we reduce the residual per-fragment overhead still present inside the
  stitched code,
- while keeping the fragment ABI explicit and extraction robust?

## 2. Hypothesis: inline the first helper layer into stencils

One concrete idea is to force-inline the first helper layer called by the
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

- [`vdbe_op_integer()`](src/box/sql/vdbe_ops_data.c#L12)
- [`vdbe_op_bool()`](src/box/sql/vdbe_ops_data.c#L24)
- [`vdbe_op_int64()`](src/box/sql/vdbe_ops_data.c#L37)
- [`vdbe_op_eq()`](src/box/sql/vdbe_ops_compare.c#L31)

These functions are small enough that an inliner could plausibly fold:

- register address calculation from `p1/p2/p3`,
- flag checks from `p5`,
- simple `Mem` stores,
- small branch sequences for `jump / fallthrough / error`.

In the best case, the stencil becomes "real opcode code" instead of "opcode
wrapper plus helper call".

## 4. Current fusion status

The first fragment-fusion step is now working in the runtime stitcher.

For `CNP_FRAG_FALLTHROUGH` fragments, the extractor records two cut points:

- `dispatch_offset`: where the terminal dispatch-table lookup starts;
- `transfer_offset`: where the stack-restore epilogue before `jmp *%rax`
  starts.

When stitching adjacent fallthrough fragments, the runtime copies:

- the fragment body up to `dispatch_offset`;
- then the epilogue suffix from `transfer_offset`;
- and drops the dispatch-table lookup plus the final indirect jump.

This means the hot path now goes directly from one copied fragment into the
next one in native memory.

Important nuance: the extractor no longer requires `jmp *%rax` to be the final
bytes of the whole fragment body. Many fragments have cold assert / error paths
after the hot dispatch tail. The current extractor finds the hot fallthrough
tail by locating the `cnp_frag_dispatch_table` relocation, scanning forward to
the first `jmp *%rax` on that hot path, and compacting only that region.

At the moment this covers the fallthrough family well, including:

- arithmetic fragments like `Integer`, `Bool`, `Int64`, `Add`;
- data / cursor helpers like `Null`, `Variable`, `Copy`, `Column`;
- heavier fallthrough fragments like `ApplyType`, `AggStep`, `AggFinal`.

What still remains explicit:

- `CNP_FRAG_ROW`,
- `CNP_FRAG_TERMINAL`,
- `CNP_FRAG_JUMP_P2` and other non-linear exits.

## 5. Why first-level inlining may still help

Inlining the first helper level is not automatically sufficient.

There are three important limits:

1. Many handlers still call deep helpers.
   Examples include `mem_cmp()`, cursor navigation, sorter logic, tuple/index
   helpers, transaction helpers, and string conversion helpers.
   Inlining only the top-level handler may still leave most of the work behind
   an external call boundary.

2. The current stencil execution model still pays one function boundary per
   opcode in [`vdbe_cnp_exec()`](src/box/sql/vdbe_cnp.c#L2010).
   Even perfect helper inlining does not remove the outer
   `indirect call -> return -> decode -> branch` cycle.

3. Aggressive inlining can harm I-cache behavior.
   If every stencil becomes large, overall instruction cache pressure may get
   worse, especially for `sql-tap` and mixed realistic programs.

So this optimization should be treated as one stage in a larger plan, not the
only answer.

## 6. Recommendation on ABI changes

The stitched-fragment path now does use an explicit internal ABI, but only
inside the copied-fragment execution domain.

The current working design is:

1. `vdbe_cnp_exec()` still calls the fragment entry with the normal SysV C ABI.
2. A tiny explicit bridge must map that SysV call into the fragment live-ins.
   The main Tarantool binary is built with a compiler that ignores
   `preserve_none` in `src/box/sql/vdbe_cnp.c`, so a direct typed C call into
   fragment code is not reliable.
3. That wrapper moves live-ins into fixed internal registers and jumps into the
   fragment entry.
4. The stitched fragment body then runs under a stable fragment contract until
   it exits back to ordinary C.

The current refinement is to treat `musttail` as an emission detail, not the
final runtime shape. When stitching adjacent fallthrough fragments, truncate the
terminal dispatch transfer bytes so the hot trace becomes one contiguous native
block.

Current live-ins:

- `r12 = p`
- `r13 = aOp`
- `r14 = pOp`
- `r15 = aMem`

This is the right level for ABI control:

- normal helpers should remain normal C functions unless proven safe to inline,
- fragment-called out-of-line helpers must stay on the normal ABI unless they
  are compiled into the fragment object itself,
- the runtime call site should remain ordinary C,
- only the stitched fragment domain should use the custom internal contract.

So the practical ABI rule is now:

- do **not** migrate every helper to `preserve_none`,
- do stabilize the entry bridge and the fragment-production source around the
  fixed live-ins above,
- and only extend that internal ABI where it materially simplifies stitched
  native execution.

## 7. What the stitched code looks like

The most useful way to reason about the current design is to look at it as
three distinct machine-code phases:

1. SysV C entry into the bridge;
2. stitched fragment execution with fixed live-ins;
3. explicit exit back to ordinary C for row / done / error or other non-linear
   control flow.

### 7.1 Entry bridge: SysV call -> fragment live-ins

`vdbe_cnp_exec()` is ordinary C code, so the first call into native fragment
code uses the normal SysV argument order:

- `rdi = target fragment entry`
- `rsi = p`
- `rdx = aOp`
- `rcx = pOp`
- `r8  = aMem`

The bridge in [`cnp_fragment_enter()`](src/box/sql/vdbe_cnp.c#L1005) saves the
surrounding C frame's callee-saved registers, then remaps the live-ins into the
fragment contract:

```asm
push %rbx
push %rbp
push %r12
push %r13
push %r14
push %r15
sub  $8, %rsp
mov  %rsi, %r12    ; p
mov  %rdx, %r13    ; aOp
mov  %rcx, %r14    ; pOp
mov  %r8,  %r15    ; aMem
call *%rdi         ; enter copied fragment
```

This is why `preserve_none` still matters for the fragment object even though
the runtime call site itself remains ordinary C: the bridge establishes the
fixed-register world exactly once at fragment entry.

### 7.2 Inside a stitched fragment

Once execution is in copied fragment code, hot state stays resident in fixed
registers:

- `r12 = p`
- `r13 = aOp`
- `r14 = pOp`
- `r15 = aMem`

For a simple stitched `SELECT 1 + 2` trace, the copied `OP_Add` body looks like
this in live disassembly:

```asm
movslq 0x4(%r14), %rax
imul   $0x68, %rax, %rsi
add    %r15, %rsi
movslq 0x8(%r14), %rax
imul   $0x68, %rax, %rdi
add    %r15, %rdi

## 8. 2026-05-02 status: retain only low-risk prepare-path cleanups

The recent work on prepare-path cost produced two different classes of change:

1. **Keep**
   - reuse cached `Vdbe.stmt_id` instead of recalculating the SQL hash from
     `zSql` during prepare export and prepared-statement cache operations;
   - simplify Lua metadata export by reading `stmt->metadata[i]` directly and
     evaluating `sql_full_metadata` once per metadata table build.

2. **Drop**
   - moving prepared-statement methods (`execute`, `unprepare`) to a shared Lua
     metatable instead of storing them as table fields.

The reason for keeping only the first class is pragmatic: the `stmt_id` reuse
showed a clear prepare-path win, while the metatable/object-shape experiment
made matrix results noisier and did not produce a stable benefit.

Important scope clarification:

- `prepare_only` microbenchmarks do **not** exercise `vdbe_cnp_compile()`;
- `cnp_compile_count` stays zero there;
- any `prepare_only` movement is therefore parser / planner / Lua prepare export
  work, not stitched native-code generation.

So the current branch direction is:

- keep execute-path work focused on fragment hot paths;
- keep prepare-path work limited to low-risk export/bookkeeping cleanups unless
  a larger parser/planner change is explicitly justified by measurements.
movslq 0xc(%r14), %rax
imul   $0x68, %rax, %rdx
add    %r15, %rdx
movabs $mem_add, %rax
callq  *%rax
test   %eax, %eax
je     .Lfallthrough
mov    $-1, %rax
pop    %rbp
retq
```

The important part is not the exact offsets but the operand sources:

- register fields like `P1/P2/P3` are read from `pOp` through `r14`;
- `Mem` cell addresses are formed from `aMem` in `r15`;
- helper calls still receive normal call arguments in SysV registers, but the
  fragment's own long-lived state remains in `r12-r15`;
- no dispatch loop state needs to be reloaded from memory between adjacent
  fused fallthrough fragments.

### 7.3 Fused fallthrough between adjacent fragments

Before fusion, a fallthrough fragment ended with a dispatch-table lookup and an
indirect jump:

```asm
add    $0x18, %r14
movabs $cnp_frag_dispatch_table, %rax
mov    (%rax), %rax
mov    %r14, %rcx
sub    %r13, %rcx
imul   %rcx, %rdx
mov    (%rax, %rdx, 1), %rax
pop    %rbp
jmpq   *%rax
```

For adjacent `CNP_FRAG_FALLTHROUGH` fragments, the stitcher now removes that
dispatch-lookup block and the final `jmpq *%rax`. The hot trace becomes:

```asm
add    $0x18, %r14
pop    %rbp
push   %rbp
mov    %rsp, %rbp
... next copied fragment starts here ...
```

So the fragment still advances `pOp` in `r14`, but instead of bouncing through
`cnp_frag_dispatch_table`, execution continues directly into the next copied
fragment body in native memory.

### 7.4 Exit from a handler / fragment

There are now two distinct exit styles.

For fused hot fallthrough, the "exit" is just the straight-line handoff shown
above: advance `pOp`, restore any local frame bytes needed by the current
fragment, and continue into the next fragment.

For non-fallthrough or terminal conditions, the copied fragment returns an
explicit status to the bridge / exec loop. Typical shapes are:

```asm
mov    $-1, %rax
pop    %rbp
retq               ; error
```

or entry into dedicated terminal helpers:

```text
cnp_frag_terminal_row()   -> SQL_ROW
cnp_frag_terminal_done()  -> SQL_DONE
cnp_frag_terminal_error() -> -1
```

That split is important:

- hot fallthrough stays fully inside the stitched native trace;
- row / done / error still cross back into ordinary C in a deliberate,
  observable way;
- `JUMP_P2` and other non-linear control flow still use explicit transfers until
  they get their own safe fusion treatment.

## 8. Current problem

The immediate `tiny_const/prepared_execute` blocker was twofold:

1. missing SysV -> fragment-live-in entry bridging;
2. incorrect resume-at-row logic that restarted from `p->cnp_code` instead of
   the actual resumed fragment PC.

Those two bugs are fixed. The next correctness risk is any fragment-called
out-of-line helper that still advertises `preserve_none` without being built
into the fragment object with the same ABI, and any future fusion of non-linear
fragment exits that drops required control-transfer bytes.

## 9. High-level optimization strategy

The optimization plan should proceed in five tracks:

1. Measurement
2. First-level helper inlining
3. Dispatch model reduction
4. Register-residency / ABI work if still needed
5. Final-trace fusion by truncating terminal transfer jumps during stitching

Track 5 is now partially complete for `CNP_FRAG_FALLTHROUGH`.

The tracks are ordered by confidence and engineering cost.

## 10. Benchmark-driven priority order

The initial workload selection should follow the measured opcode mixes from the
existing JIT benchmark harness, not intuition.

### 10.1 First target: `tiny_const / prepared_execute`

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
- fragment ABI overhead,
- and helper call overhead for tiny handlers.

Recommended first helper targets:

- [`vdbe_op_integer()`](src/box/sql/vdbe_ops_data.c#L12)
- [`vdbe_op_bool()`](src/box/sql/vdbe_ops_data.c#L24)
- [`vdbe_op_int64()`](src/box/sql/vdbe_ops_data.c#L37)
- [`vdbe_op_real()`](src/box/sql/vdbe_ops_data.c#L52)
- [`vdbe_op_add()`](src/box/sql/vdbe_ops_arith.c)

### 10.2 Second target: `hot_expr / prepared_execute`

This is the second best target. It is still arithmetic-dominated but has enough
repeated arithmetic to show whether helper inlining scales beyond the minimal
case.

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

### 10.3 Third target: `point_lookup / prepared_execute`

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
- iterator open / close costs,
- or the fragment boundary itself.

The current discard-results measurements refine that plan further.

For `point_lookup`, the fragment path is already using the per-PC typed column
binding introduced later in this document: the three `OP_Column` sites bind to
`vdbe_op_column_integer_fast()` because `bench_arith.a/b/c` are all
`INTEGER`. In focused `perf_jit.sh` runs with `BENCH_DISCARD_RESULTS=1`,
`point_lookup/prepared_execute` is already slightly faster on CnP than on the
generated interpreter, so the remaining ordinary benchmark gap is not a
stencil-vs-fragment issue and not primarily a `vdbe_cnp_exec()` problem.

That changes the next `point_lookup`-specific optimization item:

1. first, specialize the arithmetic helpers selected by neighboring bytecode
   (`Add`, `Subtract`, `Multiply`, `Divide`, `Remainder`) when the producer
   registers are known to come from exact integer `OP_Column` sites;
2. only after that, consider a narrower same-cursor multi-column fast path for
   repeated `Column -> Column -> arithmetic` patterns if
   `vdbe_op_column_typed_fast()` / `vdbe_field_ref_fetch_*()` still dominate.

### 10.4 Current scan comparison: `agg_scan` and `builtin_scan`

After fixing the recent scan-fragment correctness bugs, the safe current CnP
baseline is already competitive with the other execution modes on the two main
scan-heavy benchmark workloads.

Focused run (`BENCH_RUNS=3`, lower is better):

| workload | mode | prepared_execute | automatic_execute |
|---|---:|---:|---:|
| agg_scan | **cnp** | **27.051 us** | 27.701 us |
| agg_scan | generated | 33.184 us | 34.750 us |
| agg_scan | mcjit | 27.723 us | **26.933 us** |
| builtin_scan | **cnp** | **85.384 us** | **88.542 us** |
| builtin_scan | generated | 92.996 us | 93.367 us |
| builtin_scan | mcjit | 93.009 us | 96.041 us |

Interpretation:

- `agg_scan`: CnP is now clearly ahead of the generated interpreter and roughly
  tied with LLVM MCJIT.
  - vs generated: about **18.5% faster** in `prepared_execute`,
    **20.3% faster** in `automatic_execute`;
  - vs MCJIT: about **2.4% faster** in `prepared_execute`,
    **2.9% slower** in `automatic_execute`.
- `builtin_scan`: CnP is currently the fastest of the three modes.
  - vs generated: about **8.2% faster** in `prepared_execute`,
    **5.2% faster** in `automatic_execute`;
  - vs MCJIT: about **8.2% faster** in `prepared_execute`,
    **7.8% faster** in `automatic_execute`.

This changes the optimization priority for scan-heavy paths:

- the fragment path no longer needs emergency correctness triage for these two
  workloads;
- `JUMP_P2` fusion is still worth pursuing, but now as a targeted throughput
  optimization rather than a prerequisite for basic competitiveness;
- generic fallthrough fusion should only return once it preserves internal cold
  branches and late register restores in complex fragments such as `ApplyType`.

2026-05-05 follow-up:

- the scan-loop `JUMP_P2` transfer rewrite now matches the extracted fragment
  tail shape (`pop r64; jmp *%rax`) and updates the fragment `pOp` live-in
  before jumping to the stitched target;
- focused reruns moved `agg_scan/prepared_execute` to `22.161 us`
  (generated `30.129 us`, LLVM MCJIT `27.689 us`) and
  `builtin_scan/prepared_execute` to `79.892 us`
  (generated `90.601 us`, LLVM MCJIT `85.464 us`);
- after that pass, `perf_jit.sh` on `agg_scan/prepared_execute` points first at
  `vdbe_op_column_typed_fast()` and the `vdbe_field_ref_*()` helpers, with
  `AggStep` / `AggFinal` no longer the first item to inline.

### 10.4.1 What the scan-loop `JUMP_P2` rewrite actually means

This subsection is intentionally written for an external reader who is learning
how the stitched CnP fragments work.

#### The high-level idea

Some VDBE opcodes have a conditional control-flow shape:

- if the condition is true, jump to bytecode address `P2`;
- otherwise continue with the next opcode.

In the interpreter this is expressed with the familiar `JUMP_P2()` macro. In
the fragment pipeline, those opcodes are tagged as `CNP_FRAG_JUMP_P2`.

Before the recent scan-loop rewrite, even a *known* scan-loop branch still ended
with a generic "find the next fragment in the dispatch table" sequence. The
branch target itself was known at compile time, but the native code still did a
table lookup at runtime.

The rewrite changes only that last transfer step:

1. keep the handler logic exactly as before;
2. keep row / done / error exits explicit as before;
3. replace the generic dispatch-table lookup on the hot taken/fallthrough paths
   with a direct jump to the already-known stitched fragment;
4. explicitly restore the fragment live-in `r14 = pOp` before that direct jump.

So the rewrite is **not** "new SQL semantics". It is only a cheaper native
handoff between already-correct fragment bodies.

#### The two important live-ins

For the samples below, the important registers are:

- `r13 = aOp` — pointer to the first VDBE opcode;
- `r14 = pOp` — pointer to the current VDBE opcode.

When a fragment jumps to another fragment, the target fragment expects `r14` to
already point at the correct `VdbeOp`. That is why the rewrite is not just
"jump to a label": it also has to preserve the `pOp` live-in contract.

#### Old shape: explicit dispatch-table lookup

Here is the current disassembly shape of an **unrewritten** `OP_Init` transfer
from `EXPLAIN (disassemble=yes)`. `OP_Init` is useful as a teaching example
because it still shows the old generic `JUMP_P2` pattern clearly:

```asm
movsxd   rax, dword ptr [r14 + 8]      ; load P2
lea      rcx, [rax + 2*rax]
lea      r14, [8*rcx]
add      r14, r13                      ; r14 = &aOp[P2]
movabs   rcx, <cnp_frag_dispatch_table>
mov      rcx, qword ptr [rcx]
mov      rax, qword ptr [rcx + 8*rax]  ; load fragment entry for opcode P2
jmp      rax
```

What this means:

1. Load `P2` from the current opcode.
2. Convert that opcode index into a `VdbeOp *` and place it in `r14`.
3. Use the dispatch table to map bytecode PC -> fragment entry address.
4. Jump indirectly through that table entry.

It works, but the last two memory-dependent steps are unnecessary when the
compiler already knows the exact stitched target fragment.

#### Rewritten taken branch: direct `P2` handoff

Now compare that with the **rewritten** taken branch from a scan-loop
`OP_SeekGE` fragment in the same kind of `EXPLAIN (disassemble=yes)` output:

```asm
call     <vdbe_op_seekge>
cmp      eax, 1
jne      L01ad

movsxd   rax, dword ptr [r14 + 8]      ; P2
lea      rcx, [rax + 2*rax]
lea      r14, [8*rcx]
add      r14, r13                      ; logical target is &aOp[P2]

movabs   r14, <known VdbeOp* for P2>   ; restate pOp live-in explicitly
pop      rcx
jmp      L1454                         ; jump directly to stitched target
```

What changed:

- there is **no dispatch-table load**;
- there is **no indirect `jmp *table[index]`**;
- the branch jumps straight to the already-stitched native block for the `P2`
  target;
- immediately before that jump, the patched block writes the target `pOp`
  value into `r14`, so the next fragment sees the exact live-in it expects.

The `movabs r14, <known VdbeOp*>` may look redundant because the original code
has just computed `&aOp[P2]`. That redundancy is acceptable here: the goal of
this patch is a small, explicit, obviously-correct transfer block. The constant
write makes the target fragment contract visible in one place.

#### Rewritten fallthrough side: direct "next opcode" handoff

The same `OP_SeekGE` fragment also has a fallthrough path when the branch is not
taken. Before rewriting, the fallthrough side used the same generic lookup shape
as any other fragment:

```asm
add      r14, 24
movabs   rax, <cnp_frag_dispatch_table>
mov      rax, qword ptr [rax]
mov      rcx, r14
sub      rcx, r13
movabs   rdx, -6148914691236517205
imul     rdx, rcx                      ; strength-reduced divide by sizeof(Op)=24
mov      rax, qword ptr [rax + rdx]
pop      rcx
jmp      rax
```

After rewriting, the same fallthrough becomes:

```asm
test     eax, eax
js       L01dd
add      r14, 24
movabs   r14, <known VdbeOp* for pc + 1>
pop      rcx
jmp      L01e6
```

So the native code now says, in effect:

- "the next bytecode opcode is known to be `pc + 1`";
- "the next stitched native block is known to be `L01e6`";
- "set `r14` to the matching `VdbeOp *` and jump there directly".

That is the whole rewrite in one sentence: **replace "look up where to go" with
"we already know where to go"**.

#### Why this is safe for scan loops but not yet for everything

This pass was intentionally restricted to scan-loop `CNP_FRAG_JUMP_P2` opcodes:

- `Rewind`
- `SeekGE`, `SeekLE`, `SeekLT`, `SeekGT`
- `IdxLE`, `IdxGT`, `IdxGE`, `IdxLT`
- `Next`

Those fragments have a simple enough transfer shape that the patcher can safely
rewrite the hot taken/fallthrough exits without disturbing cold error branches
or hidden register-restore work.

That restriction matters because earlier generic fallthrough rewriting attempts
ran into exactly those problems in more complex fragments such as `ApplyType`.

#### What to look for in future disassembly

When reading a CnP `EXPLAIN (disassemble=yes)` listing, a scan-loop
`JUMP_P2` rewrite is visible when you see this pattern near a branch exit:

```asm
movabs   r14, <some constant>
pop      rcx
jmp      L....
```

and do **not** see the old table-based tail:

```asm
movabs   rax/rcx, <cnp_frag_dispatch_table>
...
mov      rax, qword ptr [table + index]
jmp      rax
```

One final note for students: the absolute addresses and local labels in these
samples change from run to run. The stable part is the **shape**:

- old shape = compute target PC -> consult dispatch table -> indirect jump;
- new shape = set `r14 = target pOp` -> direct jump to the stitched fragment.

### 10.5 Typed scan helper binding in CnP fragments

The next step after the safe scan baseline was **not** another fallthrough-tail
fusion attempt. The better payoff came from leaving fragment control flow alone
and specializing the hot helper relocs inside the existing stitched fragments.

Two per-PC bindings were added:

1. `OP_Column` now binds to a typed helper when the fragment compiler can
   recover a stable bytecode mapping
   `OpenSpace(space_id) -> IteratorOpen(cursor, ..., space_reg) -> Column`.
   When that mapping resolves to an exact schema field type, the fragment uses a
   typed helper instead of the generic `vdbe_op_column()`.
2. Single-register `OP_ApplyType` binds its `mem_cast_implicit` relocation to a
   typed fast helper when `p4.types[0]` is exact.

This keeps the fragment ABI and stitched fragment layout unchanged, which avoids
reopening the earlier correctness problems in `OP_OpenSpace` / `OP_ApplyType`
fallthrough tails.

Focused run after the typed binding change (`BENCH_RUNS=3`, lower is better):

| workload | mode | prepared_execute | automatic_execute |
|---|---:|---:|---:|
| agg_scan | **cnp typed** | **23.052 us** | **23.475 us** |
| agg_scan | generated | 26.175 us | 28.682 us |
| agg_scan | mcjit | 26.903 us | 29.366 us |
| builtin_scan | **cnp typed** | **85.821 us** | **87.630 us** |
| builtin_scan | generated | 92.545 us | 97.108 us |
| builtin_scan | mcjit | 95.726 us | 94.465 us |

Against the previous safe CnP scan baseline from section 10.4:

- `agg_scan`: `27.051 / 27.701 us` -> `23.052 / 23.475 us`
  (**14.8% faster** prepared, **15.3% faster** automatic).
- `builtin_scan`: `85.384 / 88.542 us` -> `85.821 / 87.630 us`
  (**0.5% slower** prepared, **1.0% faster** automatic).

The perf follow-up (`perf_jit.sh`, `prepared_execute`, 1% symbol cutoff)
supports the intended explanation:

- `agg_scan`
  - before: top symbols included `vdbe_field_ref_fetch_data`,
    `mem_from_mp_ephemeral`, `vdbe_op_column`, `mem_cast_implicit`;
  - after: `vdbe_op_column_typed_fast` and `vdbe_field_ref_fetch_data` remain,
    but `mem_from_mp_ephemeral` and `mem_cast_implicit` drop below the 1%
    cutoff.
- `builtin_scan`
  - before: top symbols included `vdbe_op_column`,
    `vdbe_field_ref_fetch_data`, `mem_from_mp_ephemeral`,
    `vdbe_op_builtinfunction`, and `mem_cast_implicit`;
  - after: `vdbe_op_column_typed_fast` and `vdbe_field_ref_fetch_data` remain,
    while the generic materialize/cast helpers are no longer in the top report.

So the current picture is:

- **typed helper selection works** and gives a clear win on `agg_scan`;
- `builtin_scan` also benefits, but less dramatically because builtin/string
  work remains a larger share of the total time;
- the next likely scan optimization is deeper specialization of field
  extraction / tuple slot traversal (`vdbe_field_ref_fetch_data()`), not a
  return to unsafe generic tail fusion.

### 10.6 Field-ref traversal fast path for scan columns

The next scan pass targeted the remaining `vdbe_field_ref_fetch_data()`
overhead directly instead of changing fragment control flow again.

Two small runtime changes were enough:

1. `vdbe_field_ref_prepare_tuple()` now keeps the tuple pointer in the
   `vdbe_field_ref`, so tuple-backed rows can still use tuple metadata when it
   exists.
2. `vdbe_field_ref` now tracks `rightmost_slot`, letting
   `vdbe_field_ref_fetch_data()` skip the bitmask search when a query walks
   forward through columns of the same row.

That matches the hot scan opcode order well:

- `agg_scan` repeatedly fetches columns `1 -> 3 -> 2`;
- `builtin_scan` repeatedly fetches `1 -> 2 -> 3 -> 4 -> 1`.

The first visit to a row still decodes forward with `mp_next()`, but the common
monotonic case avoids the extra "find nearest initialized slot" work before the
walk.

Focused rerun after the field-ref change (`BENCH_RUNS=3`, median per-op, lower
is better):

| workload | mode | prepared_execute | automatic_execute |
|---|---:|---:|---:|
| agg_scan | **cnp field-ref** | **22.203 us** | **20.924 us** |
| agg_scan | generated | 25.375 us | 26.546 us |
| agg_scan | mcjit | 26.606 us | 25.919 us |
| builtin_scan | **cnp field-ref** | **83.924 us** | **80.629 us** |
| builtin_scan | generated | 97.804 us | 107.819 us |
| builtin_scan | mcjit | 95.792 us | 102.451 us |

Against the previous typed-helper CnP baseline from section 10.5:

- `agg_scan`: `23.052 / 23.475 us` -> `22.203 / 20.924 us`
  (**3.7% faster** prepared, **10.9% faster** automatic).
- `builtin_scan`: `85.821 / 87.630 us` -> `83.924 / 80.629 us`
  (**2.2% faster** prepared, **8.0% faster** automatic).

The perf follow-up on `agg_scan/prepared_execute` also moved in the expected
direction:

- before this pass, `vdbe_field_ref_fetch_data()` was still around **5.25%**;
- after the field-ref fast path, it drops to **2.91%**;
- `vdbe_op_column_typed_fast` remains visible, so the hot path is now more
  clearly concentrated in typed column decode plus the stitched fragment body.

So the current scan picture is:

- typed helper binding removed the generic materialize/cast overhead;
- this field-ref pass trimmed the remaining tuple-slot traversal cost;
- the next scan work, if any, should be even narrower (for example more
   specialized typed `OP_Column` helpers), not a return to risky generic tail
   fusion.

### 10.6.1 Exact typed `OP_Column` helpers for resolved scan cursors

After the scan-loop `JUMP_P2` rewrite, the hottest helper in focused discard-mode
profiles was still `vdbe_op_column_typed_fast()`. At that point the branch
already knew the `OpenSpace -> IteratorOpen -> Column` mapping for the hot scan
statements, so the remaining generic type recheck inside that helper had become
mostly redundant work.

The follow-up pass keeps the existing typed binding logic, but makes the CnP
binding one step narrower:

1. when `cnp_select_column_handler()` resolves a stable Tarantool cursor and an
   exact schema type, it now binds an `*_exact_fast` helper instead of the older
   generic typed helper;
2. that exact helper still preserves the same observable behavior, but it skips
   the per-call schema-type equality branch and inlines the local
   `vdbe_field_ref_fetch_data()` walk into the same translation unit.

This is intentionally a small execute-path cleanup rather than a new fragment
control-flow experiment. The fragment ABI, relocations, and stitched transfer
logic stay unchanged.

Focused discard-mode reruns after the exact-helper change (`BENCH_RUNS=5`,
prepared execute only, lower is better):

| workload | generated | LLVM MCJIT | CnP exact column |
|---|---:|---:|---:|
| `agg_scan/prepared_execute` | `25.181 us` | `24.543 us` | **`21.184 us`** |
| `builtin_scan/prepared_execute` | `96.185 us` | `85.645 us` | **`77.579 us`** |

Against the previous post-`JUMP_P2` CnP baseline:

- `agg_scan`: `22.161 us` -> `21.184 us` (**4.4% faster**).
- `builtin_scan`: `79.892 us` -> `77.579 us` (**2.9% faster**).

The perf follow-up on `agg_scan/prepared_execute` also shifts in the expected
direction:

- the old `vdbe_op_column_typed_fast()` hotspot becomes
  `vdbe_op_column_typed_exact_fast()` instead;
- standalone `vdbe_field_ref_fetch_data()` no longer appears as a top symbol,
  which is exactly what we wanted from this first-level helper inlining pass;
- the remaining visible field-ref cost is now mostly
  `vdbe_field_ref_prepare_tuple()` plus a smaller `vdbe_field_ref_fetch_field()`
  / `tuple_field()` share.

So the next scan step is narrower again: not more control-flow work, and not a
return to generic `OP_Column`, but reducing the remaining tuple-backed metadata
and field-descriptor overhead around the exact typed helper path.

### 10.7 Discard-results benchmark mode

The next benchmark change did not touch the execution engines themselves. It
added a way to measure the SQL path without also timing row collection into a
`port_sql` and Lua table materialization on every iteration.

Two new C-side helpers execute SQL and discard `SQL_ROW` output immediately:

- `sql_prepare_and_execute_no_result()`
- `sql_execute_prepared_no_result()`

They are exposed only as an internal Lua hook:

- `box.internal.execute_no_result(sql[, params])`
- `box.internal.execute_no_result(stmt_id[, params])`

The benchmark harness now switches its timed loop to that path when
`BENCH_DISCARD_RESULTS=1`, while setup and warmup still use the normal checked
path so statement preparation and basic correctness stay covered.

Full isolated matrix in discard mode (`BENCH_RUNS=3`, median per-op, lower is
better):

| workload | generated prepared | mcjit prepared | cnp prepared | generated automatic | mcjit automatic | cnp automatic |
|---|---:|---:|---:|---:|---:|---:|
| tiny_const | 0.144 us | 0.143 us | **0.136 us** | 0.179 us | 0.140 us | **0.131 us** |
| hot_expr | 0.190 us | 0.208 us | **0.174 us** | 0.188 us | 0.211 us | **0.163 us** |
| point_lookup | 0.757 us | 0.739 us | **0.677 us** | 0.703 us | 0.692 us | **0.679 us** |
| bitwise_mix | **0.881 us** | 0.913 us | 0.901 us | **0.870 us** | 0.898 us | 0.886 us |
| agg_scan | 25.492 us | 22.935 us | **19.504 us** | 23.733 us | 23.392 us | **19.356 us** |
| builtin_scan | 85.964 us | 85.096 us | **81.510 us** | 91.992 us | 88.705 us | **79.014 us** |

Compared to the ordinary result-materializing matrix, the small expression
workloads drop by roughly **67-87%** in all three modes. That confirms the
earlier perf profiles: those tests were dominated mostly by Lua/result
machinery rather than the SQL execution core.

The larger scan workloads move much less:

- `agg_scan` improves mainly for LLVM MCJIT and CnP;
- `builtin_scan` only shifts by a few percent;
- `point_lookup` changes the most qualitatively, because once result
  materialization is removed, CnP becomes the fastest mode there too.

So the current engine-only picture is:

- CnP wins **5 of 6** workloads in both `prepared_execute` and
  `automatic_execute`;
- generated still has a small edge on `bitwise_mix`, so that workload remains a
  useful non-scan benchmark for arithmetic/bitwise fragment overhead;
- the main remaining CnP questions are now narrower throughput issues, not a
  broad inability to compete once front-end overhead is removed.

### 10.8 `point_lookup` discard-mode perf follow-up

The discard-mode matrix already showed that `point_lookup` flips in CnP's favor
once Lua result materialization is removed. The next focused perf pass confirms
what that means for the optimization plan.

Focused run (`perf_jit.sh`, `point_lookup/prepared_execute`,
`BENCH_DISCARD_RESULTS=1`, `BENCH_RUNS=20`):

| mode | mean per-op |
|---|---:|
| generated | 0.696 us |
| cnp | **0.674 us** |

Key observations:

- CnP stays fully in fragment mode here (`cnp_exec_count=200000`,
  `interpreter_step_count=0`, `cnp_fallback_count=0`);
- generated still shows `vdbe_exec_generated_dispatcher` in the top symbols,
  while `vdbe_cnp_exec()` is a small share of the CnP profile;
- the hot symbols in both modes are dominated by storage / field extraction
  work (`tree_iterator_start`, tuple key compare, `vdbe_op_column*`,
  `vdbe_field_ref_fetch_*`), not by the outer fragment/runtime glue.

So the next JIT-specific work for `point_lookup` should be narrow:

1. specialize integer arithmetic helpers chosen from neighboring bytecode, in
   the same spirit as the existing typed `Column` / `ApplyType` bindings;
2. if that is not enough, fuse repeated same-cursor integer `Column` fetches
   into a dedicated CnP-only helper before revisiting any broader control-flow
   ideas.

### 10.9 `point_lookup`: specialization anatomy

The `point_lookup` query used in the benchmark harness is:

```sql
SELECT a + b, a - b, a * c, a / b, a % b
FROM bench_arith
WHERE id = ?;
```

Its prepared bytecode is stable and small enough that the specialization logic
can reason about neighboring producers instead of treating each opcode in
isolation:

```text
 5 SeekGE      1 17 2
 6 IdxGT       1 17 2
 7 Column      1  1 8
 8 Column      1  2 9
 9 Add         9  8 3
10 Subtract    9  8 4
11 Column      1  3 10
12 Multiply   10  8 5
13 Divide      9  8 6
14 Remainder   9  8 7
15 ResultRow   3  5 0
16 Next        1  6 0
```

`bench_arith` is declared as:

```sql
CREATE TABLE bench_arith(
    id INTEGER PRIMARY KEY,
    a  INTEGER,
    b  INTEGER,
    c  INTEGER
);
```

So for this statement the hot fragment pattern is:

```text
SeekGE / IdxGT
  -> Column(cursor=1, field=1)   ; a
  -> Column(cursor=1, field=2)   ; b
  -> Add / Subtract
  -> Column(cursor=1, field=3)   ; c
  -> Multiply / Divide / Remainder
  -> ResultRow
```

#### 10.9.1 What gets specialized

The runtime now applies two layers of compile-time binding to that bytecode.

1. **Typed column binding**

   `cnp_select_column_handler()` recovers the mapping

   ```text
   OpenSpace(space_id) -> IteratorOpen(cursor, ..., space_reg) -> Column(cursor, field)
   ```

   and uses the schema type of `bench_arith.a/b/c` to bind all three `OP_Column`
   sites to `vdbe_op_column_integer_fast()`.

2. **Neighbor-driven arithmetic binding**

   `cnp_find_last_reg_writer()`, `cnp_find_unique_reg_writer()`, and
   `cnp_reg_is_likely_int()` propagate the "this register is an exact integer"
   fact from:

   - exact integer `OP_Column` producers,
   - integer constants,
   - and prior integer arithmetic.

   That lets `cnp_select_arith_handler()` / `cnp_select_arith_fragment_handler()`
   retarget the arithmetic opcodes to:

   - `vdbe_op_add_int_fast()`
   - `vdbe_op_sub_int_fast()`
   - `vdbe_op_multiply_int_fast()`
   - `vdbe_op_divide_int_fast()`
   - `vdbe_op_remainder_int_fast()`

For the concrete `point_lookup` bytecode above, the patched hot chain is:

| PC | Opcode | Registers | Patched helper |
|---:|---|---|---|
| 7 | `Column` | `cursor=1 field=1 -> r8` | `vdbe_op_column_integer_fast` |
| 8 | `Column` | `cursor=1 field=2 -> r9` | `vdbe_op_column_integer_fast` |
| 9 | `Add` | `r9, r8 -> r3` | `vdbe_op_add_int_fast` |
| 10 | `Subtract` | `r9, r8 -> r4` | `vdbe_op_sub_int_fast` |
| 11 | `Column` | `cursor=1 field=3 -> r10` | `vdbe_op_column_integer_fast` |
| 12 | `Multiply` | `r10, r8 -> r5` | `vdbe_op_multiply_int_fast` |
| 13 | `Divide` | `r9, r8 -> r6` | `vdbe_op_divide_int_fast` |
| 14 | `Remainder` | `r9, r8 -> r7` | `vdbe_op_remainder_int_fast` |

#### 10.9.2 Where the patch points are

For fragment mode, the generated `OP_Add` fragment is compiled with a normal-ABI
bridge symbol so the runtime can retarget it safely at link time:

```asm
cnp_frag_sym_OP_Add:
  push   %rbp
  mov    %rsp,%rbp
  movabs $0x0,%rax        # reloc @ +6  -> vdbe_op_add_sysv_bridge
  mov    %r12,%rdi        # p
  mov    %r14,%rsi        # pOp
  mov    %r15,%rdx        # aMem
  call   *%rax
  test   %eax,%eax
  je     .Lok
  mov    $-1,%rax
  ret
.Lok:
  add    $0x18,%r14       # pOp += 1
  movabs $0x0,%rax        # reloc @ +44 -> cnp_frag_dispatch_table
  ...
  jmp    *%rax
```

The generated fragment metadata for `OP_Add` records:

```text
reloc[0] offset=6   symbol=vdbe_op_add_sysv_bridge
reloc[1] offset=44  symbol=cnp_frag_dispatch_table
dispatch_offset = 42
transfer_offset = 79
```

That means the runtime has two independent patch sites to work with:

```text
bytes  0..41  : helper call + error check
bytes 42..78  : dispatch-table lookup block
bytes 79..end : terminal transfer bytes
```

Visualized on the `point_lookup` chain:

```text
pc 7  Column(a)   : call target patched -> vdbe_op_column_integer_fast
pc 8  Column(b)   : call target patched -> vdbe_op_column_integer_fast
pc 9  Add         : call target patched -> vdbe_op_add_int_fast
pc 10 Subtract    : call target patched -> vdbe_op_sub_int_fast
pc 11 Column(c)   : call target patched -> vdbe_op_column_integer_fast
pc 12 Multiply    : call target patched -> vdbe_op_multiply_int_fast
pc 13 Divide      : call target patched -> vdbe_op_divide_int_fast
pc 14 Remainder   : call target patched -> vdbe_op_remainder_int_fast
```

So the optimization is not "invent a new super-instruction for the whole
query." It is: keep the stitched fragment layout, but replace a run of generic
helper calls with narrower per-PC targets that match what the neighboring
bytecode proves.

#### 10.9.3 What the specialized helper body looks like

`vdbe_op_add_int_fast()` is still behavior-safe: it is only fast on the exact
integer path and explicitly falls back to `mem_add()` if runtime values do not
match the predicted shape.

Representative LLVM disassembly excerpt:

```asm
vdbe_op_add_int_fast:
  movslq 0x4(%rsi), %rax      # p1
  ...                         # compute &aMem[p1]
  movslq 0x8(%rsi), %rax      # p2
  ...                         # compute &aMem[p2]
  movslq 0xc(%rsi), %rax      # p3
  ...                         # compute &aMem[p3]
  ...
  test   ...                  # NULL / metatype checks
  jne    .Lnull_or_fallback
  ...
  callq  sql_add_int          # exact integer arithmetic
  ...
  callq  mem_set_int          # write result MEM_TYPE_INT/UINT
  retq

.Lnull_or_fallback:
  ...
  callq  mem_add              # generic SQL semantics fallback
```

The same structure is used for `Subtract`, `Multiply`, `Divide`, and
`Remainder`:

- stay on the fast path for exact integer `Mem` cells,
- preserve `NULL`, signedness, overflow, and divide-by-zero semantics,
- and fall back to the generic helper when runtime values violate the static
  prediction.

This is why the optimization is safe to drive from neighboring bytecode:
compile-time analysis chooses a narrower target, but runtime checks still guard
correctness.

### 10.10 Full matrix after integer arithmetic specialization

The first item above is now implemented and reflected in the full
`run_benchmark_matrix.sh` rerun (`BENCH_RUNS=3`).

The most important `point_lookup` effect is that CnP no longer trails the
generated interpreter on the execute paths:

- `prepared_execute`: `2.631 us` (generated) -> `2.473 us` (CnP)
  while LLVM MCJIT is `2.423 us`;
- `automatic_execute`: `3.643 us` (generated) -> `2.365 us` (CnP),
  which is now the fastest of the three modes.

That changes the broader matrix reading too:

- CnP now wins **9 of 14** execute-path cells outright;
- against the generated interpreter specifically, CnP is faster in **12 of 14**
  execute-path cells;
- the remaining execute-path losses are now narrower and mixed:
  `tiny_const/prepared_execute`, `bitwise_mix` by a small margin to LLVM MCJIT,
  and `sort_window/automatic_execute` by a small margin to the interpreter.

So `point_lookup` is no longer the dominant blocker in the benchmark matrix.
If more work is needed on that workload specifically, the next likely target is
still the column side (`vdbe_op_column_typed_fast()` /
`vdbe_field_ref_fetch_*()`), not another broad fragment-control-flow change.

### 10.11 Constant-specialized arithmetic for `tiny_const` / `hot_expr`

The next narrow pass targeted the tiny constant-only expression workloads:

- `tiny_const`: `SELECT 1 + 2;`
- `hot_expr`: `SELECT 1 + 2 + 3 + 4 + 5;`

Their prepared bytecode is still small:

```text
tiny_const:
  Init
  Integer
  Integer
  Goto
  Add
  ResultRow
  Halt

hot_expr:
  Init
  Integer
  Integer
  Add
  Integer
  Add
  Integer
  Add
  Integer
  Goto
  Add
  ResultRow
  Halt
```

The first `point_lookup` arithmetic pass already recognized integer producers,
but it still read both input `Mem` cells for each arithmetic opcode. For tiny
constant traces that leaves an obvious next step: if neighboring bytecode proves
one or both inputs come from `OP_Integer` / `OP_Int64`, skip those input loads
and use compile-time immediates instead.

The implementation keeps the bytecode unchanged and stores per-PC immediate
metadata in the CnP program state. Then:

- `cnp_resolve_int_constant()` traces a register back to a constant producer;
- `cnp_select_arith_handler()` / `cnp_select_arith_fragment_handler()` prefer
  `vdbe_op_*_const_fast()` when at least one operand is constant and the other
  side is still integer-specializable;
- the new helpers (`vdbe_op_add_const_fast()`, `...sub...`, `...multiply...`,
  `...divide...`, `...remainder...`) read only the dynamic inputs and combine
  them with the per-PC immediates.

That is intentionally still **not constant folding**:

- the `Integer` opcodes remain in the trace;
- the VDBE program shape is unchanged;
- the win comes from narrower arithmetic helpers, not from deleting bytecode.

Focused reruns after the change (`BENCH_RUNS=12`) show the intended direction:

| workload | mode | generated | CnP |
|---|---|---:|---:|
| `tiny_const` | `prepared_execute`, discard | `0.143 us` | **0.140 us** |
| `hot_expr` | `prepared_execute`, discard | `0.198 us` | **0.186 us** |
| `hot_expr` | `prepared_execute`, materialized | `1.195 us` | **1.114 us** |

`perf_jit.sh` on `hot_expr/prepared_execute` also now shows
`vdbe_op_add_const_fast()` as a visible CnP hot-path symbol.

So this pass is a reasonable first backend-only step for constants:

- it improves tiny arithmetic traces without changing SQL code generation;
- it reuses the same neighboring-bytecode specialization model as the
  `point_lookup` arithmetic pass;
- and it gives a better base for the next, cleaner step.

The next step should still be **prepare-time constant folding** in SQL codegen.
That would let the compiler collapse sequences such as:

```text
Integer 1
Integer 2
Add
```

into a single constant result, which helps all execution backends instead of
teaching only CnP to run the unfused bytecode faster.

### 10.12 Prepare-time constant folding for integer arithmetic

That next step is now implemented for integer literal arithmetic trees in SQL
codegen.

The fold currently covers constant-only trees built from:

- `TK_INTEGER`
- unary `+` / unary `-`
- binary `+`, `-`, `*`, `/`, `%`

and uses the same `sql_add_int()` / `sql_sub_int()` / `sql_mul_int()` /
`sql_div_int()` / `sql_rem_int()` helpers as the runtime fast paths, so
signedness and overflow handling stay aligned with normal execution.

When folding succeeds, bytecode emission now writes a single literal instead of
emitting the original arithmetic chain. For example:

```text
EXPLAIN SELECT 1 + 2 + 3 + 4 + 5;

before:
  Integer
  Integer
  Add
  Integer
  Add
  Integer
  Add
  Integer
  Goto
  Add
  ResultRow
  Halt

after:
  Init
  Integer 15
  ResultRow
  Halt
```

Likewise:

```text
EXPLAIN SELECT 10 - 3, 6 * 7, 21 / 3, 22 % 5;
```

now emits only literal loads plus `ResultRow`.

The fold is intentionally conservative:

- `SELECT 1 / 0` is **not** folded and still emits `Divide`, so the runtime
  keeps control of divide-by-zero semantics;
- overflow / unsupported cases fall back to ordinary expression codegen.

Focused reruns after enabling prepare-time folding (`BENCH_RUNS=8`) show the
expected cross-backend improvement on the tiny constant workloads:

| workload | mode | generated | LLVM MCJIT | CnP |
|---|---|---:|---:|---:|
| `tiny_const` | `prepared_execute` | `1.026 us` | `1.026 us` | **0.924 us** |
| `tiny_const` | `automatic_execute` | `0.959 us` | `0.973 us` | **0.940 us** |
| `hot_expr` | `prepared_execute` | `0.966 us` | `1.009 us` | **0.918 us** |
| `hot_expr` | `automatic_execute` | `1.038 us` | **0.926 us** | `0.947 us` |

This means the original CnP-only constant-specialized arithmetic helpers are no
longer carrying the whole burden on these traces: the SQL frontend now removes
most of the redundant arithmetic work before any backend executes the statement.

The SQL validation pass after this change was clean too:

- `sql`: **112 pass**, 6 disabled
- `sql-tap`: **485 pass**, 45 disabled
- `sql-luatest`: **48 pass**, 2 disabled

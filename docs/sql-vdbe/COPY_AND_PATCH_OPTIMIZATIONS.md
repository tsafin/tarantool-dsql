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

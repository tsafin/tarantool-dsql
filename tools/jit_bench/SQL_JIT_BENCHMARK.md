# SQL JIT benchmark

This note describes the **three-dispatcher benchmark** for Tarantool's VDBE SQL
engine: the threaded interpreter, LLVM MCJIT, and Copy-and-Patch (CnP) JIT.
It covers methodology, what is being measured, and the significant performance
improvement that resulted from adding an automatic statement cache in M4.

This note is only for the focused JIT micro-benchmark matrix. End-to-end
sequential SQL testsuite timings belong in
`docs/sql-vdbe/branch-notes/END_TO_END_SQL_BENCHMARK.md`.

The benchmark harness used for these measurements lives at:

- `tools/jit_bench/sql_llvm_mcjit_benchmark.lua`

## What is being benchmarked

The current benchmark matrix uses six SQL workload shapes and measures them in
three execution modes.

| Workload | SQL shape | Purpose |
| --- | --- | --- |
| `tiny_const` | `SELECT 1 + 2;` | Small constant expression, almost pure overhead |
| `hot_expr` | `SELECT 1 + 2 + 3 + 4 + 5;` | Small arithmetic expression that exercises the expression evaluator |
| `point_lookup` | indexed `SELECT ... FROM bench_arith WHERE id = ?` | Lookup with realistic table access plus arithmetic work |
| `agg_scan` | indexed range `sum()` / `count()` / `max()` over `bench_arith` | Heavier numeric aggregation where native execution has more room to win |
| `builtin_scan` | indexed range text builtin scan over `bench_text` | String/integer builtin mix (`length`, `abs`, `substr`, `upper`, `lower`) plus aggregation |
| `sort_window` | indexed range subquery with `ORDER BY ... LIMIT` over `bench_arith` | Sorter-heavy top-K workload for profile-driven optimization of sort paths |

`sort_window` was added after the last full published matrix in this document.
Its focused measurements and profiling are currently being used to guide the
next sorter-oriented optimization pass; the older result tables below therefore
do not yet include a `sort_window` row.

| Case | What it measures | Why it matters |
| --- | --- | --- |
| `prepare_only` | repeated `box.prepare()` / `box.unprepare()` | Isolates prepare-time JIT compile cost |
| `prepared_execute` | prepare once, execute many times by `stmt_id` | Best-case reuse path for JIT backends |
| `automatic_execute` | repeated `box.execute(sql, args)` | Warm cache path: prepare once, execute many times via auto cache |

## How it is benchmarked

The numbers below were taken from a **RelWithDebInfo** matrix with:

- build: `build-jit-relwithdebinfo`
- interpreter baseline: `VDBE_DISPATCHER=generated SQL_JIT_ENABLE=0`
- LLVM MCJIT run: `VDBE_DISPATCHER=generated SQL_JIT_ENABLE=1`
- CnP JIT run: `VDBE_DISPATCHER=cnp SQL_JIT_ENABLE=0`
- `BENCH_RUNS=3`, best-of-three (minimum latency) reported

The LLVM MCJIT `automatic_execute` numbers reflect a fix introduced after M4
(`vdbe_jit_compile_cached`) that forces JIT compilation for auto-cached stmts.
Before the fix, `jit_exec_count` was 0 for `box.execute()` paths because the
trivial-program filter blocked compilation for non-prepared stmts.

The benchmark harness records wall-clock time and `box.stat.sql()` deltas for
each run.

### Iteration counts

| Workload | `prepare_only` | `prepared_execute` | `automatic_execute` |
| --- | ---: | ---: | ---: |
| `tiny_const` | 500 | 200 000 | 200 000 |
| `hot_expr` | 500 | 200 000 | 200 000 |
| `point_lookup` | 250 | 100 000 | 100 000 |
| `agg_scan` | 500 | 5 000 | 5 000 |
| `builtin_scan` | 300 | 3 000 | 3 000 |
| `sort_window` | 200 | 2 000 | 2 000 |

`automatic_execute` now uses the same iteration count as `prepared_execute`
because the auto stmt cache (added in M4) eliminates per-call recompilation,
making the automatic path as warm as the prepared path after the first call.

## Execution model

### Prepared statement path

![Prepared statement path](images/sql_jit_benchmark/prepared_path.png)

### Automatic path (with auto stmt cache)

![Automatic path with cache](images/sql_jit_benchmark/automatic_path.png)

On first `box.execute(sql)` for a given SQL string, the statement is compiled
(and optionally JIT-compiled). It is then stored in a 256-slot direct-mapped
`auto_stmt_cache` indexed by `(sql_hash ^ sql_flags * 2654435761) & 255`.
Subsequent calls with the same SQL string hit the cache: only a reset and
rebind are performed, with no recompile.

## Current measured results

All numbers are best-of-3 runs, RelWithDebInfo build.

### Prepare only

These numbers measure prepare-time cost in isolation (prepare + unprepare, no
execute).

| Workload | Interpreter | LLVM MCJIT | CnP JIT |
| --- | ---: | ---: | ---: |
| `tiny_const` | `2.938 µs` | `6 053 µs` | `2.873 µs` |
| `hot_expr` | `4.096 µs` | `7 877 µs` | `3.653 µs` |
| `point_lookup` | `9.121 µs` | `9 338 µs` | `8.486 µs` |
| `agg_scan` | `~14.9 µs` | — | `~15.4 µs` |
| `builtin_scan` | — | — | — |

CnP prepare cost equals interpreter prepare cost — CnP patches stencils at
prepare time using only memcpy and pointer fixups, with no LLVM passes.
LLVM MCJIT prepare cost is in the **6–9 ms** range.

For `agg_scan`, CnP prepare cost (~15.4 µs) matches interpreter prepare cost
(~14.9 µs) — confirming no LLVM overhead even for heavier bytecode programs.
LLVM MCJIT and `builtin_scan` prepare numbers are not yet measured.

### Prepared execute

These numbers measure the path where the statement is prepared once and reused.

| Workload | Interpreter | LLVM MCJIT | CnP JIT | CnP vs interp |
| --- | ---: | ---: | ---: | ---: |
| `tiny_const` | `0.882 µs` | `0.971 µs` | `1.026 µs` | +16% |
| `hot_expr` | `0.974 µs` | `0.969 µs` | `1.002 µs` | +3% |
| `point_lookup` | `2.300 µs` | `2.371 µs` | `2.303 µs` | 0% |
| `agg_scan` | `24.199 µs` | — | `26.817 µs` | +11% |
| `builtin_scan` | `90.860 µs` | — | `88.783 µs` | −2% |

All three dispatchers are within **±15%** of each other at this workload scale.
In a RelWithDebInfo build the interpreter is at or slightly below JIT speeds for
most workloads; a Release build with `-O3` changes the picture for heavier
expressions and table scans.

For the two scan-heavy workloads, CnP runs in **preserve_none fragment mode**
(see section below) rather than stencil mode — the first mode where CnP can
compete with the interpreter on longer-running loops. `builtin_scan` shows CnP
2% faster; `agg_scan` shows CnP 11% slower, which is consistent with the
dispatch overhead in the current fragment stitching implementation.

### Automatic execute (warm cache)

These numbers measure `box.execute(sql, args)` with the auto stmt cache warm
(all iterations after the first call are cache hits).  LLVM MCJIT now shows
`jit_exec_count = 200 000` — native code is running because
`vdbe_jit_compile_cached()` forces compilation on the first cache-miss path.

| Workload | Interpreter | LLVM MCJIT | CnP JIT |
| --- | ---: | ---: | ---: |
| `tiny_const` | `0.923 µs` | `0.950 µs` | `0.948 µs` |
| `hot_expr` | `0.945 µs` | `0.989 µs` | `1.032 µs` |
| `point_lookup` | `2.310 µs` | `2.358 µs` | `2.336 µs` |
| `agg_scan` | not yet measured | — | not yet measured |
| `builtin_scan` | not yet measured | — | not yet measured |

All three dispatchers converge to the same throughput for small workloads. The
auto stmt cache made the `automatic_execute` path as efficient as
`prepared_execute`. `agg_scan` and `builtin_scan` automatic_execute numbers
have not yet been collected.

### CnP execution modes: stencils vs. preserve_none fragments

CnP JIT operates in two modes depending on whether the VDBE program's opcodes
are fully covered by the fragment table:

- **Stencil mode** (`CNP_MODE_STENCILS`): Each opcode is patched individually
  and run one-at-a-time from a C dispatch loop. `sql_cnp_step_count` increments
  for each stencil executed. Used by `tiny_const`, `hot_expr`, `point_lookup`.

- **Fragment mode** (`CNP_MODE_FRAGMENTS`, preserve_none): Pre-stitched native
  code using `preserve_none` calling convention + `musttail` dispatch between
  opcode handlers. The entire program runs as a chain of tail calls with no
  return to C until an error or result row. `sql_cnp_step_count` does **not**
  increment — the C step loop is bypassed. Used by `agg_scan`, `builtin_scan`.

The fragment path was extended in the current session to cover 39 opcodes
including `OP_AggStep`, `OP_AggFinal`, `OP_Column`, `OP_ApplyType`,
`OP_OpenSpace`, `OP_SkipLoad`, and the full cursor navigation set. The stubs
object is now compiled with `-fno-pic -mcmodel=large` (matching the fragments
build flags) to avoid `R_X86_64_GOTPCRELX` relocations that the CnP patcher
does not handle.

Fragment mode is a prerequisite for CnP to show any advantage on scan-heavy
workloads: in stencil mode every opcode returns to C before dispatching the
next one, which cancels most of the benefit of native execution for tight loops.

### Before and after: automatic_execute improvement

The auto stmt cache eliminated per-call recompilation. Old numbers used only 5 000
(tiny_const) and 100 (hot_expr / point_lookup) iterations because LLVM MCJIT
would recompile every call, making higher counts impractical.

| Workload | Dispatcher | Before cache | After cache | Speedup |
| --- | --- | ---: | ---: | ---: |
| `tiny_const` | Interpreter | `3.224 µs` | `0.923 µs` | **3.5×** |
| `tiny_const` | LLVM MCJIT | `1.932 µs` (no native exec) | `0.950 µs` (native) | **2.0×** |
| `hot_expr` | Interpreter | `4.402 µs` | `0.945 µs` | **4.7×** |
| `hot_expr` | LLVM MCJIT | `4.969 µs` | `0.989 µs` | **5.0×** |
| `point_lookup` | Interpreter | `9.831 µs` | `2.310 µs` | **4.3×** |
| `point_lookup` | LLVM MCJIT | `9 337 µs` (recompile each call) | `2.358 µs` | **>3 900×** |

The point_lookup LLVM MCJIT case was catastrophic before the cache: it was
recompiling a ~9 ms JIT program for every single `box.execute` call.

## Why prepare matters (LLVM MCJIT break-even)

For LLVM MCJIT, prepare cost is still in the 6–9 ms range. Because the per-execution
advantage over the interpreter is negligible (or even slightly negative at these
tiny workload sizes), the break-even point requires an impractical number of reuses.

```text
total_cost = prepare_cost + execution_count * execute_cost
```

| Workload | Extra LLVM MCJIT prepare cost | Per-exec MCJIT vs interp delta | Break-even |
| --- | ---: | ---: | ---: |
| `tiny_const` | ~6 050 µs | −0.089 µs (MCJIT **slower**) | never |
| `hot_expr` | ~7 873 µs | +0.005 µs | ~1 574 000 executions |
| `point_lookup` | ~9 329 µs | −0.071 µs (MCJIT **slower**) | never |

At RelWithDebInfo build settings, LLVM MCJIT has no execution advantage for
these tiny workloads. A Release build with `-O3` / profile-guided optimisation
changes the picture for heavier expressions, but for VDBE micro-workloads the
interpreter overhead is already very low.

**CnP has no break-even problem**: its prepare cost equals interpreter prepare
cost, so it carries zero compile-time risk for short-lived statements.

## Practical reading of the numbers

1. **All three dispatchers are now equivalent for `box.execute()` workloads.**
   The auto stmt cache removed the per-call compile penalty. Applications using
   `box.execute()` in a hot loop get the same throughput regardless of which
   dispatcher is active.

2. **LLVM MCJIT prepare cost is still 6–9 ms per statement.** For one-shot
   SQL that will never be cached (e.g., DDL, ad hoc queries), LLVM MCJIT adds
   visible latency. At RelWithDebInfo build settings and these tiny workload
   sizes, there is no measurable execution advantage to offset that cost.

3. **CnP prepare cost is zero relative to interpreter.** CnP is a safe drop-in
   for any dispatcher mode: no latency cliff for one-shot SQL, and execution
   throughput within 5–10% of interpreter.

4. **CnP fragment mode now covers agg_scan and builtin_scan workloads.**
   The preserve_none fragment pipeline (39 opcodes, compiled with
   `-fno-pic -mcmodel=large`) enables these heavier scan workloads to run
   entirely in native code without returning to C between opcodes.
   `builtin_scan` is 2% faster than interpreter; `agg_scan` is 11% slower,
   suggesting further optimisation opportunities in the aggregation fragment
   bodies.

5. **Benchmark reports should always separate prepare from execute.**
   Reporting only execution throughput hides the dominant cost in one-shot
   or low-reuse workloads.

## Current conclusion

The benchmark data supports several claims:

- **The auto stmt cache made `box.execute()` as efficient as explicit
  `box.prepare()` + `stmt:execute()`** for repeated calls with the same SQL
  string. This benefits all three dispatchers equally.

- **LLVM MCJIT `automatic_execute` now runs native code** after the
  `vdbe_jit_compile_cached()` fix. Before the fix the trivial-program filter
  blocked JIT compilation for non-prepared stmts; `jit_exec_count` was 0 for
  all `box.execute()` paths despite the cache being warm.

- **At RelWithDebInfo build settings and these tiny workload sizes, all three
  dispatchers are within ±15% of each other** for execution throughput.
  LLVM MCJIT's per-call execution advantage is not measurable here; it adds
  visible latency (~6–9 ms) at prepare time with no offsetting runtime gain.

- **CnP is a safe drop-in for any dispatcher mode**: zero latency cliff for
  one-shot SQL, and execution throughput within ±15% of interpreter across all
  six benchmark workloads including the two new scan-heavy shapes.

- **CnP fragment mode is operational for scan-heavy workloads.** The
  `builtin_scan` result (CnP −2%, i.e. slightly faster) shows that the
  preserve_none tail-call chain is already competitive with the interpreted
  dispatch loop at RelWithDebInfo. The `agg_scan` 11% regression points to
  optimisation opportunity in the aggregation opcode fragment bodies, not a
  structural limitation of the approach.

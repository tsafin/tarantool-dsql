# SQL JIT benchmark

This note describes the **three-dispatcher benchmark** for Tarantool's VDBE SQL
engine: the threaded interpreter, LLVM MCJIT, and Copy-and-Patch (CnP) JIT.
It covers methodology, what is being measured, and the significant performance
improvement that resulted from adding an automatic statement cache in M4.

The benchmark harness used for these measurements lives at:

- `tools/jit_bench/sql_llvm_mcjit_benchmark.lua`

## What is being benchmarked

The current benchmark matrix uses three SQL workload shapes and measures them in
three execution modes.

| Workload | SQL shape | Purpose |
| --- | --- | --- |
| `tiny_const` | `SELECT 1 + 2;` | Small constant expression, almost pure overhead |
| `hot_expr` | `SELECT 1 + 2 + 3 + 4 + 5;` | Small arithmetic expression that exercises the expression evaluator |
| `point_lookup` | indexed `SELECT ... FROM bench_arith WHERE id = ?` | Lookup with realistic table access plus arithmetic work |

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
- `BENCH_RUNS=3`, best-of-three reported

The benchmark harness records wall-clock time and `box.stat.sql()` deltas for
each run.

### Iteration counts

| Workload | `prepare_only` | `prepared_execute` | `automatic_execute` |
| --- | ---: | ---: | ---: |
| `tiny_const` | 500 | 200 000 | 200 000 |
| `hot_expr` | 500 | 200 000 | 200 000 |
| `point_lookup` | 250 | 100 000 | 100 000 |

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
| `tiny_const` | `2.000 µs` | `5 978 µs` | `2.000 µs` |
| `hot_expr` | `4.000 µs` | `7 776 µs` | `4.000 µs` |
| `point_lookup` | `8.000 µs` | `9 224 µs` | `8.000 µs` |

CnP prepare cost equals interpreter prepare cost — CnP patches stencils at
prepare time using only memcpy and pointer fixups, with no LLVM passes.
LLVM MCJIT prepare cost is still in the **6–9 ms** range.

### Prepared execute

These numbers measure the path where the statement is prepared once and reused.

| Workload | Interpreter | LLVM MCJIT | CnP JIT |
| --- | ---: | ---: | ---: |
| `tiny_const` | `0.930 µs` | `0.895 µs` | `1.000 µs` |
| `hot_expr` | `0.980 µs` | `0.955 µs` | `0.970 µs` |
| `point_lookup` | `2.270 µs` | `2.250 µs` | `2.290 µs` |

All three dispatchers are within **±5%** of each other at this workload scale.
LLVM MCJIT has a marginal edge; CnP is marginally slower than interpreter for
`tiny_const` but converges for heavier workloads.

### Automatic execute (warm cache)

These numbers measure `box.execute(sql, args)` with the auto stmt cache warm
(all iterations after the first call are cache hits).

| Workload | Interpreter | LLVM MCJIT | CnP JIT |
| --- | ---: | ---: | ---: |
| `tiny_const` | `0.880 µs` | `0.880 µs` | `0.895 µs` |
| `hot_expr` | `0.980 µs` | `0.990 µs` | `1.040 µs` |
| `point_lookup` | `2.250 µs` | `2.230 µs` | `2.270 µs` |

All three dispatchers now converge to the same throughput. The auto stmt cache
made the `automatic_execute` path as efficient as `prepared_execute`.

### Before and after: automatic_execute improvement

The auto stmt cache eliminated per-call recompilation. Old numbers used only 5 000
(tiny_const) and 100 (hot_expr / point_lookup) iterations because LLVM MCJIT
would recompile every call, making higher counts impractical.

| Workload | Dispatcher | Before cache | After cache | Speedup |
| --- | --- | ---: | ---: | ---: |
| `tiny_const` | Interpreter | `3.224 µs` | `0.880 µs` | **3.7×** |
| `tiny_const` | LLVM MCJIT | `1.932 µs` (no native exec) | `0.880 µs` | **2.2×** |
| `hot_expr` | Interpreter | `4.402 µs` | `0.980 µs` | **4.5×** |
| `hot_expr` | LLVM MCJIT | `4.969 µs` | `0.990 µs` | **5.0×** |
| `point_lookup` | Interpreter | `9.831 µs` | `2.250 µs` | **4.4×** |
| `point_lookup` | LLVM MCJIT | `9 337 µs` (recompile each call) | `2.230 µs` | **>4 000×** |

The point_lookup LLVM MCJIT case was catastrophic before the cache: it was
recompiling a ~9 ms JIT program for every single `box.execute` call.

## Why prepare matters (LLVM MCJIT break-even)

For LLVM MCJIT, prepare cost is still in the 6–9 ms range. The break-even
analysis shows how many reuses are needed to amortize that cost against the
marginal execution advantage.

```text
total_cost = prepare_cost + execution_count * execute_cost
```

| Workload | Extra LLVM MCJIT prepare cost | Per-exec LLVM MCJIT gain vs interp | Break-even |
| --- | ---: | ---: | ---: |
| `tiny_const` | ~5 976 µs | ~0.035 µs | ~170 000 executions |
| `hot_expr` | ~7 772 µs | ~0.025 µs | ~310 000 executions |
| `point_lookup` | ~9 216 µs | ~0.020 µs | ~460 000 executions |

At this scale of per-execution advantage (~0.02–0.04 µs), the break-even
is now in the hundreds of thousands of executions. LLVM MCJIT has a real
native-code advantage for long-lived prepared statements, but the edge over
interpreter at these workloads is small.

**CnP has no such break-even problem**: its prepare cost equals interpreter
prepare cost, so it carries zero compile-time risk for short-lived statements.

## Practical reading of the numbers

1. **All three dispatchers are now equivalent for `box.execute()` workloads.**
   The auto stmt cache removed the per-call compile penalty. Applications using
   `box.execute()` in a hot loop get the same throughput regardless of which
   dispatcher is active.

2. **LLVM MCJIT prepare cost is still 6–9 ms per statement.** For one-shot
   SQL that will never be cached (e.g., DDL, ad hoc queries), LLVM MCJIT adds
   visible latency. For cached or explicitly prepared statements executed
   hundreds of thousands of times, the marginal execution advantage may matter.

3. **CnP prepare cost is zero relative to interpreter.** CnP is a safe drop-in
   for any dispatcher mode: no latency cliff for one-shot SQL, and execution
   throughput within 5–10% of interpreter.

4. **Benchmark reports should always separate prepare from execute.**
   Reporting only execution throughput hides the dominant cost in one-shot
   or low-reuse workloads.

## Current conclusion

The benchmark data supports two claims:

- **The auto stmt cache made `box.execute()` as efficient as
  explicit `box.prepare()` + `stmt:execute()`** for repeated calls with the
  same SQL string. This benefits all three dispatchers equally.

- **For long-lived explicitly prepared statements, LLVM MCJIT is modestly
  faster than interpreter** (~0–5% at these workload sizes). The advantage
  is real but small; CnP and interpreter are competitive.


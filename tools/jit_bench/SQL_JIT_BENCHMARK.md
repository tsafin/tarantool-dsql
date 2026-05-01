# SQL JIT benchmark

This note describes the focused **three-dispatcher SQL micro-benchmark** for
Tarantool's VDBE engine:

- the generated threaded interpreter,
- LLVM MCJIT,
- Copy-and-Patch (CnP).

It documents the workload set, the current `sort_window` profiling target, and
the latest full matrix results from `build-jit-relwithdebinfo`.

This note is only for the focused JIT micro-benchmark matrix. End-to-end
sequential SQL testsuite timings belong in
`docs/sql-vdbe/branch-notes/END_TO_END_SQL_BENCHMARK.md`.

The benchmark harness used for these measurements lives at:

- `tools/jit_bench/sql_llvm_mcjit_benchmark.lua`

## What is being benchmarked

The current default matrix uses seven workload shapes and measures them in
three execution modes.

| Workload | SQL shape | Purpose |
| --- | --- | --- |
| `tiny_const` | `SELECT 1 + 2;` | Small constant expression, almost pure overhead |
| `hot_expr` | `SELECT 1 + 2 + 3 + 4 + 5;` | Small arithmetic expression that exercises the expression evaluator |
| `point_lookup` | indexed `SELECT ... FROM bench_arith WHERE id = ?` | Lookup with realistic table access plus arithmetic work |
| `bitwise_mix` | mixed integer bitwise expression over bound values | Integer-heavy ALU / comparison path with little storage work |
| `agg_scan` | indexed range `sum()` / `count()` / `max()` over `bench_arith` | Heavier numeric aggregation where native execution has more room to win |
| `builtin_scan` | indexed range text builtin scan over `bench_text` | String/integer builtin mix (`length`, `abs`, `substr`, `upper`, `lower`) plus aggregation |
| `sort_window` | indexed range subquery with `ORDER BY ... LIMIT` over `bench_arith` | Sorter-heavy top-K workload for profile-driven optimization of sort paths |

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
- `BENCH_RUNS=3`
- reported value: `mean_per_op_us` from the harness JSON output

The benchmark harness records wall-clock time and `box.stat.sql()` deltas for
each run.

### Iteration counts

| Workload | `prepare_only` | `prepared_execute` | `automatic_execute` |
| --- | ---: | ---: | ---: |
| `tiny_const` | 500 | 200 000 | 200 000 |
| `hot_expr` | 500 | 200 000 | 200 000 |
| `point_lookup` | 250 | 100 000 | 100 000 |
| `bitwise_mix` | 2 500 | 100 000 | 20 000 |
| `agg_scan` | 500 | 5 000 | 5 000 |
| `builtin_scan` | 300 | 3 000 | 3 000 |
| `sort_window` | 200 | 2 000 | 2 000 |

For most workloads, `automatic_execute` uses the same iteration count as
`prepared_execute` because the auto stmt cache eliminates per-call
recompilation, making the automatic path as warm as the prepared path after the
first call. `bitwise_mix` keeps a shorter automatic loop because it is already
long enough to be stable at the default count.

## Execution model

### Prepared statement path

![Prepared statement path](images/sql_jit_benchmark/prepared_path.png)

### Automatic path (with auto stmt cache)

![Automatic path with cache](images/sql_jit_benchmark/automatic_path.png)

On first `box.execute(sql)` for a given SQL string, the statement is compiled
(and optionally JIT-compiled). It is then stored in a 256-slot direct-mapped
`auto_stmt_cache`. Subsequent calls with the same SQL string hit the cache:
only a reset and rebind are performed, with no recompile.

## `sort_window`: why this workload exists

`sort_window` is the dedicated sorter stress case used for the current
profile-driven CnP pass. The goal is to make sorter and coroutine costs visible
without letting Lua result materialization dominate the measurement.

The SQL shape is:

```sql
SELECT sum(score), sum(src_id)
FROM (
    SELECT ((a * 17 + b * 7 - c * 3) % 257) AS score,
           id AS src_id
    FROM bench_arith
    WHERE id BETWEEN ? AND ?
    ORDER BY score DESC, b ASC, id DESC
    LIMIT 32
);
```

This is intentionally structured in three layers:

1. an **indexed range scan** on `bench_arith` keeps the storage access pattern
   realistic and repeatable;
2. an inner subquery computes a non-trivial integer score and then performs
   `ORDER BY ... LIMIT 32`, forcing the VDBE sorter path;
3. an outer aggregate collapses the top-K rows into a single result row so the
   timed loop measures SQL execution instead of Lua row handling.

The workload is therefore not just a synthetic sort. It is a compact way to
exercise the exact combination we want to optimize:

- indexed range access,
- computed sort keys,
- coroutine-driven subquery execution,
- sorter insert / sort / fetch / next,
- final aggregation over the limited result.

### Illustrative prepared-statement bytecode

The prepared form of `sort_window` lowers to the expected coroutine + sorter
shape:

```text
0  Init
1  InitCoroutine
2  SorterOpen
3  Noop
4  Integer
5  MustBeInt
6  Integer
7  Ge
8  SetDiag
9  Halt
10 Eq
11 OpenSpace
12 IteratorOpen
13 Variable
14 IsNull
15 SeekGE
16 Variable
17 IsNull
18 IdxGT
...
30 MakeRecord
31 SorterInsert
32 Next
33 OpenPseudo
34 SorterSort
35 SorterData
36 Column
37 Column
38 Yield
39 DecrJumpZero
40 SorterNext
41 EndCoroutine
42 Null
43 InitCoroutine
44 Yield
45 Copy
46 ApplyType
47 AggStep
48 Copy
49 ApplyType
50 AggStep
51 Goto
52 AggFinal
53 AggFinal
54 Copy
55 Copy
56 ResultRow
57 Halt
```

That bytecode shape is the reason `sort_window` is useful for optimization
work: it contains the sorter, coroutine, comparison, and final aggregation
opcodes that the current CnP pass needs to cover and speed up.

## Current full matrix

All numbers below are **mean-of-3 runs** from the current
`build-jit-relwithdebinfo` matrix.

| Workload | Case | Interpreter | LLVM MCJIT | CnP JIT |
| --- | --- | ---: | ---: | ---: |
| `tiny_const` | `prepare_only` | `3.223 µs` | `5.213 µs` | `3.186 µs` |
| `tiny_const` | `prepared_execute` | `1.095 µs` | `1.025 µs` | `0.938 µs` |
| `tiny_const` | `automatic_execute` | `1.087 µs` | `0.962 µs` | `0.928 µs` |
| `hot_expr` | `prepare_only` | `5.860 µs` | `4.593 µs` | `5.902 µs` |
| `hot_expr` | `prepared_execute` | `1.258 µs` | `1.166 µs` | `1.148 µs` |
| `hot_expr` | `automatic_execute` | `1.298 µs` | `1.012 µs` | `1.005 µs` |
| `point_lookup` | `prepare_only` | `9.108 µs` | `9.426 µs` | `12.337 µs` |
| `point_lookup` | `prepared_execute` | `2.515 µs` | `2.311 µs` | `2.368 µs` |
| `point_lookup` | `automatic_execute` | `2.287 µs` | `2.331 µs` | `2.461 µs` |
| `bitwise_mix` | `prepare_only` | `21.071 µs` | `24.320 µs` | `21.039 µs` |
| `bitwise_mix` | `prepared_execute` | `3.437 µs` | `3.322 µs` | `3.272 µs` |
| `bitwise_mix` | `automatic_execute` | `3.356 µs` | `3.307 µs` | `3.323 µs` |
| `agg_scan` | `prepare_only` | `14.089 µs` | `14.319 µs` | `14.180 µs` |
| `agg_scan` | `prepared_execute` | `25.428 µs` | `26.019 µs` | `20.881 µs` |
| `agg_scan` | `automatic_execute` | `25.070 µs` | `27.348 µs` | `23.197 µs` |
| `builtin_scan` | `prepare_only` | `16.014 µs` | `16.196 µs` | `15.923 µs` |
| `builtin_scan` | `prepared_execute` | `91.421 µs` | `88.900 µs` | `82.342 µs` |
| `builtin_scan` | `automatic_execute` | `90.175 µs` | `89.283 µs` | `81.262 µs` |
| `sort_window` | `prepare_only` | `29.829 µs` | `22.050 µs` | `21.514 µs` |
| `sort_window` | `prepared_execute` | `83.824 µs` | `95.748 µs` | `79.026 µs` |
| `sort_window` | `automatic_execute` | `82.167 µs` | `82.152 µs` | `78.267 µs` |

## Practical reading of the numbers

The current matrix is a good result for CnP:

- CnP is the fastest dispatcher in **15 of 21** table cells.
- On the two execution-heavy cases (`prepared_execute`,
  `automatic_execute`), CnP wins **11 of 14** cells.
- The only clear remaining execution regression is **`point_lookup`**.
- `bitwise_mix/automatic_execute` is slightly behind LLVM MCJIT, but only by a
  noise-level margin (`3.323 µs` vs `3.307 µs`).

The strongest current wins are the workloads that motivated fragment-mode work:

- `agg_scan`: CnP is fastest in both execute modes;
- `builtin_scan`: CnP is fastest in both execute modes;
- `sort_window`: CnP is fastest in **all three** cases.

## CnP execution modes: stencils vs. preserve_none fragments

CnP operates in two modes depending on whether the VDBE program's opcodes are
fully covered by the fragment table:

- **Stencil mode** (`CNP_MODE_STENCILS`): each opcode is patched individually
  and run one-at-a-time from a C dispatch loop. `sql_cnp_step_count`
  increments for each stencil executed.

- **Fragment mode** (`CNP_MODE_FRAGMENTS`, preserve_none): pre-stitched native
  code using the fragment live-in ABI and tail-dispatch between copied opcode
  bodies. The program runs as one native chain until row / done / error exit.
  `sql_cnp_step_count` does not represent the full opcode count in this mode,
  because the outer C step loop is bypassed.

The current scan- and sorter-heavy workloads are important because they now
exercise fragment mode on realistic longer-running statements rather than only
on tiny arithmetic traces.

## Current conclusion

The current matrix supports three practical conclusions:

- **CnP is now winning almost everywhere in the benchmark matrix.** The
  remaining meaningful runtime gap is `point_lookup`; everything else is either
  a win or effectively tied.
- **`sort_window` is doing its job as the sorter-focused guide workload.** It
  exercises coroutine + sorter + aggregate bytecode and CnP is already the
  fastest mode on that path.
- **The next profile-driven pass should focus on `point_lookup` and any
  residual cursor-loop overhead, not on the sorter path that originally blocked
  fragment execution.**

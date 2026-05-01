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

## `point_lookup`: what CnP is now specializing

The current `point_lookup` optimization is much narrower than the earlier
sorter/coroutine work. The query shape is:

```sql
SELECT a + b, a - b, a * c, a / b, a % b
FROM bench_arith
WHERE id = ?;
```

The hot prepared-bytecode slice is:

```text
 7 Column      1  1 8
 8 Column      1  2 9
 9 Add         9  8 3
10 Subtract    9  8 4
11 Column      1  3 10
12 Multiply   10  8 5
13 Divide      9  8 6
14 Remainder   9  8 7
```

`bench_arith.a/b/c` are all declared as `INTEGER`, so CnP now does two layers
of per-PC retargeting on this slice:

1. each `OP_Column` site is bound to `vdbe_op_column_integer_fast()`;
2. each arithmetic site is bound to an integer-only helper
   (`vdbe_op_add_int_fast()`, `...sub...`, `...multiply...`, `...divide...`,
   `...remainder...`) when the neighboring producer ops prove the inputs come
   from exact integer columns/constants/prior integer arithmetic.

In fragment mode those bindings happen by patching the generated call site that
initially points at a generic SysV bridge symbol; the surrounding stitched
fragment layout stays the same.

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
| `tiny_const` | `prepare_only` | `4.068 µs` | `3.322 µs` | `4.455 µs` |
| `tiny_const` | `prepared_execute` | `1.122 µs` | `0.958 µs` | `1.135 µs` |
| `tiny_const` | `automatic_execute` | `1.258 µs` | `0.980 µs` | `0.922 µs` |
| `hot_expr` | `prepare_only` | `4.597 µs` | `5.012 µs` | `6.359 µs` |
| `hot_expr` | `prepared_execute` | `1.212 µs` | `1.220 µs` | `1.025 µs` |
| `hot_expr` | `automatic_execute` | `1.243 µs` | `1.205 µs` | `1.002 µs` |
| `point_lookup` | `prepare_only` | `9.705 µs` | `12.329 µs` | `10.307 µs` |
| `point_lookup` | `prepared_execute` | `2.631 µs` | `2.423 µs` | `2.473 µs` |
| `point_lookup` | `automatic_execute` | `3.643 µs` | `2.503 µs` | `2.365 µs` |
| `bitwise_mix` | `prepare_only` | `21.878 µs` | `22.624 µs` | `21.498 µs` |
| `bitwise_mix` | `prepared_execute` | `3.574 µs` | `3.388 µs` | `3.408 µs` |
| `bitwise_mix` | `automatic_execute` | `3.554 µs` | `3.394 µs` | `3.416 µs` |
| `agg_scan` | `prepare_only` | `14.409 µs` | `20.924 µs` | `14.703 µs` |
| `agg_scan` | `prepared_execute` | `25.672 µs` | `26.366 µs` | `21.396 µs` |
| `agg_scan` | `automatic_execute` | `26.873 µs` | `25.362 µs` | `21.427 µs` |
| `builtin_scan` | `prepare_only` | `20.157 µs` | `16.299 µs` | `15.598 µs` |
| `builtin_scan` | `prepared_execute` | `103.225 µs` | `91.967 µs` | `88.974 µs` |
| `builtin_scan` | `automatic_execute` | `90.513 µs` | `94.104 µs` | `87.823 µs` |
| `sort_window` | `prepare_only` | `25.257 µs` | `22.055 µs` | `21.349 µs` |
| `sort_window` | `prepared_execute` | `85.525 µs` | `79.924 µs` | `78.614 µs` |
| `sort_window` | `automatic_execute` | `80.133 µs` | `81.478 µs` | `80.997 µs` |

## Practical reading of the numbers

The current matrix is a good result for CnP:

- CnP is the fastest dispatcher in **12 of 21** table cells.
- On the two execution-heavy cases (`prepared_execute`,
  `automatic_execute`), CnP wins **9 of 14** cells outright.
- Against the generated interpreter specifically, CnP is faster in **12 of 14**
  execute-path cells; the only losses there are `tiny_const/prepared_execute`
  and `sort_window/automatic_execute`.
- `point_lookup` is no longer the remaining execution regression:
  `prepared_execute` now beats the interpreter and `automatic_execute` is the
  fastest of the three modes.

The strongest current wins are the workloads that motivated fragment-mode work:

- `agg_scan`: CnP is fastest in both execute modes;
- `builtin_scan`: CnP is fastest in both execute modes;
- `sort_window`: CnP is fastest in `prepare_only` and `prepared_execute`, and
  close to parity in `automatic_execute`.

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

- **The `point_lookup` arithmetic pass paid off.** CnP now beats the generated
  interpreter on both `point_lookup` execute paths, and `automatic_execute` is
  the fastest mode there.
- **`sort_window` is doing its job as the sorter-focused guide workload.** It
  exercises coroutine + sorter + aggregate bytecode and CnP remains strong on
  that path, especially in `prepared_execute`.
- **The remaining gaps are now narrower and more mixed.** The main execute-path
  cells to keep an eye on are `tiny_const/prepared_execute`,
  `bitwise_mix` relative to LLVM MCJIT, and `sort_window/automatic_execute`,
  while any further `point_lookup` work should be a narrow column/field-fetch
  optimization rather than another broad control-flow change.

## Focused follow-up: SQL constant folding

After the matrix above, SQL codegen was taught to fold constant integer
arithmetic expressions before VDBE bytecode emission. That means tiny traces
such as:

```sql
SELECT 1 + 2 + 3 + 4 + 5;
```

now compile down to a single literal result instead of an `Integer` / `Add`
chain.

Focused reruns after that change (`BENCH_RUNS=8`) show the expected improvement
on the tiny arithmetic workloads:

| workload | mode | generated | LLVM MCJIT | CnP |
| --- | --- | ---: | ---: | ---: |
| `tiny_const` | `prepared_execute` | `1.026 µs` | `1.026 µs` | **`0.924 µs`** |
| `tiny_const` | `automatic_execute` | `0.959 µs` | `0.973 µs` | **`0.940 µs`** |
| `hot_expr` | `prepared_execute` | `0.966 µs` | `1.009 µs` | **`0.918 µs`** |
| `hot_expr` | `automatic_execute` | `1.038 µs` | **`0.926 µs`** | `0.947 µs` |

So the old `tiny_const/prepared_execute` concern from the full matrix is no
longer the right optimization target. The SQL frontend now removes most of that
work before any backend sees it, and CnP stays competitive on the remaining
tiny-trace cost.

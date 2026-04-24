# End-to-End SQL Testsuite Benchmark

This note is for **whole-testsuite timing** only.

It is intentionally separate from
`tools/jit_bench/SQL_JIT_BENCHMARK.md`, which covers the focused
micro-benchmark matrix (`prepare_only`, `prepared_execute`,
`automatic_execute`) for interpreter, LLVM MCJIT, and CnP.

## Scope

These measurements answer a different question from the micro-benchmark:

- What is the wall-clock cost of running the SQL testsuites end to end?
- How do dispatcher and JIT modes affect overall suite throughput?
- Do the JIT backends help or hurt realistic mixed SQL workloads?

This note should contain only:

- sequential testsuite runs,
- `-j1` execution,
- final suite timings,
- mode-to-mode comparison,
- exact commands used for reproduction.

It should not mix in:

- per-query micro-benchmark numbers,
- synthetic workload counters,
- debugger or perf examples.

## Methodology

The intended end-to-end measurement procedure is:

1. Use the same build tree for all compared modes.
2. Run the SQL testsuites sequentially with `-j1`.
3. Run each mode under the same machine conditions.
4. Record the final wall-clock time reported by the test runner.
5. Compare only runs that use the same suite set and the same build type.

Recommended command shape:

```bash
cd /home/tsafin/tarantool

env VDBE_DISPATCHER=old \
    python3 test/test-run.py \
    --suite sql --builddir /path/to/build --vardir /tmp/t-sql-old \
    -j1 --force

env VDBE_DISPATCHER=generated \
    python3 test/test-run.py \
    --suite sql --builddir /path/to/build --vardir /tmp/t-sql-generated \
    -j1 --force

env VDBE_DISPATCHER=generated SQL_JIT_ENABLE=1 \
    python3 test/test-run.py \
    --suite sql --builddir /path/to/build --vardir /tmp/t-sql-mcjit \
    -j1 --force

env VDBE_DISPATCHER=cnp SQL_JIT_ENABLE=0 \
    python3 test/test-run.py \
    --suite sql --builddir /path/to/build --vardir /tmp/t-sql-cnp \
    -j1 --force
```

If the measured suite set differs from the full `sql` suite, write that down
explicitly in the results section.

## Result Format

Record results in a single table like this:

| Mode | Build | Suite set | Command shape | Wall-clock time | Delta vs baseline |
| --- | --- | --- | --- | ---: | ---: |
| `old` | `...` | `sql` | `test-run.py --suite sql -j1` | `...` | baseline |
| `generated` | `...` | `sql` | `test-run.py --suite sql -j1` | `...` | `...` |
| `generated + MCJIT` | `...` | `sql` | `test-run.py --suite sql -j1` | `...` | `...` |
| `cnp` | `...` | `sql` | `test-run.py --suite sql -j1` | `...` | `...` |

Below the table, include:

- the exact build directory,
- exact suite selection,
- whether the run was warm or cold,
- whether WAL or background activity may have affected timings,
- the date of measurement.

## Measured Results

These measurements were taken on April 25, 2026 with:

- build directory: `build-jit-relwithdebinfo`
- build type: `RelWithDebInfo`
- suite set: `sql`, `sql-luatest`, `sql-tap`
- execution mode: sequential `-j1`
- runs: one full pass per mode and suite

Measured per-suite wall-clock time:

| Suite | `old` | `generated` | `generated + MCJIT` | `cnp` |
| --- | ---: | ---: | ---: | ---: |
| `sql` | `32.157s` | `32.062s` | `66.302s` | `34.465s` |
| `sql-luatest` | `15.296s` | `15.175s` | `19.813s` | `15.097s` |
| `sql-tap` | `78.135s` | `78.255s` | `356.265s` | `74.688s` |

Combined totals across all three suites:

| Mode | Build | Suite set | Command shape | Wall-clock time | Delta vs baseline |
| --- | --- | --- | --- | ---: | ---: |
| `old` | `build-jit-relwithdebinfo` | `sql + sql-luatest + sql-tap` | `test-run.py --suite ... -j1` | `125.588s` | baseline |
| `generated` | `build-jit-relwithdebinfo` | `sql + sql-luatest + sql-tap` | `test-run.py --suite ... -j1` | `125.492s` | `-0.096s` |
| `generated + MCJIT` | `build-jit-relwithdebinfo` | `sql + sql-luatest + sql-tap` | `test-run.py --suite ... -j1` | `442.380s` | `+316.792s` |
| `cnp` | `build-jit-relwithdebinfo` | `sql + sql-luatest + sql-tap` | `test-run.py --suite ... -j1` | `124.250s` | `-1.338s` |

Observed suite outcomes were stable across modes:

- `sql`: `112 pass`, `6 disabled`
- `sql-luatest`: `46 pass`, `2 disabled`
- `sql-tap`: `485 pass`, `45 disabled`, `2 skip`

Exact mode setup:

- `old`: `VDBE_DISPATCHER=old SQL_JIT_ENABLE=0`
- `generated`: `VDBE_DISPATCHER=generated SQL_JIT_ENABLE=0`
- `generated + MCJIT`: `VDBE_DISPATCHER=generated SQL_JIT_ENABLE=1`
- `cnp`: `VDBE_DISPATCHER=cnp SQL_JIT_ENABLE=0`

Command shape actually used:

```bash
env VDBE_DISPATCHER=<mode> SQL_JIT_ENABLE=<0|1> \
    python3 test/test-run.py \
    --suite <sql|sql-luatest|sql-tap> \
    --builddir /home/tsafin/tarantool/build-jit-relwithdebinfo \
    --vardir /tmp/t-<suite>-<mode>-e2e \
    -j1 --force
```

Run notes:

- the runs were sequential and executed on the host, not in the sandbox
- the suite trees were reused between runs, so these are warm-enough practical
  branch timings, not cold-boot measurements
- `sql-tap` under MCJIT had several very slow tests (`table.test.lua`,
  `join3.test.lua`, `select9.test.lua`), which dominate the total
- no test failures were introduced by switching modes in this run

## Interpretation

Whole-testsuite timings should be read as:

- parser + planner + prepare + execute + teardown combined,
- dominated by the actual suite mix, not only hot execution loops,
- useful for regression tracking and branch-level performance claims.

They should not be used as a substitute for the focused JIT benchmark matrix.

The micro-benchmark tells us where the runtime cost moves inside the SQL
engine. The testsuite timing tells us whether those changes matter at the
product level.

## Relationship to the Micro-Benchmark

Use the two notes together:

- `tools/jit_bench/SQL_JIT_BENCHMARK.md`
  for isolated dispatcher/JIT behavior on fixed workloads.
- `docs/sql-vdbe/branch-notes/END_TO_END_SQL_BENCHMARK.md`
  for end-to-end sequential SQL testsuite timing.

This split keeps branch claims defensible:

- micro-benchmark claims stay about query-shape behavior,
- end-to-end claims stay about suite throughput.

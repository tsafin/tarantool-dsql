# Current Status Audit - 2026-04-10

## Scope

This document records what was verified locally on 2026-04-10 for the
interpreter-dispatcher refactoring and the LLVM JIT track.

The goal is to separate:

- verified facts from source and local runs;
- stale or optimistic claims in planning documents;
- missing validation work before declaring either track stable.

## Verified Build Configuration

Existing local build tree: `build/`

Verified from `build/CMakeCache.txt`:

- `CMAKE_BUILD_TYPE=Debug`
- `VDBE_USE_GENERATED_DISPATCH=ON`
- `ENABLE_SQL_JIT=OFF`
- `ENABLE_VDBE_GOTO_DISPATCH=OFF`

## Verified Source-Level Status

### Interpreter / dispatcher track

What exists in code:

- `sqlVdbeExec()` invokes the JIT first when enabled, then dispatches through
  the generated-dispatcher selection path, then falls back to the old inline
  dispatcher.
- The generated dispatcher is implemented in
  `src/box/sql/vdbe_dispatch_wrapper.c`.
- Runtime dispatcher mode selection exists via `VDBE_DISPATCHER` with
  `old|generated|parallel|auto`.
- The unconditional generated-dispatcher `fprintf(stderr, ...)` tracing has
  been removed locally.

Important implication:

- The generated dispatcher is integrated and callable.
- A verified external old/generated equivalence harness now exists in
  `tools/verify_dispatchers_equivalence.lua` and
  `tools/verify_dispatchers_equivalence.sh`.
- That harness found a real generated-dispatch regression in `OP_SkipLoad`,
  which is now fixed locally.
- The interpreter track is materially further along than the earlier audit
  snapshot, but it still lacks regular-suite and fuzz integration.

### "Parallel validation" status

The repository contains validation scaffolding, but it is not true lock-step
validation yet.

Verified behavior in code:

- `vdbe_exec_parallel_validation()` now fails fast with a diagnostic instead of
  recursing through `sqlVdbeExec()` twice.
- `vdbe_validate_state()` compares only return codes.
- It does not compare register state, cursor state, result rows, side effects,
  or per-opcode state transitions.

Current conclusion:

- The in-process validation framework is still only a skeleton.
- There is now a working external dispatcher-equivalence harness.
- Any claim of in-process "lock-step validated" should still be treated as not
  yet proven.

### JIT track

What exists in code:

- `vdbe_jit.c` and `vdbe_jit.h` exist.
- JIT hooks are integrated into prepare-time (`sqlVdbeMakeReady()`), execution
  (`sqlVdbeExec()`), and cleanup (`sqlVdbeClearObject()`).
- `struct Vdbe` contains JIT-specific fields.
- The current implementation is LLVM-specific and uses LLVM C API types
  directly.
- The implementation comments/docs say "OrcJIT", but the code currently
  initializes MCJIT / `LLVMExecutionEngine`.

Current conclusion:

- JIT integration exists in source.
- It is not yet backed by a fully verified clean-build + test matrix.

## Verified Local Runs

All runs below were performed on 2026-04-10 from the `build/` directory unless
stated otherwise.

### 1. Rebuild current non-JIT build

Command:

```bash
make -C build tarantool
```

Result:

- Success.
- `build/src/tarantool` was rebuilt successfully.

Conclusion:

- The current non-JIT Debug build is buildable in the existing local build
  tree.

### 2. `test_phase58.lua` regression script

Commands:

```bash
./src/tarantool ../test_phase58.lua
env VDBE_DISPATCHER=old ./src/tarantool ../test_phase58.lua
```

Observed result after the tuple-constraint fix:

- `VDBE_DISPATCHER=old`: passes `70/70`
- `VDBE_DISPATCHER=generated`: passes `70/70`

Conclusion:

- `test_phase58.lua` is now usable again as a targeted regression gate for this
   work.
- The previous blocker was real, shared by both dispatcher modes, and is now
  fixed locally.
- This check is now part of the scripted baseline matrix in
  `tools/verify_dispatcher_baseline.sh`.

### 3. Direct SQL probe under old dispatcher

Command (abridged):

```bash
env VDBE_DISPATCHER=old ./src/tarantool -e '... box.execute("SELECT * FROM test_basic") ...'
```

Observed result:

- `box.execute("SELECT * FROM test_basic")` returned `nil`.
- The process then failed when the probe tried to access `r.rows`.

Conclusion:

- This was not a dispatcher regression.
- The probe failed because sequential scan is disabled by default in this
  environment and the query errored with "Scanning is not allowed for
  'test_basic'".
- After setting `SET SESSION sql_seq_scan = true`, the smoke probe worked in
  both `old` and `generated` modes.

### 4. `tools/verify_dispatchers_simple.lua`

The script required two audit fixes before it was useful:

- removed socket binding from `box.cfg{}` so it can run in this environment;
- updated result handling to use `result.rows` instead of `result:len()`.

Verified smoke results after that:

#### Old dispatcher

Command:

```bash
env VDBE_DISPATCHER=old ./src/tarantool ../tools/verify_dispatchers_simple.lua
```

Observed result:

- Passed all checks.

#### Generated dispatcher

Command:

```bash
env VDBE_DISPATCHER=generated ./src/tarantool ../tools/verify_dispatchers_simple.lua
```

Observed result:

- Same functional result as old dispatcher on this smoke test: all checks
  passed.
- No unconditional generated-dispatcher trace spam was emitted by the rebuilt
  binary.

Conclusion:

- On this smoke test, generated matches old.
- The obvious stderr noise issue is fixed locally, but that alone does not make
  the generated dispatcher release-ready.
- This smoke check is now part of `tools/verify_dispatcher_baseline.sh`.

### 5. External old/generated equivalence harness

New scripts added during this audit:

- `tools/verify_dispatchers_equivalence.lua`
- `tools/verify_dispatchers_equivalence.sh`

Verified command:

```bash
bash tools/verify_dispatchers_equivalence.sh
```

Observed result after fixing `OP_SkipLoad`:

- `Dispatcher equivalence verified`

What this harness does:

- runs the same deterministic SQL workload in fresh temp directories;
- executes it once with `VDBE_DISPATCHER=old` and once with
  `VDBE_DISPATCHER=generated`;
- captures canonical JSON traces for each step;
- compares step-by-step results outside the process.

Important note:

- This is the current supported equivalence gate.
- It is not the same thing as the still-unimplemented in-process `parallel`
  mode.
- It is also included in `tools/verify_dispatcher_baseline.sh`, so the current
  supported old/generated matrix has a single entrypoint.

### 5a. Representative SQL TAP subset

Verified follow-up run against the rebuilt binary with explicit `test-run.py`
paths and isolated vardirs:

- `sql-tap/select1.test.lua`
- `sql-tap/check.test.lua`
- `sql-tap/trigger1.test.lua`
- `sql-tap/limit.test.lua`
- `sql-tap/join.test.lua`
- `sql-tap/join2.test.lua`
- `sql-tap/join3.test.lua`
- `sql-tap/orderby1.test.lua`
- `sql-tap/orderby2.test.lua`
- `sql-tap/distinct.test.lua`
- `sql-tap/subselect.test.lua`
- `sql-tap/where2.test.lua`
- `sql-tap/where3.test.lua`

Observed result after fixing generated-dispatch error-path issues:

- `VDBE_DISPATCHER=old`: passes
- `VDBE_DISPATCHER=generated`: passes

Operational note:

- `test-run.py` must be pointed explicitly at `build/src/tarantool`; otherwise
  it may pick up a stale `src/tarantool` binary from the source tree.
- Old/generated suite runs must use separate `--vardir` values; concurrent runs
  against the default vardir can produce WAL lock false failures.
- The scripted entrypoint for this subset is now
  `tools/verify_dispatcher_sql_subset.sh`.

### 6. Parallel mode

Command:

```bash
env VDBE_DISPATCHER=parallel ./src/tarantool ../tools/verify_dispatchers_simple.lua
```

Observed result:

- Originally aborted with:

```text
*** stack smashing detected ***: <unknown> terminated
```

- During this audit, `parallel` mode was changed to fail fast with a clear SQL
  error instead of recursing into stack corruption.

Conclusion:

- Parallel mode is not stable enough to use as a validation gate.
- This is consistent with the source audit: the mode is still a scaffold, not a
  finished equivalence harness.
- The scripted baseline treats `VDBE_DISPATCHER=parallel` as an expected
  failure/placeholder check rather than as a passing validation mode.

### 7. Stable CHECK-constraint repro in old mode

Command pattern:

```bash
env VDBE_DISPATCHER=old ./src/tarantool /tmp/test_phase58_temp.lua
```

Where `/tmp/test_phase58_temp.lua` is a copy of `test_phase58.lua` modified
only to:

- use a fresh temp `memtx_dir`/`wal_dir`/`vinyl_dir`;
- set `io.stdout:setvbuf("no")` for unbuffered progress output.

Observed result:

- The script consistently reaches section 7 and prints:

```text
OK  create with CHECK
```

- It then aborts on the first valid insert into a table with a tuple-level
  SQL CHECK constraint:

```text
tarantool: ./src/box/tuple.h:1288: mp_tuple_assert:
Assertion `mp_typeof(*tuple) == MP_ARRAY' failed.
```

Minimal repro:

```sql
CREATE TABLE t_ck (id INT PRIMARY KEY, score INT, CHECK(score >= 0));
INSERT INTO t_ck VALUES (1, 100);
```

### 8. GDB backtrace summary for the CHECK crash

Using `gdb` on the minimal repro in `VDBE_DISPATCHER=old`, the assertion stack
shows:

- `tuple_constraint_call_func()` calls `port_get_msgpack()` on the SQL_EXPR
  result port;
- `port_sql_get_msgpack()` delegates to `port_c_get_msgpack()`;
- `port_c_get_msgpack()` sees an empty port and returns an empty msgpack array;
- tuple-constraint code then decodes that invalid result as if it contained a
  boolean return value;
- the bad msgpack then flows into `tarantoolsqlInsert()` /
  `vdbe_op_idx_insert_replace()` and reaches `mp_tuple_assert()`.

Relevant frames from the verified backtrace:

- `src/box/tuple_constraint_func.c:105`
- `src/box/port.c:323`
- `src/box/sql/port.c:369`
- `src/box/sql.c:464`
- `src/box/sql/vdbe_ops_index.c:346`

Root cause and current status:

- The problem was tuple-level SQL CHECK constraint execution around SQL_EXPR
  result-port handling.
- SQL_EXPR constraint functions returned a single scalar boolean in a C port,
  but tuple-constraint code consumed it through the generic
  `port_get_msgpack()` wrapper path.
- The local fix decodes SQL_EXPR constraint results directly from the first
  port entry instead of routing them through the generic wrapper.
- After that change, the section-7 crash is gone and the full `test_phase58.lua`
  run passes in both `old` and `generated` modes.

## Verified Test Coverage Situation

### What exists

- A large general SQL test corpus exists under `test/sql`, `test/sql-tap`,
  `test/sql-luatest`, and related directories.
- A generic SQL fuzz target exists under `test/fuzz/sql_fuzzer`.
- Project-local VDBE scripts exist:
  - `test_phase58.lua`
  - `tools/verify_dispatchers_simple.lua`

### What is missing or not yet proven

- No verified integration of dispatcher mode matrix into the regular SQL test
  suites.
- A small scripted regular-suite subset now exists via
  `tools/verify_dispatcher_sql_subset.sh`, but broad suite coverage is still not
  in place.
- No in-process lock-step old/generated validator.
- No broad regular-suite integration of the new external equivalence harness.
- No dispatcher-specific fuzzing matrix across `old`, `generated`, and `JIT`.
- No verified JIT-enabled regression run in this audit.
- No verified Release-build performance numbers in this audit.

## Verified JIT Buildability Status

### Fresh JIT configure

Fresh configure succeeded in `/tmp/tarantool-jit-audit` and
`/tmp/tarantool-jit-audit2` with:

```bash
-DENABLE_SQL_JIT=ON
-DVDBE_USE_GENERATED_DISPATCH=ON
```

Observed notes:

- CMake found LLVM 11.1.0 through `LLVMConfig.cmake`.
- The host `llvm-config --version` reports `7.0.1`, so PATH and CMake are not
  selecting the same LLVM installation.
- The build warns that LLVM 12+ is recommended.

### Fresh JIT build

Observed results:

- First fresh JIT build attempt failed early in bundled `libunwind` setup.
- Second fresh JIT configure with `-DENABLE_BUNDLED_LIBUNWIND=OFF` progressed
  substantially further.
- A full clean JIT build was not completed within this audit window.

Conclusion:

- JIT clean-configure is partially verified.
- JIT clean-build is not yet verified end-to-end.
- Therefore JIT cannot currently be marked "buildable and runnable" with the
  same confidence as the non-JIT local build.

## Current Status Summary

### Interpreter dispatcher refactor

Status:

- Buildable in the existing local non-JIT build tree: yes.
- Integrated into execution path: yes.
- Runnable for basic operations: yes.
- `test_phase58.lua` in `old` and `generated`: yes.
- External old/generated equivalence harness: yes.
- Stable enough to declare complete: no.

Why not complete yet:

- the in-process `parallel` mode is still not a real validator;
- the new equivalence harness plus scripted SQL TAP subset improve confidence,
  but still do not cover a broad regular test matrix;
- no dispatcher-mode fuzzing or wider regular-suite coverage has been added yet;
- JIT-enabled parity testing still has not been completed.

### LLVM JIT refactor

Status:

- Source integration exists: yes.
- Clean configure with JIT enabled: yes.
- Clean end-to-end build verification: not yet proven in this audit.
- Runtime verification in JIT-enabled binary: not yet proven in this audit.

Why not complete yet:

- clean JIT build path is not fully verified;
- no JIT-enabled regression matrix was executed;
- no JIT stability or performance data was collected in this audit.

## Immediate Recommendations

1. Add the external old/generated equivalence harness to a broader SQL test
   subset.
2. Decide whether to keep `parallel` as a fail-fast placeholder or remove it
   until a true in-process validator exists.
3. Complete one clean JIT-enabled build and then run the same smoke/regression
   matrix against it.
4. Delay performance claims until a Release build and stable test matrix exist.

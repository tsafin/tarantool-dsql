# Stabilization Plan - Interpreter, LLVM JIT, and Performance

## Goal

Bring the VDBE refactor work to a state where all three claims are defensible:

1. the interpreter refactor is functionally stable;
2. the LLVM JIT path is integrated and tested to the same standard;
3. performance claims are based on repeatable Release-build measurements.

## Guiding Principle

Do not advance to JIT stabilization or benchmarking while the baseline SQL
execution path still has known regressions that reproduce in the old
dispatcher.

The 2026-04-10 audit found exactly that situation.

## Phase 0 - Re-establish a Trustworthy Baseline

### Objective

Fix the shared baseline failures that block meaningful comparison work.

### Required work

1. Diagnose why `box.execute("SELECT * ...")` returns `nil` in this checkout.
2. Diagnose the `mp_tuple_assert(mp_typeof(*tuple) == MP_ARRAY)` abort reached
   by `test_phase58.lua`.
3. Re-run the direct probe and the smoke script in `old` mode only.
4. Record the exact root cause and fix commit(s).

Status correction from the audit:

- The `SELECT *`/`box.execute()` issue was not a core SQL regression. It was
  caused by `sql_seq_scan` being disabled by default in the local environment.
- The actual shared blocker is the tuple-level SQL CHECK path. A minimal repro
  is:

```sql
CREATE TABLE t_ck (id INT PRIMARY KEY, score INT, CHECK(score >= 0));
INSERT INTO t_ck VALUES (1, 100);
```

- A verified `gdb` backtrace shows the crash path crossing:
  - `tuple_constraint_call_func()`
  - `port_sql_get_msgpack()`
  - `port_c_get_msgpack()`
  - `tarantoolsqlInsert()`
  - `vdbe_op_idx_insert_replace()`

Immediate focus inside Phase 0 was therefore the SQL_EXPR
constraint-result handling bug, not generic SELECT execution.

### Exit criteria

- Plain `SELECT *`, `SELECT ... WHERE`, and `SELECT` with arithmetic
  expressions work in `VDBE_DISPATCHER=old` once `sql_seq_scan` is enabled for
  the session.
- `test_phase58.lua` no longer aborts in `VDBE_DISPATCHER=old`.
- `test_phase58.lua` also passes in `VDBE_DISPATCHER=generated`.

Current Phase 0 status:

- The CHECK-constraint crash has been fixed locally.
- The simple dispatcher smoke script and the `test_phase58.lua` matrix now pass
  in both `old` and `generated` modes.
- The generated-dispatcher stderr trace noise has been removed locally.
- A real external old/generated comparison harness now exists and passes.
- That harness found and helped fix a real generated-dispatch bug in
  `OP_SkipLoad`.
- The remaining interpreter-track blocker is broader validation coverage, not
  the original baseline crash.

## Phase 1 - Interpreter Refactor Stabilization

### Objective

Prove that old and generated dispatchers are functionally equivalent on an
explicit test matrix.

### Required work

1. Remove generated-dispatcher debug `fprintf(stderr, ...)` traces.
2. Replace the current fake "parallel validation" with a real comparison
   harness.

Real lock-step validation must compare at least:

- SQL return code;
- `box.execute()` result object shape;
- row data;
- row count;
- final `p->pc`;
- selected register state and cursor invariants where practical.

3. Run the smoke matrix in supported modes:
   - `old`
   - `generated`
   - use `tools/verify_dispatcher_baseline.sh` as the baseline entrypoint
4. Run a targeted SQL regression subset in both `old` and `generated`.
5. Add dispatcher-mode coverage to at least one regular test entrypoint so the
   work is no longer validated only by ad hoc scripts.

### Minimum test matrix

- `tools/verify_dispatchers_simple.lua`
- `test_phase58.lua`
- `tools/verify_dispatcher_sql_subset.sh`
- representative tests from:
  - `test/sql`
  - `test/sql-tap`
  - `test/sql-luatest`

### Exit criteria

- `old` and `generated` both pass the agreed SQL regression subset.
- The supported old/generated comparison harness reports zero functional
  differences on the target matrix.
- Generated dispatcher is free of debug stderr noise.

## Phase 2 - Complete LLVM JIT Build Integration

### Objective

Make the JIT path cleanly buildable from a fresh tree and clearly document the
required environment.

### Required work

1. Complete one clean JIT-enabled build from scratch.
2. Resolve environment-sensitive build issues found in the audit:
   - bundled vs system `libunwind`;
   - LLVM installation selection mismatch (`llvm-config` vs `LLVMConfig.cmake`);
   - any LLVM 11 compatibility issues.
3. Write down the supported configuration matrix:
   - minimum LLVM version actually tested;
   - recommended LLVM version;
   - required CMake options;
   - whether bundled/system libunwind is required.
4. Decide whether the implementation is officially MCJIT-based or move it to a
   real ORC implementation to match the documentation.

### Exit criteria

- Fresh-tree JIT configure succeeds.
- Fresh-tree JIT build succeeds.
- JIT build instructions are written and accurate.

## Phase 3 - JIT Functional Stabilization

### Objective

Test the JIT path against the same functional standard as the interpreter
refactor.

### Required work

1. Add a JIT-enabled smoke matrix:
   - JIT disabled, old dispatcher
   - JIT disabled, generated dispatcher
   - JIT enabled, fallback allowed
2. Add explicit tests for:
   - prepare-time compilation succeeds/fails cleanly;
   - unsupported opcodes fall back correctly;
   - `OP_Program` fallback path remains correct;
   - result-row behavior remains correct;
   - cleanup path releases/forgets JIT resources safely.
3. Add instrumentation or logs sufficient to answer:
   - was the query JIT-compiled?
   - did it run JIT code?
   - where did it fall back?
4. Reuse the same SQL regression subset from Phase 1 under JIT-enabled builds.

### Exit criteria

- JIT-enabled smoke tests pass.
- JIT-enabled regression subset passes.
- JIT fallback behavior is explicitly tested.
- No crashes or state corruption on mixed JIT/interpreter execution.

## Phase 4 - Fuzzing and Stress Validation

### Objective

Move from scripted confidence to adversarial confidence.

### Required work

1. Reuse `test/fuzz/sql_fuzzer` against:
   - non-JIT old dispatcher
   - non-JIT generated dispatcher
   - JIT-enabled build
2. Add a simple mode matrix around the fuzzer entrypoint so failures can be
   attributed to:
   - baseline SQL bug;
   - generated dispatcher bug;
   - JIT bug.
3. Preserve and minimize crashing inputs.
4. Track unique failures separately by engine/mode.

### Exit criteria

- Fuzzing runs exist for all target modes.
- Unique crashes are triaged and tracked.
- No known unreduced crashers remain for the release target.

## Phase 5 - Performance Evaluation

### Objective

Produce credible numbers only after correctness is stable.

### Rules

- Use Release builds only.
- Do not compare Debug JIT vs Debug interpreter.
- Record both prepare-time overhead and execution-time benefit.

### Required measurements

1. `old` vs `generated` dispatcher on representative SQL workloads.
2. `generated` vs `JIT` on the same workloads.
3. Prepare latency:
   - no JIT
   - JIT compile enabled
4. Steady-state execution latency and throughput.
5. Fallback-heavy queries vs JIT-friendly queries.

### Suggested workload buckets

- point lookup
- index range scan
- ORDER BY / sorter-heavy query
- aggregation
- trigger / sub-program query
- DDL / schema-change statements

### Exit criteria

- Benchmark scripts and exact build flags are documented.
- Results are reproducible.
- Each published comparison states:
  - build type;
  - dispatcher mode;
  - JIT setting;
  - workload;
  - sample size.

## Recommended Execution Order

1. Finish Phase 0.
2. Finish Phase 1.
3. Finish Phase 2.
4. Finish Phase 3.
5. Run Phase 4 continuously while Phases 1-3 are in progress.
6. Start Phase 5 only after Phases 1-3 have passed.

## Current Priority

As of 2026-04-10, the highest-priority blocker is not LLVM itself.

It was the shared baseline SQL CHECK-constraint failure that aborted
`test_phase58.lua` in both dispatcher modes. That blocker is now fixed locally.

The next blockers are the absence of real lock-step validation, the unstable
`parallel` mode, and the lack of equivalent JIT-enabled test coverage.

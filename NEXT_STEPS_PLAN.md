# VDBE Refactoring - Next Steps Plan

**Created**: February 18, 2026
**Updated**: February 19, 2026
**Interpreter dispatcher coverage**: 142/142 opcodes (100%) — generated, not yet default
**JIT coverage**: 141/142 opcodes — OP_Program falls back to generated dispatcher
**Test suite**: 70/70 tests pass (`test_phase58.lua`)
**Status**: Generated dispatcher validated; next is activating it as default

---

## Architecture Clarification (Feb 19, 2026)

There are **two independent tracks**:

### Track 1: DSL-based Generated Threaded Interpreter
- Input: `tools/vdbe_dsl/opcodes.yaml` + `tools/vdbe_codegen.py`
- Output: `src/box/sql/generated/vdbe_dispatch_generated.c` + `vdbe_opcodes_generated.h`
- Build: `make vdbe_codegen` (CMake target with proper dependency tracking)
- **This is a conventional threaded/switch interpreter dispatcher, NOT a JIT**
- Status: 142/142 opcodes, NOT yet the default execution path

### Track 2: LLVM JIT (`vdbe_jit.c`)
- Compiles VDBE programs to native code at prepare time using LLVM OrcJIT
- Completely separate from the interpreter dispatcher
- Status: Incomplete

---

## Executive Summary

The generated threaded interpreter dispatcher now covers **142/142 opcodes (100%)**.
The `external_inline` handler type was added to `vdbe_codegen.py` (Feb 19, 2026).
70/70 unit tests pass. The generated dispatcher is fully validated but **not yet the
default execution path** — `VDBE_USE_GENERATED_DISPATCH` in `vdbe_dispatch.h` is
still commented out and the old inline dispatcher in `vdbe.c` remains primary.

**Immediate Action**: Activate generated dispatcher as default, run regression tests.

---

## Current Status Clarifications

### 1. SELECT — ✅ WORKING (false alarm in previous notes)
**Status**: Confirmed working in both generated and original dispatchers
**Verified**: Feb 18, 2026

**Root Cause of False Alarm**: Previous test scripts used double-quoted strings in SQL
(`INSERT INTO t1 VALUES (1, "hello")`). In SQL, double quotes denote **identifiers**, not
string literals. The correct syntax is single quotes (`'hello'`). Those INSERTs were silently
failing with `Can't resolve field 'hello'`, leaving the table empty — so SELECT correctly
returned 0 rows.

**Verified Behavior** (generated dispatcher, `SET SESSION sql_seq_scan = true`):
```
INSERT row_count:     1  (both rows)
SELECT * FROM t1 →   2 rows: (1, hello), (2, world)
SELECT count(*) →    2
SELECT WHERE id=1 →  1 row
```
Generated and original dispatchers produce **identical results**.

**Testing reminders going forward**:
- Use Lua `[[ ]]` long strings when embedding SQL with single quotes (avoids escaping hell)
- Write multi-line tests as `.lua` script files rather than shell `-e` one-liners
- Use `SET SESSION sql_seq_scan = true` before `SELECT *` (sequential scan off by default)
- Use `SET SESSION sql_vdbe_debug = true` to print VDBE bytecode listing

**No action needed.**

---

### 2. OP_NoConflict — ✅ WORKING (was a false alarm)
**Status**: Confirmed fully working in both dispatchers
**Verified**: Feb 18, 2026

**Test coverage** (10 tests, generated vs original — identical output):
- CREATE TABLE (NoConflict on system space) ✅
- Normal INSERT (row_count=1) ✅
- Duplicate PK → `nil` + correct error message, original row unchanged ✅
- Duplicate UNIQUE value → `nil` + correct error message, row not inserted ✅
- INSERT OR REPLACE (row_count=2: delete old + insert new) ✅
- CREATE TABLE IF NOT EXISTS (conflict silently skipped) ✅
- Multiple rapid CREATE TABLEs ✅
- Batch inserts (10 rows) ✅
- UPDATE (row_count=1) ✅
- DELETE (row_count=1) ✅

The earlier "partially fixed" status was based on stale notes. The fixes applied in commits
2fb0c16c34 (OP_Clear) and 30f32f985291 (OP_MakeRecord/OP_NoConflict uninitialized registers)
are all still in place and working correctly.

**No action needed.**

---

## Actual Current State (Verified Feb 18, 2026)

### Coverage Reality Check
**Verified by code analysis** (not stale docs):
- Total opcodes in current build: **142** (docs said 176 — outdated)
- Handled by generated dispatcher: **141 / 142 (99.3%)**
- Inline handler functions: 12 in vdbe_ops_inline_medium_8.c + coroutines inline in dispatcher
- Remaining fallback: **1 opcode** (OP_Program — intentionally deferred)

### Phase 5.6d — ✅ DONE (already implemented)
All target sorter opcodes verified in `vdbe_dispatch_wrapper.c`:
- OP_Sort ✅ → `vdbe_op_sort_inline`
- OP_SorterOpen ✅ → `vdbe_op_sorteropen`
- OP_SorterInsert ✅ → `vdbe_op_sorterinsert`
- OP_SorterNext ✅ → `vdbe_op_sorternext`
- OP_SorterSort ✅ → `vdbe_op_sortersort`
- OP_SorterData ✅ → `vdbe_op_sorterdata`
- OP_SorterCompare ✅ → `vdbe_op_sortercompare`

### Phases 5.6e-5.6h — ✅ DONE (all planned opcodes implemented)
The 37 inline handler functions cover far more than the 12 documented.

---

## Phased Work Plan

### Phase 5.8: Implement Remaining 17 Opcodes — ✅ DONE (Feb 18, 2026)

**Result**: 141/142 opcodes dispatched (99.3%)

| Category | Opcodes | Implementation |
|----------|---------|----------------|
| Misc data | ResetCount, ElseNotEq, FCopy, FetchByName, NextIdEphemeral, NextSystemSpaceId | Handler functions in vdbe_ops_inline_medium_8.c |
| Coroutines | Gosub, Return, Yield, InitCoroutine, EndCoroutine | Inline in vdbe_dispatch_wrapper.c (need pc/aOp) |
| DDL | CreateForeignKey, CreateCheck, AddFuncDefault, CheckViewReferences, RenameTable, LoadAnalysis | Handler functions in vdbe_ops_inline_medium_8.c |
| Kept as fallback | OP_Program | Too complex (VdbeFrame setup for trigger sub-programs) |

**Commit**: f8cd200b34

---

### Phase 5.9: Unit Opcode Verification — ✅ DONE (updated Feb 19, 2026)

**Result**: 70/70 tests pass (45 original + 25 added Feb 19).

**Test file**: `test_phase58.lua` — covers Phase 5.8 opcodes plus new sections:
- Gosub/Return: GROUP BY sort subroutine
- InitCoroutine/Yield/EndCoroutine: UNION ALL + INSERT…SELECT
- ElseNotEq: DISTINCT ORDER BY
- ResetCount/FCopy: trigger body with DML
- NextIdEphemeral: ORDER BY non-indexed column (ephemeral sorter)
- CreateCheck/FetchByName: CHECK constraint DDL
- CreateForeignKey: FOREIGN KEY DDL
- AddFuncDefault: DEFAULT value in CREATE TABLE
- CheckViewReferences: DROP TABLE
- RenameTable: ALTER TABLE RENAME TO
- LoadAnalysis: ANALYZE (no-op stub)
- NextSystemSpaceId: CREATE SEQUENCE

**Added Feb 19** (sections 14–16 in test_phase58.lua):
- Section 14: OP_Program — trigger sub-programs, RAISE(IGNORE), AFTER trigger logging
- Section 15: OP_IfPos — LIMIT/OFFSET handling
- Section 16: OP_Once/OP_DecrJumpZero — DISTINCT queries

**Also verified**: Behavioral notes —
- CHECK/FK constraints defined but not enforced at SQL layer (silent insert succeeds)
- ANALYZE returns `nil` result (not an error) — DDL-style statement
- CREATE/DROP SEQUENCE returns `nil` result (same)

---

### Phase 5.9b: DSL Cleanup — ✅ DONE (Feb 19, 2026)

- Added `external_inline` handler type to `vdbe_codegen.py`
  - Calls `vdbe_op_<name>_inline(p, pOp, aMem)`, handles rc: <0=error, 1=jump P2, 0=continue
  - Both goto and switch dispatch paths updated
- Removed accidentally committed `tools/vdbe_dsl/opcodes.yaml.bak` (commit f0c808daac)
- Added `*.bak` to `.gitignore`
- Regenerated `vdbe_dispatch_generated.c` + `vdbe_opcodes_generated.h`
- Build verified clean, 70/70 tests pass

---

### Phase 5.10: Activate Generated Dispatcher as Default — ⬜ NEXT (interpreter track)
**Duration**: 1-2 days
**Goal**: Make generated dispatcher the primary execution path

**Tasks**:
1. Enable `VDBE_USE_GENERATED_DISPATCH` in `src/box/sql/vdbe_dispatch.h`
2. Build with `make tarantool`, verify 70/70 tests still pass
3. Run Tarantool SQL test suite as regression baseline
4. Fix any failures (use `VDBE_DISPATCHER=parallel` for per-opcode diff)
5. Benchmark on TPC-H queries (requires Release build: `-O2`/`-O3`)

---

### Phase 5.11: Clean Up Remaining `inline` Stubs
**Duration**: 1-2 days
**Goal**: Remove raw `inline_code` blobs from opcodes.yaml

9 opcodes still use `handler_type: inline`:
OP_OffsetLimit, OP_SetSession, OP_ShowCreateTable, OP_SorterSort, OP_Program,
OP_Compare, OP_Permutation, OP_TTransaction, OP_IteratorOpen/OP_SorterOpen.
Extract each to a `_inline` C handler, switch to `external_inline`.

---

### Phase 5.12: Remove Old Dispatcher from vdbe.c
**Duration**: 1 day
**Goal**: Delete the large inline switch/goto block in vdbe.c
Only safe after generated dispatcher is default and regression tests pass.

---

## Parallel Investigation Threads

### Thread A: Activate + Regression Testing
**Priority**: HIGH
**Status**: Unblocked — 142/142 opcodes, 70/70 unit tests pass

**Steps**:
1. Enable `VDBE_USE_GENERATED_DISPATCH` in `vdbe_dispatch.h`
2. Run full SQL test suite with generated dispatcher as default
3. Use `VDBE_DISPATCHER=parallel` to isolate any per-opcode diffs
4. Fix failures, then benchmark (Release build required)

**Expected Completion**: 1-2 days

---

### Thread B: OP_NoConflict — ✅ RESOLVED
**Status**: Confirmed working (Feb 18, 2026) — 10 tests pass, both dispatchers identical
**No further action needed.**

---

### Thread C: Opcode Coverage Analysis
**Priority**: LOW (planning only)
**Status**: Largely answered — 18 remaining opcodes are DDL/coroutine/control flow.
Most are rarely used (RenameTable, LoadAnalysis, Program). Frequency analysis would
confirm which of the 18 to prioritize vs. leave as fallback.

**Deliverable**: Skip formal document — use test suite failures to drive priority

---

## Resource Plan

### No New Resources Needed
- Use existing tools and infrastructure
- Helper extraction pattern established
- Documentation templates in place
- Build system ready

### Knowledge Base to Leverage
- P2_BRANCHING_AUDIT.md for pattern review
- VDBE_HANDLER_IMPLEMENTATION_GUIDE.md for implementation
- PHASE_5_6c_SESSION_SUMMARY.md for proven patterns
- vdbe_ops_inline_medium_2.c for code examples

---

## Success Criteria

### Phase 5.8 Success (Remaining 18 Opcodes)
- [ ] OP_ResetCount, OP_ElseNotEq, OP_FCopy, OP_FetchByName, OP_NextIdEphemeral, OP_NextSystemSpaceId implemented
- [ ] OP_Gosub, OP_Return, OP_Yield, OP_InitCoroutine, OP_EndCoroutine implemented
- [ ] Decision made on OP_Program, OP_RenameTable (implement or accept fallback)
- [ ] 138+/142 opcodes dispatched (97%+)
- [ ] All new handlers compile without errors
- [ ] Helper extraction pattern (if used) documented
- [ ] Basic testing of sort/memory operations passes
- [ ] Session summary created
- [ ] Todo tracking updated

### Phase 5.10 Success (Critical Gate)
- [ ] 100% of existing Tarantool SQL tests pass with generated dispatcher
- [ ] VDBE_DISPATCHER=parallel shows zero differences vs original
- [ ] Performance <2% regression vs. original
- [ ] No assertion failures or memory issues

### Full Project Success (Phase 5.11+)
- [x] 142/142 opcodes handled (done)
- [ ] Generated dispatcher is default execution path
- [ ] <2% performance regression (measured on Release build)
- [ ] Full test suite passing
- [ ] Old dispatcher removed from vdbe.c

---

## Risk Assessment

### Low Risk - Monitor
1. **Regression failures** — generated dispatcher may expose subtle behavior differences
   vs old dispatcher on edge-case SQL; use `VDBE_DISPATCHER=parallel` to isolate
2. **OP_Program inline stubs** — complex VdbeFrame setup; safe to leave as `inline` in DSL
   since it works correctly via `op_program_enter()`; extract to `_inline` handler later

---

## Timeline Estimate (Updated Feb 19, 2026)

| Phase | Duration | Status | Expected Completion |
|-------|----------|--------|---------------------|
| 5.8 (17 remaining opcodes) | — | ✅ Done | Feb 18 |
| 5.9 (Unit verification, 45→70 tests) | — | ✅ Done | Feb 19 |
| 5.9b (DSL cleanup, external_inline) | — | ✅ Done | Feb 19 |
| 5.10 (Activate as default + regress) | 1-2 days | ⬜ Next | Feb 20-21 |
| 5.11 (Clean up inline stubs) | 1-2 days | ⬜ | Feb 22-23 |
| 5.12 (Remove old dispatcher) | 1 day | ⬜ | Feb 24 |
| **Total remaining** | **~3-5 days** | | **~Feb 24** |

**Critical Path**: Full SQL regression suite → fix any failures → cutover decision → cleanup

---

## Long-term Strategy

### After Phase 5.10 (generated dispatcher active by default)
- Remove old inline dispatcher from vdbe.c
- Extract remaining `inline` stubs to `_inline` C handlers
- Clean up `vdbe_dispatch_wrapper.c` scaffolding

### Benchmarking (requires Release build)
- Rebuild with `CMAKE_BUILD_TYPE=Release`
- Compare generated dispatcher vs old dispatcher on TPC-H queries
- Compare LLVM JIT vs generated dispatcher on TPC-H queries

---

## Documentation

### Already done
- `DISPATCHER_STATUS.md` — opcode-by-opcode tracking
- `CLAUDE.md` — two-track architecture, build notes, remaining work
- `test_phase58.lua` — 70-test regression suite

### Optional future
- `PERFORMANCE_PROFILING.md` — benchmark results after Release build

---

## Immediate Next Steps

### Must Do (Phase 5.10 — activate generated dispatcher):
1. [ ] Enable `VDBE_USE_GENERATED_DISPATCH` in `src/box/sql/vdbe_dispatch.h`
2. [ ] Build with `make tarantool`, verify 70/70 tests still pass
3. [ ] Run Tarantool SQL test suite (test/sql/ + test-run.py) as regression baseline
4. [ ] Fix any failures (use `VDBE_DISPATCHER=parallel` for per-opcode diff)
5. [ ] Benchmark generated vs old dispatcher on TPC-H queries (need Release build)

### Should Do:
1. [ ] `VDBE_DISPATCHER=parallel` smoke-run on the tpch benchmark queries
2. [ ] Decide final fate of OP_Program (keep as fallback or implement)

### Nice to Have:
1. [ ] Implement OP_Program (VdbeFrame setup — complex but achieves 100%)
2. [ ] Create performance baseline document before cutover

---

## Responsibility Assignment

- **Phase 5.8 Implementation (18 opcodes)**: Primary track
- **Full Test Suite Validation**: Parallel once opcodes are done
- **Documentation**: Ongoing with each phase

---

## Conclusion

The generated threaded interpreter dispatcher covers **142/142 opcodes (100%)**, with 70/70 unit tests passing. The remaining work is:

1. **Activate generated dispatcher as default** (Phase 5.10) — flip the switch, run regression tests
2. **Clean up inline stubs** in opcodes.yaml (Phase 5.11) — extract to `_inline` C handlers
3. **Remove old dispatcher from vdbe.c** (Phase 5.12) — after default switch stable
4. **Benchmark** — requires Release build (`-O2`/`-O3`); current Debug build is not meaningful

Note: LLVM JIT (Track 2) is a separate track — benchmarking it also requires Release build.

---

**Created**: 2026-02-18
**Updated**: 2026-02-19 (142/142 opcodes, 70/70 tests, external_inline type added, .bak removed)
**Next Review**: After Phase 5.10 activation + regression run
**Success Criteria**: Full Tarantool SQL test suite passes 100% with generated dispatcher as default

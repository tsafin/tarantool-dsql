# VDBE Refactoring - Next Steps Plan (Feb 18, 2026)

**Created**: February 18, 2026
**Updated**: February 18, 2026 (Phase 5.8 + 5.9 verification complete)
**Coverage**: 141/142 opcodes (99.3%) — only OP_Program remains as intentional fallback
**Status**: Ready for Phase 5.10 (full Tarantool SQL test suite regression run)

---

## Executive Summary

The VDBE refactoring project has achieved **99.3% dispatcher coverage (141/142 opcodes)**.
Phases 5.1–5.8 are complete. Phase 5.9 unit verification passed 45/45 tests across both
dispatchers with identical output. The only remaining fallback opcode is OP_Program (trigger
sub-program execution — intentionally deferred due to complexity).

**Immediate Action**: Phase 5.10 — run the full Tarantool SQL regression test suite with
`VDBE_DISPATCHER=generated` and compare against `VDBE_DISPATCHER=original`.

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

### Phase 5.9: Unit Opcode Verification — ✅ DONE (Feb 18, 2026)

**Result**: 45/45 tests pass, both dispatchers produce **identical output**.

**Test file**: `test_phase58.lua` — covers all 17 newly-added Phase 5.8 opcodes:
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

**Also verified**: Behavioral notes recorded —
- CHECK/FK constraints defined but not enforced at SQL layer (silent insert succeeds)
- ANALYZE returns `nil` result (not an error) — DDL-style statement
- CREATE/DROP SEQUENCE returns `nil` result (same)

---

### Phase 5.10: Full Tarantool SQL Test Suite — ⬜ NEXT
**Duration**: 2-3 days
**Goal**: Verify generated dispatcher passes all existing SQL regression tests

**Tasks**:
1. Run Tarantool SQL test suite with `VDBE_DISPATCHER=original` (baseline)
2. Run same suite with `VDBE_DISPATCHER=generated`
3. Use `VDBE_DISPATCHER=parallel` for any differences
4. Fix any handler bugs found
5. Performance benchmark: target <2% regression

---

### Phase 5.11: Cutover Decision
**Duration**: 1 day
**Goal**: Decide whether to make generated dispatcher default

**Decision criteria**:
- 100% test pass rate with generated dispatcher → proceed to cutover
- <2% performance regression → proceed to cutover
- If any criteria fail → fix and re-test

---

### Phase 5.12: Cleanup (after cutover)
- Remove old inline dispatcher from vdbe.c
- Archive/remove code generator shell scripts
- Finalize documentation

---

## Parallel Investigation Threads

### Thread A: Full Regression Testing
**Priority**: HIGH (validates dispatcher correctness)
**Status**: Unblocked — Phase 5.8 + 5.9 complete, all unit tests pass

**Steps**:
1. Run full SQL test suite with `VDBE_DISPATCHER=original` to collect baseline pass/fail counts
2. Run same suite with `VDBE_DISPATCHER=generated`
3. Run with `VDBE_DISPATCHER=parallel` to get per-opcode diff on any failures
4. For each failure: trace to specific opcode, fix handler, re-test
5. Document pass rates in test report

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
- [ ] 141/142 opcodes handled (done — pending cutover decision)
- [ ] <2% performance regression
- [ ] Full test suite passing
- [ ] All P2-branching patterns verified
- [ ] Documentation complete for future developers

---

## Risk Assessment

### Medium Risk - Monitor
1. **Coroutine opcodes** - OP_Gosub/Return/Yield require PC manipulation through handler return path
   - Mitigation: Design handler to update `pc` struct field; test with triggers/views
   - Impact: Generated dispatcher reliability

2. **DDL opcodes** - OP_RenameTable, OP_LoadAnalysis may require significant refactoring
   - Mitigation: Implement or accept fallback depending on test failures
   - Impact: May remain at 97% rather than 100%

### Low Risk - Expected and Planned
1. **Incremental implementation** - Some opcodes deferred
   - Mitigation: Hybrid fallback handles all opcodes
   - Impact: Fallback overhead for rare opcodes

---

## Timeline Estimate (Updated Feb 18, 2026)

| Phase | Duration | Status | Expected Completion |
|-------|----------|--------|---------------------|
| 5.8 (17 remaining opcodes) | — | ✅ Done | Feb 18 |
| 5.9 (Unit opcode verification) | — | ✅ Done | Feb 18 |
| 5.10 (Full SQL test suite) | 2-3 days | ⬜ Next | Feb 20-21 |
| 5.11 (Cutover Decision) | 1 day | ⬜ | Feb 22 |
| 5.12 (Cleanup) | 1-2 days | ⬜ | Feb 23-24 |
| **Total remaining** | **~4-6 days** | | **~Feb 24** |

**Critical Path**: Full SQL regression suite → fix any failures → cutover decision → cleanup

---

## Long-term Strategy (Phase 5.10+)

### Phase 5.10: Cutover Decision
- Decision: Make generated dispatcher default? Retire old dispatcher?
- Depends on Phase 5.9 results
- If 100% test pass + <2% regression: Go ahead
- If issues remain: Continue with hybrid mode

### Phase 5.11: Cleanup
- Remove old inline dispatcher from vdbe.c
- Delete shell script generators
- Finalize documentation
- Archive code generator outputs

### Phase 5.12+: Optimization
- Profile generated dispatcher
- Optimize hot paths (likely comparison opcodes)
- Consider computed-goto for switch-based handlers
- Add performance benchmarking to test suite

---

## Documentation to Create

### Required
1. PHASE_5_8_SESSION_SUMMARY.md - After implementing remaining 18 opcodes
2. TEST_SUITE_VALIDATION_REPORT.md - After Phase 5.9

### Optional (Reference)
1. PERFORMANCE_PROFILING.md - Benchmark results after cutover
2. HYBRID_DISPATCHER_EVALUATION.md - When ready to decide on Phase 5.10 cutover

---

## Immediate Next Steps

### Must Do (Phase 5.10 — full SQL test suite):
1. [ ] Locate test runner: `test/sql/` directory + `test-run.py`
2. [ ] Run `VDBE_DISPATCHER=original ./test-run.py suite/sql` → capture baseline
3. [ ] Run `VDBE_DISPATCHER=generated ./test-run.py suite/sql` → compare
4. [ ] Fix any failures (use `VDBE_DISPATCHER=parallel` for per-opcode diff)
5. [ ] Run performance benchmark (TPC-H queries or similar)

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

The VDBE refactoring project has reached **87.3% coverage (124/142 opcodes)** with solid architectural foundations. The next 7-10 days should focus on:

1. **Run full Tarantool SQL regression suite** (Phase 5.10) — start immediately
2. **Cutover decision** after validation (Phase 5.11)
3. **Cleanup and optimization** once dispatcher is default (Phase 5.12+)

The project has achieved 99.3% coverage (141/142). The remaining work is validation and
cutover — no more implementation needed unless the full test suite surfaces OP_Program failures.

---

**Created**: 2026-02-18  
**Updated**: 2026-02-18 (Phase 5.8 + Phase 5.9 unit verification complete; 141/142 opcodes)  
**Next Review**: After Phase 5.10 full test suite run (Feb 21)  
**Success Criteria**: Full Tarantool SQL test suite passes 100% with generated dispatcher

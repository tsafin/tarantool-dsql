# Phase 5.6g - Medium Opcode Batch 6 Implementation Plan

## Objective
Continue expanding the generated dispatcher by implementing 5 additional medium-complexity inline opcodes (100-300 chars), completing the straightforward medium-complexity batch and reaching ~55% total dispatcher coverage.

## Current Status (End of Phase 5.6f)

### Achievements
- ✓ 29 inline opcode handlers implemented (46% of 63 inline opcodes)
- ✓ Dispatcher coverage: 92/176 opcodes (52%)
- ✓ Helper pattern proven - zero new helpers required through 5 phases
- ✓ Medium-complexity batches: 5.6b-f completed (23 opcodes done)
- ✓ Remaining medium opcodes: 23 candidates identified and ranked

### Coverage Summary
| Category | Implemented | Total | Coverage |
|----------|------------|-------|----------|
| Simple (< 100 chars) | 6 | 10 | 60% |
| Medium (100-300 chars) | 24 | 47 | 51% |
| Complex (> 300 chars) | 0 | 14 | 0% |
| **Total Inline** | **29** | **63** | **46%** |
| **Total Dispatcher** | **92** | 176 | 52% |

### Phase Progress
```
Phase 5.6a (Simple):    6 opcodes  ✅ DONE
Phase 5.6b (Medium 1):  2 opcodes  ✅ DONE
Phase 5.6c (Medium 2):  4 opcodes  ✅ DONE
Phase 5.6d (Medium 3):  6 opcodes  ✅ DONE
Phase 5.6e (Medium 4):  6 opcodes  ✅ DONE
Phase 5.6f (Medium 5):  5 opcodes  ✅ DONE
Phase 5.6g (Medium 6):  5 opcodes  🔄 READY FOR START
```

## Phase 5.6g Implementation Strategy

### Batch Selection Approach
Phase 5.6g targets the simplest remaining medium-complexity opcodes with minimal dependencies:

1. **Value Loading Operations** - Simple register initialization
2. **Cursor Management** - Space and sequence operations
3. **Data Retrieval** - Field fetch with proven error handling

### Selection Criteria
1. Opcodes with straightforward logic (≤200 lines equivalent code)
2. Opcodes that don't require new helper extraction
3. Opcodes without complex branching or nested control flow
4. Opcodes with clear input/output register patterns
5. Preference for natural operation clustering

### Target Candidates for Batch 6

**Selected 5 opcodes (best candidates):**

1. **OP_Decimal** (114 chars) ⭐ TIER 1
   - Flags: OUT2
   - Logic: Load decimal constant into register
   - Dependencies: vdbe_prepare_null_out(), mem_set_dec()
   - Complexity: VERY LOW (3 lines logic)
   - Pattern: Identical to OP_AddImm from 5.6a, proven safe
   - Risk: NONE

2. **OP_OpenSpace** (135 chars) ⭐ TIER 1
   - Flags: IN3
   - Logic: Create space reference cursor by ID lookup
   - Dependencies: space_by_id()
   - Complexity: VERY LOW (4 lines logic)
   - Pattern: Similar to OP_Clear from 5.6f, cursor setup
   - Risk: VERY LOW

3. **OP_Sequence** (195 chars) ⭐ TIER 1
   - Flags: IN1, OUT2
   - Logic: Get sequence counter value and increment
   - Dependencies: Cursor access, vdbe_prepare_null_out(), mem_set_uint()
   - Complexity: LOW (6 lines logic)
   - Pattern: Proven cursor access pattern from OP_NullRow (5.6e)
   - Risk: VERY LOW

4. **OP_SequenceTest** (150 chars) ⭐ TIER 2
   - Flags: JUMP
   - Logic: Test sequence counter, jump if zero
   - Dependencies: Cursor validation, JUMP_P2() macro
   - Complexity: LOW (5 lines logic)
   - Pattern: Proven conditional jump from Phase 5.6e (OP_IfNot, OP_IfPos)
   - Risk: VERY LOW

5. **OP_Fetch** (209 chars) ⭐ TIER 2
   - Flags: IN1, OUT3
   - Logic: Fetch field value from record
   - Dependencies: vdbe_field_ref_fetch(), vdbe_prepare_null_out()
   - Complexity: LOW (5 lines logic)
   - Pattern: External function wrapper, proven from OP_ShowCreateTable (5.6f)
   - Risk: LOW

## Implementation Plan

### Phase 5.6g Checklist

- [ ] **Analysis Phase** ✓ DONE
  - [x] Identified all 23 remaining medium candidates
  - [x] Ranked by complexity and dependencies
  - [x] Selected top 5 for implementation

- [ ] **Helper Analysis** ✓ DONE
  - [x] Verified all selected opcodes use existing helpers
  - [x] Confirmed zero new helper extraction needed
  - [x] Documented helper reuse patterns

- [ ] **Implementation** (5 opcodes)
  - [ ] Create vdbe_ops_inline_medium_6.c
  - [ ] Implement 5 selected opcodes as handler functions
  - [ ] Add prototypes to vdbe_ops.h
  - [ ] Add dispatcher cases to vdbe_dispatch_wrapper.c
  - [ ] Update CMakeLists.txt

- [ ] **Verification**
  - [ ] Code generation: `cmake --build . --target generate_sql_files`
  - [ ] Syntax validation of all files
  - [ ] Verify prototypes match implementations
  - [ ] Build attempt (may fail on unrelated libunwind, that's OK)

- [ ] **Documentation**
  - [ ] Create PHASE_5_6g_SESSION_SUMMARY.md
  - [ ] Update TODO.md with completion status
  - [ ] Update PHASE_5_6_QUICK_REFERENCE.md
  - [ ] Document any patterns discovered

## Expected Outcomes

### Coverage After Phase 5.6g
- **Inline handlers**: 29 → 34 opcodes
- **Inline coverage**: 46% → 54%
- **Total dispatcher**: 92 → 97 opcodes (55%)
- **Medium opcodes remaining**: 23 → 18 (72% complete of medium batch)

### Key Metrics
- Batch size: 5 opcodes (consistent with Phase 5.6f)
- New helpers expected: 0 (all use existing infrastructure)
- Build validation: Should pass all checks
- Code quality: Same high standards as previous batches
- Average opcode length: 161 chars

### Pattern Discovery Expected
- Value loading patterns (Decimal complements AddImm)
- Cursor management consistency (Space/Sequence operations)
- Sequence counter patterns for position tracking
- Field reference patterns for data retrieval

## Risk Assessment

### Low Risk
- ✅ Established handler pattern from phases 5.6a-f
- ✅ Build system fully validated
- ✅ Dispatcher integration proven
- ✅ All helpers already available (no new extraction needed)
- ✅ All patterns proven in previous batches
- ✅ All opcodes ≤ 224 chars (well within bounds)

### Medium Risk
- ⚠️ Field reference patterns (OP_Fetch) - first time, but simple
- ⚠️ Sequence counter semantics - increment patterns

### High Risk
- None identified at this stage

## Dependencies and Prerequisites

### Already Available
- vdbe_helpers.h with existing helpers
- Build system configured for new handler files
- Dispatcher integration pattern proven
- CMake generator working correctly
- Function prototype pattern established

### May Be Needed
- None expected (all helpers already available)

## Next Phases Overview

### Phase 5.6h: Batch 7 (Optional)
- Target: Next batch of 4-6 medium opcodes
- Remaining: 18 medium-complexity opcodes
- Examples: OP_FCopy, OP_Getitem, OP_SorterInsert
- Expected coverage: 54% → 57-59%

### Phase 5.7: Complex Opcode Handlers
- Target: 14 complex opcodes (>300 chars)
- Examples: OP_Program (1000+ chars), OP_RenameTable (500+ chars)
- Strategy: May require different approach (refactoring)
- Coverage goal: 60%+

### Phase 5.8: Test Suite Validation
- Run full test suite with generated dispatcher
- Parallel validation mode (VDBE_DISPATCHER=parallel)
- Performance profiling and regression testing

## Success Criteria

Phase 5.6g is successful when:
1. ✓ 5 medium opcodes implemented as handlers
2. ✓ All handlers pass syntax validation
3. ✓ Dispatcher integration complete
4. ✓ Build system integration verified
5. ✓ Session summary and documentation created
6. ✓ TODO.md updated with progress
7. ✓ Coverage reaches 55% (97 opcodes)
8. ✓ Zero compilation errors in handler code
9. ✓ Zero new helpers required

## References
- [Phase 5.6f Session Summary](PHASE_5_6f_SESSION_SUMMARY.md) - Previous batch (5 opcodes)
- [Phase 5.6e Session Summary](PHASE_5_6e_SESSION_SUMMARY.md) - Batch 4 reference
- [Phase 5.6d Session Summary](PHASE_5_6d_SESSION_SUMMARY.md) - Batch 3 reference
- [Phase 5.6 Strategy](PHASE_5_6_INLINE_CODE_STRATEGY.md) - Overall approach
- [TODO.md](TODO.md) - Master project status
- [vdbe_helpers.h](src/box/sql/vdbe_helpers.h) - Available helpers
- [vdbe_ops_inline_medium_5.c](src/box/sql/vdbe_ops_inline_medium_5.c) - Reference implementation

## Execution Plan

### Timeline (No Estimates)
1. **Analysis**: ✓ DONE - Identified and selected 5 best candidates
2. **Implementation**: Extract handlers following established pattern
3. **Verification**: Build and syntax validation
4. **Documentation**: Record decisions and lessons learned

### Key Success Factors
- Maintain consistent handler quality from previous batches
- Document any new patterns discovered
- Keep opcodes well-scoped and independent
- Validate build system remains stable

## Notes

### Why These 5 Opcodes?
- **OP_Decimal**: Simplest value loading (matches AddImm pattern)
- **OP_OpenSpace**: Straightforward cursor setup (matches Clear pattern)
- **OP_Sequence**: Cursor counter operation (matches NullRow pattern)
- **OP_SequenceTest**: Conditional jump (matches IfNot pattern)
- **OP_Fetch**: Field data retrieval (matches ShowCreateTable pattern)

All share:
- Minimal branching (0-1 conditional paths)
- No complex loops or nested control flow
- Existing helper dependencies only
- Clear success/error returns
- Natural operation clustering

### Scaling Consideration
After Phase 5.6g, we'll expand from 29 (46%) to 34 (54%) inline handlers. With 55% total dispatcher coverage, we're approaching the halfway point. The 23 remaining medium opcodes will be tackled in subsequent batches (5.6h and potentially 5.6i).

### Helper Reuse Pattern Validation
Phase 5.6g validates the comprehensive helper infrastructure:
- Phase 5.6c: 50% helper:opcode (2 helpers, 4 opcodes)
- Phase 5.6d: 0% helper:opcode (0 helpers, 6 opcodes)
- Phase 5.6e: 0% helper:opcode (0 helpers, 6 opcodes)
- Phase 5.6f: 0% helper:opcode (0 helpers, 5 opcodes)
- Phase 5.6g: Expected 0% (0 helpers, 5 opcodes)

Total pattern: 2 helpers / 26 opcodes = 7.7% helper:opcode ratio

## Conclusion

Phase 5.6g builds on the proven momentum and consistent patterns from phases 5.6a-f. With systematic selection of the simplest remaining medium-complexity opcodes and comprehensive analysis showing zero new helper requirements, this batch represents continued efficient progress toward 55%+ coverage.

The natural clustering of these opcodes (value loading, cursor management, data retrieval) demonstrates mature understanding of the codebase and effective batch selection strategy. All patterns have been proven in previous phases, making this a low-risk, high-confidence implementation batch.

---

**Next Phase**: Phase 5.6g - Medium Batch 6 Implementation

**Planning Status**: Ready for implementation

**Last Updated**: 2025-12-20 (Starting Phase 5.6g)

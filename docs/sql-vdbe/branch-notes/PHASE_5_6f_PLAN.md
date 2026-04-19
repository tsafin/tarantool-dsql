# Phase 5.6f - Medium Opcode Batch 5 Implementation Plan

## Objective
Continue expanding the generated dispatcher by implementing 4-6 additional medium-complexity inline opcodes (100-300 chars), building on the success of Phase 5.6e.

## Current Status (End of Phase 5.6e)

### Achievements
- ✓ 24 inline opcode handlers implemented (38% of 63 inline opcodes)
- ✓ Dispatcher coverage: 87/176 opcodes (49%)
- ✓ Helper pattern proven without requiring new helpers in batch 4
- ✓ Control flow opcodes cluster well for efficiency
- ✓ Cursor operations remain straightforward

### Coverage Summary
| Category | Implemented | Total | Coverage |
|----------|------------|-------|----------|
| Simple (< 100 chars) | 6 | 10 | 60% |
| Medium (100-300 chars) | 18 | 39 | 46% |
| Complex (> 300 chars) | 0 | 14 | 0% |
| **Total Inline** | **24** | **63** | **38%** |
| **Total Dispatcher** | ~87 | 176 | 49% |

### Remaining Medium Opcodes
- Batches completed: 4 (24 opcodes)
- Remaining: ~19 medium-complexity opcodes
- Planned batches: 5.6f (batch 5), 5.6g (batch 6)

## Phase 5.6f Implementation Strategy

### Batch Selection Approach
Phase 5.6f targets the simplest and most straightforward of the remaining 19 opcodes:

1. **Type/Value Operations** - Simple conversions with minimal logic
2. **Space Management** - Direct state manipulation without complex branching
3. **Sorting Operations** - Cursor validation with simple control flow

### Selection Criteria
1. Opcodes with straightforward logic (≤200 lines of equivalent code)
2. Opcodes that don't require new helper extraction
3. Opcodes without complex goto chains or fallthrough logic
4. Opcodes with clear input/output register patterns
5. Preference for opcodes with high code reuse potential

### Target Candidates for Batch 5

**Selected 5 opcodes (best candidates):**

1. **OP_ShowCreateTable** (137 chars)
   - Flags: IN1, OUT2
   - Logic: Call sql_show_create_table() with register setup
   - Dependencies: None new (external function)
   - Complexity: Low

2. **OP_ResetSorter** (150 chars)
   - Flags: IN1
   - Logic: Cursor validation and conditional sorter reset
   - Dependencies: isSorter(), sqlVdbeSorterReset()
   - Complexity: Low

3. **OP_Sort** (169 chars)
   - Flags: IN1, JUMP
   - Logic: Test harness code with fallthrough to OP_Rewind
   - Dependencies: None new
   - Complexity: Low (mostly test code)

4. **OP_Clear** (188 chars)
   - Flags: IN1
   - Logic: Space validation with optional truncate
   - Dependencies: space_by_id(), box_truncate()
   - Complexity: Low

5. **OP_Param** (203 chars)
   - Flags: OUT2
   - Logic: Frame-based parameter copy with register setup
   - Dependencies: vdbe_prepare_null_out(), mem_copy_as_ephemeral()
   - Complexity: Low

## Implementation Plan

### Phase 5.6f Checklist

- [ ] **Analysis Phase** ✓ DONE
  - [x] Identified all 19 remaining candidates
  - [x] Categorized by complexity and dependencies
  - [x] Selected top 5 for implementation

- [ ] **Helper Analysis** (if needed)
  - [ ] Verify all selected opcodes use existing helpers
  - [ ] Confirm no new helper extraction needed
  - [ ] Document helper reuse patterns

- [ ] **Implementation** (5 opcodes)
  - [ ] Create vdbe_ops_inline_medium_5.c
  - [ ] Implement selected opcodes as handler functions
  - [ ] Add prototypes to vdbe_ops.h
  - [ ] Add dispatcher cases to vdbe_dispatch_wrapper.c
  - [ ] Update CMakeLists.txt

- [ ] **Verification**
  - [ ] Code generation: `cmake --build . --target generate_sql_files`
  - [ ] Syntax validation of all files
  - [ ] Verify prototypes match implementations
  - [ ] Build attempt (may fail on unrelated libunwind, that's OK)

- [ ] **Documentation**
  - [ ] Create PHASE_5_6f_SESSION_SUMMARY.md
  - [ ] Update TODO.md with completion status
  - [ ] Update PHASE_5_6_QUICK_REFERENCE.md
  - [ ] Document any patterns discovered

## Expected Outcomes

### Coverage After Phase 5.6f
- **Inline handlers**: 24 → 29 opcodes
- **Inline coverage**: 38% → 46%
- **Total dispatcher**: 87 → 92 opcodes (52%)

### Key Metrics
- Batch size: 5 opcodes (matching Phase 5.6e)
- New helpers expected: 0 (all use existing infrastructure)
- Build validation: Should pass all checks
- Code quality: Same high standards as previous batches

### Pattern Discovery Expected
- Type conversion/value manipulation patterns
- Space management operation groupings
- Sorting operation efficiency patterns

## Risk Assessment

### Low Risk
- Established handler pattern from phases 5.6a-e
- Build system fully validated
- Dispatcher integration proven
- All helpers already available
- No new dependencies required

### Medium Risk
- OP_Sort test code may have subtle semantics
- Frame-based parameter copying (OP_Param) complexity
- Space truncation error handling (OP_Clear)

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

### Phase 5.6g: Batch 6
- Target: Final 14 medium opcodes
- Expected: Data operations, function calls, advanced operations
- Strategy: Same incremental approach as batches 4-5

### Phase 5.7: Complex Opcode Handlers
- Target: 14 complex opcodes (>300 chars)
- Examples: OP_Program (1000+ chars), OP_RenameTable
- Strategy: May require different approach

### Phase 5.8: Test Suite Validation
- Run full test suite with generated dispatcher
- Parallel validation mode (VDBE_DISPATCHER=parallel)
- Performance profiling and regression testing

## Success Criteria

Phase 5.6f is successful when:
1. ✓ 5 medium opcodes implemented as handlers
2. ✓ All handlers pass syntax validation
3. ✓ Dispatcher integration complete
4. ✓ Build system integration verified
5. ✓ Session summary and documentation created
6. ✓ TODO.md updated with progress
7. ✓ Coverage reaches 52% (92 opcodes)
8. ✓ Zero compilation errors in handler code
9. ✓ Zero new helpers required

## References
- [Phase 5.6e Session Summary](PHASE_5_6e_SESSION_SUMMARY.md) - Completed batch 4
- [Phase 5.6d Session Summary](PHASE_5_6d_SESSION_SUMMARY.md) - Reference batch 3
- [Phase 5.6c Session Summary](PHASE_5_6c_SESSION_SUMMARY.md) - Helper extraction pattern
- [Phase 5.6 Strategy](PHASE_5_6_INLINE_CODE_STRATEGY.md) - Overall approach
- [TODO.md](TODO.md) - Master project status
- [vdbe_helpers.h](src/box/sql/vdbe_helpers.h) - Available helpers
- [vdbe_ops_inline_medium_4.c](src/box/sql/vdbe_ops_inline_medium_4.c) - Reference implementation

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
- **OP_ShowCreateTable**: Simple external function call (123 chars)
- **OP_ResetSorter**: Straightforward cursor manipulation (150 chars)
- **OP_Sort**: Test harness with fallthrough (169 chars)
- **OP_Clear**: Space operation with optional action (188 chars)
- **OP_Param**: Frame-based parameter handling (203 chars)

All share:
- Minimal branching (1-2 conditional paths)
- No complex loops or nested control flow
- Existing helper dependencies only
- Clear success/error returns

### Scaling Consideration
After Phase 5.6f, we'll have expanded from 24 (38%) to 29 (46%) inline handlers. With 52%+ total dispatcher coverage, we're approaching the halfway point for full implementation.

### Helper Reuse Pattern
Phase 5.6f validates that the helper infrastructure is comprehensive:
- Phase 5.6c: 50% helper:opcode (2 helpers, 4 opcodes)
- Phase 5.6d: 0% helper:opcode (0 helpers, 6 opcodes)
- Phase 5.6e: 0% helper:opcode (0 helpers, 6 opcodes)
- Phase 5.6f: Expected 0% (0 helpers, 5 opcodes)

## Conclusion

Phase 5.6f builds on the momentum and proven patterns from phases 5.6a-e to continue expanding inline opcode coverage. With 49% dispatcher coverage already achieved and the helper infrastructure fully matured, we're in excellent position to reach 50%+ coverage in this phase.

The selection of these 5 opcodes reflects a focus on operational simplicity and maximum code reuse, positioning the project well for the remaining medium opcodes and eventual complex opcode handling.

---

**Next Phase**: Phase 5.6f - Medium Batch 5 Implementation

**Planning Status**: Ready for implementation

**Last Updated**: 2025-12-20 (Starting Phase 5.6f)

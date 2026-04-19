# Phase 5.6e - Medium Opcode Batch 4 Implementation Plan

## Objective
Continue expanding the generated dispatcher by implementing 4-6 additional medium-complexity inline opcodes (100-300 chars), leveraging lessons learned from Phase 5.6d.

## Current Status (End of Phase 5.6d)

### Achievements
- ✓ 18 inline opcode handlers implemented (29% of 63 inline opcodes)
- ✓ Dispatcher coverage: 81/176 opcodes (46%)
- ✓ Helper pattern proven without requiring new helpers in batch 3
- ✓ Build system validated for rapid handler addition
- ✓ Zero compilation issues with handler integration

### Coverage Summary
| Category | Implemented | Total | Coverage |
|----------|------------|-------|----------|
| Simple (< 100 chars) | 6 | 10 | 60% |
| Medium (100-300 chars) | 12 | 39 | 31% |
| Complex (> 300 chars) | 0 | 14 | 0% |
| **Total Inline** | **18** | **63** | **29%** |
| **Total Dispatcher** | ~81 | 176 | 46% |

### Remaining Medium Opcodes
- Batch 3 implemented: 6 opcodes
- Remaining: ~25 medium-complexity opcodes
- Planned batches: 5.6e (batch 4), 5.6f+ (batches 5-6)

## Phase 5.6e Implementation Strategy

### Batch Selection Approach
Unlike Phase 5.6d which had constraint operations naturally grouping, Phase 5.6e should target:
1. **Register/memory manipulation opcodes** (OP_RegisterPush, OP_RegisterPop, etc.)
2. **Cursor operations** (OP_OpenRead, OP_OpenWrite variants)
3. **Simple numeric operations** (OP_Increment, OP_Decrement)
4. **Result formatting opcodes** (OP_ArrayAppend, similar)

### Selection Criteria
1. Opcodes with straightforward logic (≤150 lines of equivalent code)
2. Opcodes that don't require multiple new helpers
3. Opcodes without complex goto chains or fallthrough logic
4. Opcodes with clear input/output register patterns

### Target Candidates for Batch 4

**To be identified through analysis of:**
- opcodes.yaml inline_code sections
- vdbe.c case statements for medium-length implementations
- Dependency analysis on box/*, tarantool APIs, and internal helpers

**Estimated categories:**
- 3-4 opcodes with existing helper dependencies
- 2-3 opcodes requiring 1 simple new helper function
- 0 opcodes with external (box/tarantool) API dependencies beyond previous batches

## Implementation Plan

### Phase 5.6e Checklist

- [ ] **Analysis Phase**
  - [ ] Identify 6-8 candidate opcodes from remaining 25
  - [ ] Categorize by helper dependencies
  - [ ] Create dependency impact analysis
  - [ ] Flag any that require helper extraction

- [ ] **Helper Extraction** (if needed)
  - [ ] Identify any new helper functions
  - [ ] Extract to vdbe_helpers.h following Phase 5.6c pattern
  - [ ] Update vdbeInt.h include if needed
  - [ ] Document in helper inventory

- [ ] **Implementation** (4-6 opcodes)
  - [ ] Create vdbe_ops_inline_medium_4.c
  - [ ] Implement selected opcodes as handler functions
  - [ ] Add prototypes to vdbe_ops.h
  - [ ] Add dispatcher cases to vdbe_dispatch_wrapper.c
  - [ ] Update CMakeLists.txt

- [ ] **Verification**
  - [ ] Code generation: `cmake --build . --target generate_sql_files`
  - [ ] Syntax validation of all files
  - [ ] Verify prototypes match implementations
  - [ ] Build attempt (may fail on unrelated libunwind, that's OK)
  - [ ] Run tests if possible

- [ ] **Documentation**
  - [ ] Create PHASE_5_6e_SESSION_SUMMARY.md
  - [ ] Update TODO.md with completion status
  - [ ] Update progress metrics
  - [ ] Document any new patterns discovered

## Expected Outcomes

### Coverage After Phase 5.6e
- **Inline handlers**: 18 → 22-24 opcodes
- **Inline coverage**: 29% → 35-38%
- **Total dispatcher**: 81 → 85-87 opcodes (48-49%)

### Key Metrics
- Batch size: 4-6 opcodes (matching historical pattern)
- New helpers expected: 0-2 (learning from efficient batch 3)
- Build validation: Should pass all checks
- Code quality: Same high standards as previous batches

### Pattern Discovery Expected
- Identify common patterns in register manipulation
- Discover cursor operation groupings
- Possible need for memory allocation helpers

## Risk Assessment

### Low Risk
- Established handler pattern from phases 5.6a-d
- Build system fully validated
- Dispatcher integration proven
- Syntax checking automated

### Medium Risk
- May identify new helpers requiring extraction
- Possible discovery of more complex opcodes masquerading as medium
- Edge cases in register/cursor handling

### High Risk
- None identified at this stage
- Phase 5.6d success with zero new helpers greatly reduces risk

## Dependencies and Prerequisites

### Already Available
- vdbe_helpers.h with 2 helpers (sqlVdbeMemAboutToChange, vdbe_prepare_null_out)
- Build system configured for new handler files
- Dispatcher integration pattern proven
- CMake generator working correctly
- Function prototype pattern established

### May Be Needed
- Additional memory management helpers (allocateCursor, etc.)
- Register access utilities
- Cursor validation helpers

## Next Phases Overview

### Phase 5.6f: Batch 5
- Target: 4-6 more medium opcodes
- Expected: Complex register operations, advanced cursor management
- Strategy: Same incremental approach as batch 4

### Phase 5.6g: Batch 6
- Target: Final 10-15 medium opcodes
- Expected: Complex memory operations, special-case handlers
- May identify patterns for complex opcodes

### Phase 5.7: Complex Opcode Handlers
- Target: 14 complex opcodes (>300 chars)
- Examples: OP_Program (1000+ chars), OP_RenameTable
- Strategy: Likely requires different approach (refactoring vs. extraction)

### Phase 5.8: Test Suite Validation
- Run full test suite with generated dispatcher
- Parallel validation mode (VDBE_DISPATCHER=parallel)
- Performance profiling and regression testing

## Success Criteria

Phase 5.6e is successful when:
1. ✓ 4-6 medium opcodes implemented as handlers
2. ✓ All handlers pass syntax validation
3. ✓ Dispatcher integration complete
4. ✓ Build system integration verified
5. ✓ Session summary and documentation created
6. ✓ TODO.md updated with progress
7. ✓ Coverage reaches 48-49% (85-87 opcodes)
8. ✓ Zero compilation errors in handler code

## References
- [Phase 5.6d Session Summary](PHASE_5_6d_SESSION_SUMMARY.md) - Completed batch 3
- [Phase 5.6c Session Summary](PHASE_5_6c_SESSION_SUMMARY.md) - Helper extraction pattern
- [Phase 5.6 Strategy](PHASE_5_6_INLINE_CODE_STRATEGY.md) - Overall approach
- [TODO.md](TODO.md) - Master project status
- [vdbe_helpers.h](src/box/sql/vdbe_helpers.h) - Available helpers
- [vdbe_ops_inline_medium_3.c](src/box/sql/vdbe_ops_inline_medium_3.c) - Reference implementation

## Execution Plan

### Timeline (No Estimates)
1. **Analysis**: Understand remaining 25 opcodes, select best 6-8 candidates
2. **Selection**: Pick 4-6 with minimal dependencies
3. **Helper Extraction**: If needed, extract 1-2 helpers to vdbe_helpers.h
4. **Implementation**: Create handlers following established pattern
5. **Verification**: Build and syntax validation
6. **Documentation**: Record decisions and lessons learned

### Key Success Factors
- Maintain consistent handler quality from previous batches
- Document any new patterns discovered
- Keep batch size manageable (4-6 opcodes)
- Validate build system remains stable

## Notes

### Why 4-6 Opcodes Per Batch?
- Balances progress with quality
- Allows time for thorough testing
- Keeps commits focused and reviewable
- Reduces risk of introducing bugs

### Scaling Consideration
After Phase 5.6g, we'll have expanded from 12 (19%) to potentially 30+ (48%+) inline handlers. This will significantly reduce the workload for phases 5.6h and beyond, as the helper infrastructure becomes more comprehensive.

### Helper Reusability
Helpers extracted in earlier phases become increasingly valuable as more opcodes are implemented. Phase 5.6d proved this by needing zero new helpers. Phase 5.6e may identify 1-2 new ones that subsequent batches will also benefit from.

## Conclusion

Phase 5.6e builds on the momentum and proven patterns from phases 5.6a-d to continue expanding inline opcode coverage. With 46% dispatcher coverage already achieved and a clear roadmap for remaining batches, we're on track for 50%+ coverage by phase 5.6g.

The constraint drop operations from batch 3 validated that similar operations naturally group together. Batch 4 should explore other groupings (register operations, cursor operations, memory operations) to continue improving handler organization and code quality.

---

**Next Phase**: Phase 5.6e - Medium Batch 4 Implementation

**Planning Status**: Ready for implementation

**Last Updated**: 2025-12-20 (Post Phase 5.6d)

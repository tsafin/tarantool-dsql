# Phase 5.6d - Medium Opcode Batch 3+ Implementation Plan

## Objective
Continue expanding the generated dispatcher by implementing the remaining 31 medium-complexity inline opcodes (100-300 chars), using the helper extraction pattern established in Phase 5.6c.

## Current Status (End of Phase 5.6c)

### What Works
- ✓ Helper function extraction pattern proven and documented
- ✓ 12 inline opcode handlers integrated (19% of 63 inline opcodes)
- ✓ vdbe_helpers.h provides reusable shared utilities
- ✓ Build system and dispatcher integration complete
- ✓ Four opcodes with different dependency types successfully implemented

### What's Ready
- vdbe_helpers.h with two helpers (sqlVdbeMemAboutToChange, vdbe_prepare_null_out)
- Build system configured to handle new handler files
- Dispatcher integration pattern proven (simple case statements)
- Documentation of implementation approach for future phases

### Coverage Summary
| Category | Implemented | Total | Coverage |
|----------|-------------|-------|----------|
| Simple (< 100 chars) | 6 | 10 | 60% |
| Medium (100-300 chars) | 12 | 39 | 31% |
| Complex (> 300 chars) | 0 | 14 | 0% |
| **Total Inline** | **12** | **63** | **19%** |
| **Total Dispatcher** | ~75 | 176 | 42.6% |

## Implementation Strategy

### Batch Sequencing
Instead of implementing all 31 opcodes in one massive phase, break into 4-5 smaller batches:
- **Phase 5.6d**: Batch 3 - 4-6 opcodes with identified helpers
- **Phase 5.6e**: Batch 4 - 4-6 opcodes with new helpers
- **Phase 5.6f**: Batch 5 - 4-6 opcodes with new helpers
- **Phase 5.6g**: Batch 6 - Remaining opcodes
- **Phase 5.6h**: Final batch if needed

### Batch Selection Criteria
1. Opcodes using existing helpers (vdbe_helpers.h)
2. Opcodes with only one new required helper
3. Opcodes with no external dependencies (box/*, tarantoolInt.h, etc.)
4. Opcodes with straightforward logic (no complex branching)

## Phase 5.6d: Medium Batch 3 Plan

### Identified Medium Opcodes (Remaining 31)

#### Dependency Analysis
The 31 remaining medium opcodes require:
- **8 opcodes**: Need helpers already extracted
- **12 opcodes**: Need simple cursor/register access helpers
- **6 opcodes**: Need memory allocation/manipulation helpers
- **5 opcodes**: Deferred (complex or external dependencies)

### Batch 3 Target Opcodes

Based on complexity analysis, Phase 5.6d should target opcodes that either:
1. Use existing helpers (memAboutToChange, vdbe_prepare_null_out)
2. Require only one simple new helper
3. Have no external box/* dependencies

**Candidates for Phase 5.6d**:

1. **OP_Integer** (from Phase 1) - Already implemented, reference for pattern
2. **OP_String** (from Phase 1) - Already implemented
3. **OP_Real** - Load floating-point constant (similar to OP_Decimal)
4. **OP_Null** - Load NULL to register (similar to OP_Decimal)
5. **OP_Int64** - Load 64-bit integer (similar to OP_Integer)
6. **OP_Bool** - Load boolean value

#### Quick Analysis
- **OP_Real**: ~100 chars, simple register assignment, no dependencies
- **OP_Null**: ~50 chars, trivial register initialization
- **OP_Int64**: ~80 chars, simple register assignment
- **OP_Bool**: ~60 chars, simple register assignment

**Note**: Some of these may already be extracted. Check opcodes.yaml before implementation.

### Phase 5.6d Implementation Checklist

- [ ] **Analysis Phase**
  - [ ] Review all 31 remaining medium opcodes in opcodes.yaml
  - [ ] Categorize by dependencies
  - [ ] Identify helpers already extracted vs. new required helpers
  - [ ] Create dependency graph

- [ ] **Helper Extraction** (if needed)
  - [ ] Identify most commonly needed helper(s)
  - [ ] Extract to vdbe_helpers.h following Phase 5.6c pattern
  - [ ] Update vdbeInt.h include if new header added
  - [ ] Verify no circular dependencies

- [ ] **Implementation** (4-6 opcodes)
  - [ ] Create vdbe_ops_inline_medium_3.c
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
  - [ ] Create PHASE_5_6d_SESSION_SUMMARY.md
  - [ ] Update TODO.md with completion status
  - [ ] Update progress metrics

## Expected Batch 3 Outcomes

### Coverage After Phase 5.6d
- **Inline handlers**: 12 → 16-18 opcodes
- **Inline coverage**: 19% → 25-29%
- **Total dispatcher**: ~75 → 79-81 opcodes (45-46%)

### Pattern Validation
- Confirms helper extraction pattern works for multiple batches
- Demonstrates scalability to remaining opcodes
- Provides foundation for batches 4-6

### Lessons for Future Batches
- New helpers identified and extracted
- Dispatcher integration pattern refined
- Build system integration validated

## Helper Functions Likely Needed

### Already Available
- `sqlVdbeMemAboutToChange()` - Register modification tracking
- `vdbe_prepare_null_out()` - Output register initialization
- `mem_set_null()`, `mem_set_int()`, `mem_set_uint()` - Memory operations
- `mem_set_dec()`, `mem_set_ptr()` - Decimal and pointer storage

### Potentially Needed
- `allocateCursor()` - Cursor allocation for cursor-related opcodes
- `mem_copy_*()` - Memory copying operations
- `sqlite3_value_*()` - Value extraction helpers
- Register access macros from vdbe.c

## Risk Assessment

### Low Risk
- Using proven helper extraction pattern from Phase 5.6c
- Simple opcodes (constant loading) with minimal logic
- No external dependencies for batch 3
- Build system already tested

### Medium Risk
- May identify new helpers requiring extraction
- Potential for register access pattern variations
- Build environment issues (libunwind unrelated)

### High Risk
- None identified at this stage

## Next Phases Overview

### Phase 5.6e: Batch 4
- Target: 4-6 more medium opcodes
- Expected: Register manipulation, cursor operations
- New helpers: Likely allocateCursor(), cursor field accessors

### Phase 5.6f: Batch 5
- Target: 4-6 more medium opcodes
- Expected: Memory operations, error handling variants
- New helpers: mem_copy_*() variants

### Phase 5.6g: Batch 6
- Target: Final medium opcodes
- Expected: Complex opcodes with conditional logic
- New helpers: Specialized memory/cursor operations

### Phase 5.7: Complex Opcodes
- Target: 14 complex opcodes (>300 chars)
- Examples: OP_Program (1000+ chars), OP_RenameTable
- Strategy: May require different approach (refactoring vs. extraction)

### Phase 5.8: Test Suite Validation
- Run complete test suite with generated dispatcher
- Parallel validation mode for comprehensive testing
- Performance profiling

## Timeline Expectations (No Estimates)

This phase should proceed incrementally:
1. **Analysis**: Understand dependency graph for all 31 opcodes
2. **Selection**: Pick 4-6 with minimal new dependencies
3. **Implementation**: Create handlers following established pattern
4. **Verification**: Build system and syntax checks
5. **Documentation**: Record decisions and lessons learned

Each batch becomes simpler as helpers accumulate.

## References
- [Phase 5.6c Session Summary](PHASE_5_6c_SESSION_SUMMARY.md) - Helper extraction pattern
- [Phase 5.6c Plan](PHASE_5_6c_PLAN.md) - Original helper extraction strategy
- [Phase 5.6 Strategy](PHASE_5_6_INLINE_CODE_STRATEGY.md) - Overall approach
- [TODO.md](TODO.md) - Master project status
- [vdbe_helpers.h](src/box/sql/vdbe_helpers.h) - Available helpers
- [vdbe_ops_inline_medium_2.c](src/box/sql/vdbe_ops_inline_medium_2.c) - Reference implementation

## Conclusion

Phase 5.6d builds on the proven helper extraction pattern from Phase 5.6c to continue expanding inline opcode coverage. With the helper pattern established and documented, expanding from 12 to 16-18 opcodes should follow the same proven approach.

The goal is to demonstrate the pattern's scalability and maintain momentum toward full dispatcher coverage.

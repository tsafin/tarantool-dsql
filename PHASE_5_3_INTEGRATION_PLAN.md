# Phase 5.3: Parallel Dispatch Validation - Integration Plan

## Overview

Phase 5.3 aims to validate the generated dispatcher by running both the old inline dispatcher and the newly generated dispatcher in parallel, comparing their results and validating performance.

## Current Status

### ✓ Completed Infrastructure
1. **Dispatcher selection framework** (`vdbe_dispatch.h`)
   - `VDBE_USE_GENERATED_DISPATCH` flag for compile-time selection
   - `VDBE_PARALLEL_VALIDATION` flag for parallel execution mode
   - `VDBE_VALIDATION_STATS` flag for statistics collection

2. **Validation infrastructure** (`vdbe_dispatch_validate.c`)
   - Statistics collection functions
   - Mismatch logging to `/tmp/vdbe_validation.log`
   - State consistency validation functions

3. **Generated dispatcher** (`vdbe_dispatch_generated.c`)
   - Complete dispatch table for all 176 opcodes
   - Both computed-goto and switch-statement implementations
   - Integration of 63 inline opcode implementations
   - Calls to 60+ extracted handler functions

4. **Dispatcher interface** (`vdbe_dispatch_interface.h`)
   - Common interface for dispatcher implementations
   - Type definitions for dispatcher functions
   - Selection logic for runtime dispatcher choice

5. **Dispatcher wrapper** (`vdbe_dispatch_wrapper.c`)
   - Placeholder implementations for dispatcher functions
   - Infrastructure for future dispatcher extraction

### Architecture: Wrapper Function Approach (Option A)

The integration uses **Option A: Wrapper Functions** for several advantages:

1. **Lowest risk** - Minimal changes to working code
2. **Modular** - Each dispatcher in separate compilation unit
3. **Testable** - Can run both and compare results
4. **Evolutionary** - Can be enhanced to other options later

## Integration Architecture

```
sqlVdbeExec() (vdbe.c)
    |
    +-- Initialization code
    |
    +-- Dispatcher selection
    |   |
    |   +-- VDBE_USE_GENERATED_DISPATCH ON?
    |   |   +-- YES: Use generated dispatcher loop (inline in vdbe.c)
    |   |   +-- NO: Use old dispatcher loop (inline in vdbe.c)
    |
    +-- Execution loop
    |   |
    |   +-- Old dispatcher path: Current implementation
    |   +-- Generated dispatcher path: Generated code execution
    |
    +-- Cleanup and return
```

## Implementation Phases

### Phase 5.3.1: Interface and Wrapper Setup (CURRENT)
**Status**: ✓ DONE

- [x] Create `vdbe_dispatch_interface.h` with dispatcher function types
- [x] Create `vdbe_dispatch_wrapper.c` with wrapper function stubs
- [x] Add wrapper to build system (CMakeLists.txt)
- [x] Create this integration plan document

### Phase 5.3.2: Dispatcher Extraction (NEXT)
**Status**: PENDING

- [ ] Extract old dispatcher loop from vdbe.c into separate function
- [ ] Move to `vdbe_dispatch_old.c` with proper wrapping
- [ ] Verify old dispatcher still works correctly
- [ ] Test with existing test suite

**Details**:
- Keep initialization and cleanup in `sqlVdbeExec()`
- Move only the main dispatch loop to separate function
- Dispatcher function takes: `Vdbe *p, VdbeOp *aOp, Mem *aMem`
- Dispatcher function returns: status code

### Phase 5.3.3: Generated Dispatcher Integration
**Status**: PENDING

- [ ] Wrap generated dispatcher loop in callable function
- [ ] Handle dispatcher state and local variables properly
- [ ] Test generated dispatcher standalone
- [ ] Verify all opcode handlers call correctly

### Phase 5.3.4: Parallel Validation Testing
**Status**: PENDING

- [ ] Enable VDBE_PARALLEL_VALIDATION mode
- [ ] Run test suite with both dispatchers
- [ ] Collect validation statistics
- [ ] Compare results and performance
- [ ] Verify <2% performance regression
- [ ] Test both computed-goto and switch modes

### Phase 5.3.5: Single Dispatcher Selection
**Status**: PENDING

- [ ] Enable VDBE_USE_GENERATED_DISPATCH flag
- [ ] Switch dispatcher selection at runtime or compile-time
- [ ] Run full test suite
- [ ] Verify all tests pass
- [ ] Document performance characteristics

## Technical Challenges and Solutions

### Challenge 1: Local Variables in Dispatcher Loop
**Problem**: Current dispatcher uses local variables (`pOp`, `aMem`, `pOrigOp`, etc.) defined in `sqlVdbeExec()`

**Solution**:
- Pass required state as parameters to dispatcher function
- Or, store state in `struct Vdbe` for dispatcher access
- Or, keep initialization in `sqlVdbeExec()` and only extract main loop

**Chosen Approach**: Keep initialization in `sqlVdbeExec()`, extract only main dispatch loop

### Challenge 2: Control Flow and Error Handling
**Problem**: Dispatcher uses `goto abort_due_to_error` and `goto vdbe_return` labels defined in `sqlVdbeExec()`

**Solution**:
- Return status code from dispatcher function
- Handle error cases in `sqlVdbeExec()` after dispatcher returns
- Keep cleanup code in main function

### Challenge 3: Preprocessor Macro Expansion
**Problem**: Generated code uses macros (DISPATCH, JUMP_P2) that must expand correctly in different contexts

**Solution**:
- Ensure macro definitions are available to both dispatcher paths
- Use consistent macro implementations for both old and new dispatchers
- Validate macro expansion in generated code

### Challenge 4: Debug and Trace Support
**Problem**: Current code has SQL_DEBUG branches for tracing that need to work in extracted dispatchers

**Solution**:
- Keep debug macros compatible with extracted code
- Both dispatchers must support same debug/trace modes
- Validation infrastructure helps detect debug-related issues

## File Organization

```
src/box/sql/
├── vdbe.c                              # Main execution loop (sqlVdbeExec)
│   ├── sqlVdbeExec() initialization
│   ├── Dispatcher selection logic       # NEW: Choose old or generated
│   ├── [OLD dispatcher loop - PHASE 5.3.2: extract to vdbe_dispatch_old.c]
│   └── [Cleanup code]
│
├── vdbe_dispatch_interface.h            # ✓ NEW: Dispatcher interface
├── vdbe_dispatch_wrapper.c              # ✓ NEW: Wrapper implementations
├── vdbe_dispatch.h                      # ✓ Dispatcher selection framework
├── vdbe_dispatch_validate.c             # ✓ Validation infrastructure
│
├── generated/vdbe_dispatch_generated.c  # Generated dispatcher (Phase 5.3.3)
│
├── [vdbe_dispatch_old.c]                # FUTURE: Extracted old dispatcher
│
└── vdbe_dispatch.h                      # Dispatcher configuration
```

## Success Criteria

### Phase 5.3.1: Interface Setup ✓ DONE
- [x] Interface header created and compiles
- [x] Wrapper functions created and compiles
- [x] Build succeeds with new files integrated

### Phase 5.3.2: Dispatcher Extraction
- [ ] Old dispatcher extracts to separate function
- [ ] Function signature matches interface
- [ ] All tests pass with extracted dispatcher
- [ ] No performance regression

### Phase 5.3.3: Generated Integration
- [ ] Generated dispatcher wraps in callable function
- [ ] Generated dispatcher compiles without errors
- [ ] All 176 opcodes dispatch correctly
- [ ] Extracted handlers are called properly

### Phase 5.3.4: Parallel Validation
- [ ] Both dispatchers run and complete execution
- [ ] Results are identical
- [ ] Performance is within <2% regression limit
- [ ] All tests pass
- [ ] Validation statistics show 100% match rate

### Phase 5.3.5: Generated as Default
- [ ] VDBE_USE_GENERATED_DISPATCH can be enabled
- [ ] All tests pass with generated dispatcher
- [ ] Old dispatcher remains available as fallback
- [ ] Performance is acceptable for production

## Testing Strategy

### Unit Tests
- Individual opcode handler tests (already exist)
- Dispatcher function tests (new)
- Interface compatibility tests (new)

### Integration Tests
- Full SQL test suite with old dispatcher
- Full SQL test suite with generated dispatcher
- Parallel comparison of both

### Performance Tests
- Benchmark with old dispatcher
- Benchmark with generated dispatcher
- Measure regression/improvement

### Validation Tests
- Execution statistics collection
- Mismatch detection and logging
- State consistency checking

## Next Steps

1. **Immediate (Phase 5.3.2)**: Extract old dispatcher loop into separate function
2. **Near-term (Phase 5.3.3)**: Integrate generated dispatcher wrapper
3. **Testing (Phase 5.3.4)**: Run parallel validation and compare results
4. **Completion (Phase 5.3.5)**: Enable generated dispatcher as default option

## Related Issues and References

- **Phase 5.1**: Code generator enhancement - COMPLETED
- **Phase 5.2**: Inline opcode extraction - COMPLETED
- **Phase 5.3**: Parallel dispatch validation - IN PROGRESS
- **Phase 5.4**: Cutover to generated dispatcher - PENDING
- **Phase 5.5**: Cleanup and polish - PENDING

## Notes

- Generated dispatcher is production-ready once validated
- Old dispatcher remains as fallback during transition period
- Wrapper approach allows incremental integration
- Can later refactor to Option B (state machine) if needed
- Documentation updated as phases complete

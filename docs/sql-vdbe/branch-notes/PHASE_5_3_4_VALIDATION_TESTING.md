# Phase 5.3.4: Parallel Validation Testing

## Overview

Phase 5.3.4 implements comprehensive parallel validation testing infrastructure for the VDBE dispatcher refactoring. This phase enables:

1. **Runtime dispatcher selection** via environment variables
2. **Parallel execution mode** to run both dispatchers side-by-side
3. **Result comparison** and validation infrastructure
4. **Performance monitoring** with <2% regression target

## Implementation Status

### Completed Infrastructure

#### 1. Dispatcher Mode Selection (vdbe_dispatch_interface.h)
- Added `VdbeDispatchMode` enum with 4 modes:
  - `VDBE_DISPATCH_AUTO` (0): Use compile-time default
  - `VDBE_DISPATCH_OLD` (1): Force old dispatcher
  - `VDBE_DISPATCH_GENERATED` (2): Force generated dispatcher
  - `VDBE_DISPATCH_PARALLEL` (3): Run both and compare

#### 2. Runtime Configuration (vdbe_dispatch_wrapper.c)
- `vdbe_get_dispatcher_mode()`: Initialize from `VDBE_DISPATCHER` env var
  ```bash
  export VDBE_DISPATCHER=old|generated|parallel|auto
  ```
- `vdbe_set_dispatcher_mode()`: Programmatic mode selection for testing

#### 3. Parallel Validation Execution (vdbe_dispatch_wrapper.c)
- `vdbe_exec_parallel_validation()`: Runs both dispatchers sequentially
  - Executes with old dispatcher first
  - Executes with generated dispatcher second
  - Compares return codes via `vdbe_validate_state()`
  - Returns result with validation metrics

#### 4. Validation Statistics (vdbe_dispatch_validate.c)
- Enhanced `vdbe_print_validation_stats()` with Phase 5.3.4 header
- Tracks:
  - Opcodes executed
  - Validation passes
  - Validation failures
  - Success rate
  - Performance overhead

#### 5. Dispatcher Selection (vdbe_dispatch_interface.h::vdbe_get_dispatcher)
- Updated to support all 4 dispatcher modes
- Returns appropriate function pointer based on mode
- Falls back to compile-time default if env var not set

## Testing Workflow

### Enable Parallel Validation Testing

```bash
# Set to parallel validation mode
export VDBE_DISPATCHER=parallel

# Or select individual dispatchers for testing
export VDBE_DISPATCHER=old    # Test old dispatcher
export VDBE_DISPATCHER=generated  # Test generated dispatcher

# Run test suite
cd /home/tsafin/tarantool
make test
```

### Verify Results

After testing:
```bash
# Check validation statistics
tail /tmp/vdbe_validation.log

# Verify success rate is 100% (all validators passed)
# Verify performance overhead is <2%
```

## Expected Behavior

### Current Status (Phase 5.3.4 Ready)

**Important**: Both `vdbe_exec_old_dispatcher()` and `vdbe_exec_generated_dispatcher()` currently call `sqlVdbeExec()`. This means:

1. **Parallel validation will show 100% match rate** - both dispatchers do identical execution
2. **Infrastructure is ready** for when Phase 5.3.3.2 implements actual generated dispatcher
3. **Test harness is operational** - can validate the testing framework itself

### Phase 5.3.3.2 (Next Step)

When the actual generated dispatcher is implemented:

```c
/* Phase 5.3.3.2: Implement actual generated dispatcher
 * - vdbe_exec_generated_dispatcher() will have real implementation
 * - Uses generated dispatch loop from vdbe_dispatch_generated.c
 * - Integrates extracted opcode handlers
 * - Parallel validation will compare old vs. generated implementations
 */
```

At that point, parallel validation will:
1. Execute same VDBE program with old dispatcher
2. Execute same program with generated dispatcher
3. Validate identical results
4. Verify <2% performance regression

## Validation Criteria

| Metric | Target | Status |
|--------|--------|--------|
| Execution identical | 100% match | Infrastructure ready |
| Performance regression | <2% | Measurable on real dispatcher |
| Test coverage | All opcodes | Depends on test suite |
| Error handling | Identical | Tracked in validation log |
| Memory consistency | No corruption | Validated via state checks |

## Integration Points

### Files Modified for Phase 5.3.4

1. **vdbe_dispatch_interface.h**
   - Added VdbeDispatchMode enum
   - Added mode getter/setter functions
   - Updated vdbe_get_dispatcher() to support all modes

2. **vdbe_dispatch_wrapper.c**
   - Added vdbe_get_dispatcher_mode() implementation
   - Added vdbe_set_dispatcher_mode() implementation
   - Added vdbe_exec_parallel_validation() function

3. **vdbe_dispatch_validate.c**
   - Enhanced vdbe_print_validation_stats() with Phase 5.3.4 headers
   - Improved error reporting

### Files Included but Not Modified

- **vdbe_dispatch.h**: Validation statistics structures (pre-existing)
- **vdbe.c**: Includes vdbe_dispatch.h for dispatcher selection

## Performance Expectations

### Phase 5.3.4 (Both dispatchers identical)
- Overhead: Minimal - just comparing return codes
- Expected <2% performance impact from validation logging

### Phase 5.3.3.2+ (Real generated dispatcher)
- Will measure actual dispatcher performance
- Target: Generated dispatcher within 2% of old dispatcher
- Parallel mode adds ~2% overhead (runs 2x as many programs)

## Next Steps After Phase 5.3.4

### Phase 5.3.3.2: Implement Actual Generated Dispatcher
- Refactor vdbe_dispatch_generated.c to be callable
- Remove label-based control flow
- Use return codes for control flow
- Integrate all 176 opcode handlers

### Phase 5.3.5: Enable Generated Dispatcher by Default
- Once Phase 5.3.3.2 is complete and validated
- Flip VDBE_USE_GENERATED_DISPATCH default to ON
- Run full test suite with generated dispatcher default
- Keep old dispatcher available as fallback

### Future Cleanup
- Phase 5.4: Deprecate old code
- Phase 5.5: Remove old implementation

## Debugging & Troubleshooting

### View Validation Log
```bash
cat /tmp/vdbe_validation.log
```

### Enable Detailed Tracing
```bash
export VDBE_DISPATCHER=parallel
export SQL_VDBETRACE=1  # If available in your build
```

### Test Individual Dispatcher
```bash
# Test old dispatcher
export VDBE_DISPATCHER=old
make test

# Test generated dispatcher
export VDBE_DISPATCHER=generated
make test
```

### Validation Failures
If `vdbe_print_validation_stats()` shows failures:
1. Check /tmp/vdbe_validation.log for details
2. Compare execution with SQL_VDBETRACE enabled
3. Verify opcode handler implementations match
4. Check for memory corruption or state inconsistency

## Key Differences from Manual Testing

| Aspect | Manual | Phase 5.3.4 |
|--------|--------|------------|
| Automation | Manual comparison | Automatic validation |
| Coverage | Selected tests | Full test suite |
| Metrics | Manual measurement | Automatic collection |
| Reproducibility | Low | High |
| CI/CD Integration | Complex | Built-in support |

## Files for Reference

- **Main validation logic**: `/home/tsafin/tarantool/src/box/sql/vdbe_dispatch_wrapper.c`
- **Dispatcher interface**: `/home/tsafin/tarantool/src/box/sql/vdbe_dispatch_interface.h`
- **Validation infrastructure**: `/home/tsafin/tarantool/src/box/sql/vdbe_dispatch_validate.c`
- **Configuration**: `VDBE_DISPATCHER` environment variable
- **Logging**: `/tmp/vdbe_validation.log`

## Conclusion

Phase 5.3.4 provides comprehensive infrastructure for testing and validating the VDBE dispatcher refactoring:

✓ Runtime dispatcher selection
✓ Parallel validation mode
✓ Result comparison framework
✓ Performance monitoring
✓ Comprehensive statistics

This infrastructure is ready for use and will become fully utilized when Phase 5.3.3.2 implements the actual generated dispatcher.

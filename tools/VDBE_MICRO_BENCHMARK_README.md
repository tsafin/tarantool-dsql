# VDBE Dispatcher Micro-Benchmark

**Created**: 2026-02-04 (Phase 5.9 / Pre-Phase 5.10)  
**Purpose**: Permanent performance baseline for VDBE dispatcher comparison

## Overview

This micro-benchmark suite provides fast, comprehensive performance testing of the VDBE dispatcher implementations. It was created before Phase 5.10 cleanup to preserve the ability to compare dispatcher performance even after the original inline dispatcher code is removed.

## Files

- **vdbe_micro_benchmark.lua** - Core benchmark suite (Lua/Tarantool)
- **compare_vdbe_dispatchers.sh** - Automated comparison script (Bash)
- **VDBE_MICRO_BENCHMARK_README.md** - This documentation

## Quick Start

### Option 1: Single Dispatcher Test

Run benchmark with current default dispatcher:
```bash
cd /home/tsafin/tarantool
./build/src/tarantool -e "dofile('tools/vdbe_micro_benchmark.lua')"
```

### Option 2: Compare Both Dispatchers

Run automated comparison (requires runtime dispatcher selection):
```bash
./tools/compare_vdbe_dispatchers.sh
```

Or specify custom binary:
```bash
./tools/compare_vdbe_dispatchers.sh /path/to/tarantool
```

### Option 3: Manual Comparison

```bash
# Test generated dispatcher
VDBE_DISPATCHER=generated ./build/src/tarantool \
    -e "dofile('tools/vdbe_micro_benchmark.lua')" > generated.txt

# Test original dispatcher
VDBE_DISPATCHER=old ./build/src/tarantool \
    -e "dofile('tools/vdbe_micro_benchmark.lua')" > original.txt

# Compare
diff generated.txt original.txt
```

## Benchmark Categories

The benchmark tests 9 categories of VDBE operations:

1. **Arithmetic** - Add, Subtract, Multiply, Divide, Remainder opcodes
2. **Bitwise** - BitAnd, BitOr, ShiftLeft, ShiftRight opcodes
3. **Comparison** - Eq, Ne, Lt, Le, Gt, Ge opcodes
4. **Data** - Integer, String, Null, Move, Copy opcodes
5. **Insert** - MakeRecord, ResultRow opcodes
6. **Index** - SeekGE, Found, IdxInsert opcodes
7. **Aggregate** - AggStep, AggFinal opcodes
8. **Cursor** - OpenRead, Rewind, Next, Column opcodes
9. **Mixed** - Realistic workload combining multiple operations

Each test runs:
- 3 warmup iterations (discarded)
- 10 measurement iterations (statistics calculated)
- 1000 operations per iteration (configurable)

## Configuration

Edit `vdbe_micro_benchmark.lua` to adjust:

```lua
local WARMUP_RUNS = 3      -- Warmup iterations
local MEASURE_RUNS = 10    -- Measurement iterations
local ITERATIONS = 1000    -- Operations per run
```

## Output Format

### Human-Readable Output

```
[TEST] Arithmetic Mix (arithmetic)
  Mean:   0.123456 s (±0.001234)
  Median: 0.123400 s
  Range:  0.122000 - 0.125000 s
```

### JSON Output

Complete results in machine-parseable JSON format:
```json
{
  "metadata": {
    "timestamp": "2026-02-04 12:34:56",
    "tarantool_version": "3.1.0",
    "dispatcher_mode": "generated",
    "warmup_runs": 3,
    "measure_runs": 10,
    "iterations": 1000
  },
  "tests": [
    {
      "name": "Arithmetic Mix",
      "category": "arithmetic",
      "stats": {
        "mean": 0.123456,
        "stddev": 0.001234,
        "min": 0.122000,
        "max": 0.125000,
        "median": 0.123400
      }
    }
  ]
}
```

## Comparison Report

The `compare_vdbe_dispatchers.sh` script generates a detailed comparison report:

```
TEST COMPARISON
================================================================================
Test Name                      Generated       Original        Diff
--------------------------------------------------------------------------------
Arithmetic Mix                 123.5 ms        135.2 ms       -8.7% ⚡
Bitwise Operations             45.6 ms         46.1 ms        -1.1% ≈
...
--------------------------------------------------------------------------------
TOTAL                          1.234 s         1.352 s        -8.7%

SUMMARY
================================================================================
✅ Generated dispatcher is 8.7% FASTER

BY CATEGORY
================================================================================
Category             Generated       Original        Diff
--------------------------------------------------------------------------------
aggregate            234.5 ms        245.6 ms       -4.5%
arithmetic           123.5 ms        135.2 ms       -8.7%
...
```

## Use Cases

### Before Phase 5.10 Cleanup

Run comparison to establish baseline:
```bash
./tools/compare_vdbe_dispatchers.sh > baseline_comparison.txt
```

Save results for future reference:
```bash
cp /tmp/vdbe_benchmark_*/comparison_report.txt \
   docs/VDBE_DISPATCHER_BASELINE_2026-02-04.txt
```

### After Phase 5.10 Cleanup

Run single dispatcher test to verify no regression:
```bash
./build/src/tarantool -e "dofile('tools/vdbe_micro_benchmark.lua')"
```

Compare against baseline:
```bash
# Current results
./build/src/tarantool -e "dofile('tools/vdbe_micro_benchmark.lua')" \
    > current_results.txt

# Compare with baseline
diff docs/VDBE_DISPATCHER_BASELINE_2026-02-04.txt current_results.txt
```

### Regression Testing

Integrate into CI/CD:
```bash
#!/bin/bash
# In CI pipeline
./tools/compare_vdbe_dispatchers.sh
if [ $? -ne 0 ]; then
    echo "Performance regression detected!"
    exit 1
fi
```

### Performance Profiling

Identify hotspots:
```bash
# Run with profiler
valgrind --tool=callgrind ./build/src/tarantool \
    -e "dofile('tools/vdbe_micro_benchmark.lua')"

# Analyze with kcachegrind
kcachegrind callgrind.out.*
```

## Interpreting Results

### Statistical Significance

- **Mean**: Primary metric for comparison
- **StdDev**: Lower is better (more consistent)
- **Median**: More robust to outliers than mean
- **Range**: Shows variability (min/max)

### Performance Thresholds

- **< -5%**: Significant improvement ⚡
- **-5% to +5%**: Equivalent performance ≈
- **> +5%**: Potential regression 🐌
- **< -2% overall**: Meets project target ✅

### Red Flags

Watch for:
- High standard deviation (>10% of mean) - inconsistent performance
- Unexpected regressions in specific categories
- Overall regression >2% from baseline

## Technical Details

### Runtime Dispatcher Selection

The benchmark relies on the `VDBE_DISPATCHER` environment variable:

```c
// In vdbe_dispatch_wrapper.c
VdbeDispatchMode vdbe_get_dispatcher_mode(void)
{
    const char *env = getenv("VDBE_DISPATCHER");
    if (env && strcmp(env, "old") == 0)
        return VDBE_DISPATCH_OLD;
    else if (env && strcmp(env, "generated") == 0)
        return VDBE_DISPATCH_GENERATED;
    // ...
}
```

Supported values:
- `old` - Original inline dispatcher
- `generated` - Generated dispatcher
- `auto` - Default (uses build configuration)
- `parallel` - Run both and compare (development only)

### Why Micro-Benchmark?

Advantages over full test suite:
1. **Fast**: <1 minute vs hours for full suite
2. **Focused**: Tests specific opcode patterns
3. **Reproducible**: Controlled environment, minimal noise
4. **Portable**: Single script, no external dependencies
5. **Permanent**: Can be preserved after cleanup

### Limitations

This is a micro-benchmark, not a full performance suite:
- Tests synthetic workloads, not real applications
- May not reflect production usage patterns
- Cache behavior differs from long-running operations
- Doesn't test all 176 opcodes individually

For comprehensive testing, also run:
- Full Tarantool test suite
- TPC-H benchmark (in /home/tsafin/tpch)
- Real application workloads

## Maintenance

### Adding New Tests

Edit `vdbe_micro_benchmark.lua` and add to `tests` table:

```lua
{
    name = "My New Test",
    category = "custom",
    description = "Tests specific opcode pattern",
    setup = function()
        -- Create tables, data
    end,
    run = function()
        -- Execute benchmark workload
        for i = 1, ITERATIONS do
            -- Your test here
        end
    end,
    teardown = function()
        -- Cleanup
    end
}
```

### Updating Configuration

For more detailed profiling:
```lua
local WARMUP_RUNS = 5      -- More warmup for stability
local MEASURE_RUNS = 20    -- More runs for confidence
local ITERATIONS = 5000    -- More ops per run
```

For quick smoke test:
```lua
local WARMUP_RUNS = 1
local MEASURE_RUNS = 3
local ITERATIONS = 100
```

## History

- **2026-02-04**: Created before Phase 5.10 cleanup
  - Purpose: Preserve ability to compare dispatchers
  - Baseline performance established
  - Both dispatchers validated

## Future Work

After Phase 5.10 cleanup:
1. Update comparison script to use historical baseline
2. Integrate into CI/CD pipeline
3. Add regression detection thresholds
4. Consider adding more opcode-specific tests

## See Also

- [PROJECT_STATUS_UPDATE.md](../docs/sql-vdbe/branch-notes/PROJECT_STATUS_UPDATE.md) - Overall project status
- [VDBE_REFACTOR_MASTER_PLAN.md](../docs/sql-vdbe/branch-notes/VDBE_REFACTOR_MASTER_PLAN.md) - Complete architecture
- [BUILD-VDBE.md](../BUILD-VDBE.md) - Build system details
- Phase 5.8 performance reports in project root

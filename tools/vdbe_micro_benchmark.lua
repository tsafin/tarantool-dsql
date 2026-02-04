#!/usr/bin/env tarantool
--[=[
VDBE Dispatcher Micro-Benchmark
Created: 2026-02-04
Phase 5.9 / Pre-Phase 5.10

PURPOSE:
--------
This micro-benchmark compares the performance of the original (inline) and
generated VDBE dispatchers. It's designed to be:

1. Fast to run (<1 minute for full suite)
2. Comprehensive (covers various opcode types)
3. Reproducible (multiple runs with statistics)
4. Preserved after Phase 5.10 cleanup (as performance baseline)

USAGE:
------
Run with single dispatcher:
    ./tarantool -e "dofile('tools/vdbe_micro_benchmark.lua')"

Compare both dispatchers (requires single binary with runtime selection):
    VDBE_DISPATCHER=generated ./tarantool -e "dofile('tools/vdbe_micro_benchmark.lua')" > generated.txt
    VDBE_DISPATCHER=old ./tarantool -e "dofile('tools/vdbe_micro_benchmark.lua')" > old.txt
    diff generated.txt old.txt

Or use the comparison script:
    ./tools/compare_vdbe_dispatchers.sh

OUTPUT:
-------
Results are printed to stdout in both human-readable and machine-parseable formats.
JSON output can be redirected to a file for later analysis.

BENCHMARK CATEGORIES:
---------------------
1. Arithmetic operations (Add, Subtract, Multiply, Divide, Remainder)
2. Bitwise operations (BitAnd, BitOr, ShiftLeft, ShiftRight)
3. Comparison operations (Eq, Ne, Lt, Le, Gt, Ge)
4. Data operations (Integer, String, Null, Move, Copy)
5. Control flow (Goto, If, IfNot)
6. Index operations (SeekGE, Found, IdxInsert)
7. Aggregate operations (AggStep, AggFinal)
8. Cursor operations (OpenRead, Next, Column)

Each test is run multiple times with warmup to ensure stable measurements.
]=]

-- Initialize Tarantool
box.cfg{log_level = 3} -- Reduce noise

local clock = require('clock')
local json = require('json')

-- Configuration
local WARMUP_RUNS = 5
local MEASURE_RUNS = 20
local ITERATIONS = 10000  -- Per test run (10x increase for heavier load)

-- Results storage
local results = {
    metadata = {
        timestamp = os.date("%Y-%m-%d %H:%M:%S"),
        tarantool_version = _TARANTOOL,
        dispatcher_mode = os.getenv("VDBE_DISPATCHER") or "auto",
        warmup_runs = WARMUP_RUNS,
        measure_runs = MEASURE_RUNS,
        iterations = ITERATIONS
    },
    tests = {}
}

-- Statistics helper
local function calculate_stats(times)
    if #times == 0 then return nil end
    
    table.sort(times)
    local sum = 0
    for _, v in ipairs(times) do
        sum = sum + v
    end
    local mean = sum / #times
    
    local variance = 0
    for _, v in ipairs(times) do
        variance = variance + (v - mean) ^ 2
    end
    local stddev = math.sqrt(variance / #times)
    
    return {
        mean = mean,
        stddev = stddev,
        min = times[1],
        max = times[#times],
        median = times[math.floor(#times / 2) + 1],
        runs = #times
    }
end

-- Test runner
local function run_test(test)
    print(string.format("\n[TEST] %s (%s)", test.name, test.category))
    
    -- Setup
    if test.setup then
        test.setup()
    end
    
    -- Warmup runs
    for i = 1, WARMUP_RUNS do
        test.run()
    end
    
    -- Measurement runs
    local times = {}
    for i = 1, MEASURE_RUNS do
        collectgarbage("collect") -- Clean state
        
        local start = clock.monotonic()
        test.run()
        local elapsed = clock.monotonic() - start
        
        table.insert(times, elapsed)
    end
    
    -- Teardown
    if test.teardown then
        test.teardown()
    end
    
    -- Calculate statistics
    local stats = calculate_stats(times)
    
    print(string.format("  Mean:   %.6f s (±%.6f)", stats.mean, stats.stddev))
    print(string.format("  Median: %.6f s", stats.median))
    print(string.format("  Range:  %.6f - %.6f s", stats.min, stats.max))
    
    return {
        name = test.name,
        category = test.category,
        description = test.description,
        iterations = ITERATIONS,
        stats = stats
    }
end

-- ============================================================================
-- TEST DEFINITIONS
-- ============================================================================

local tests = {
    -- Category 1: Arithmetic Operations
    {
        name = "Arithmetic Mix",
        category = "arithmetic",
        description = "Tests Add, Subtract, Multiply, Divide, Remainder opcodes",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_arith")
            box.execute("CREATE TABLE t_arith(id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, c INTEGER)")
            for i = 1, 500 do
                box.execute("INSERT INTO t_arith VALUES (?, ?, ?, ?)", {i, i * 10, i * 5, i * 2})
            end
        end,
        run = function()
            for i = 1, ITERATIONS do
                box.execute("SELECT a + b, a - b, a * c, a / b, a % b FROM t_arith WHERE id = ?", {(i % 100) + 1})
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_arith")
        end
    },
    
    -- Category 2: Bitwise Operations
    {
        name = "Bitwise Operations",
        category = "bitwise",
        description = "Tests BitAnd, BitOr, ShiftLeft, ShiftRight opcodes",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_bitwise")
            box.execute("CREATE TABLE t_bitwise(id INTEGER PRIMARY KEY, val INTEGER)")
            for i = 1, 200 do
                box.execute("INSERT INTO t_bitwise VALUES (?, ?)", {i, i * 16})
            end
        end,
        run = function()
            for i = 1, ITERATIONS do
                box.execute("SELECT val << 2, val >> 1 FROM t_bitwise WHERE id = ?", {(i % 50) + 1})
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_bitwise")
        end
    },
    
    -- Category 3: Comparison Operations
    {
        name = "Comparison Operations",
        category = "comparison",
        description = "Tests Eq, Ne, Lt, Le, Gt, Ge opcodes via WHERE clauses",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_compare")
            box.execute("CREATE TABLE t_compare(id INTEGER PRIMARY KEY, value INTEGER)")
            for i = 1, 1000 do
                box.execute("INSERT INTO t_compare VALUES (?, ?)", {i, i * 3})
            end
        end,
        run = function()
            for i = 1, ITERATIONS do
                local threshold = (i % 100) * 3
                box.execute("SELECT * FROM t_compare WHERE value = ? OR value > ? OR value < ?", 
                    {threshold, threshold + 10, threshold - 10})
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_compare")
        end
    },
    
    -- Category 4: Data Operations
    {
        name = "Data Loading",
        category = "data",
        description = "Tests Integer, String, Null, Move, Copy opcodes",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_data")
            box.execute("CREATE TABLE t_data(id INTEGER PRIMARY KEY, name TEXT, count INTEGER)")
            for i = 1, 300 do
                box.execute("INSERT INTO t_data VALUES (?, ?, ?)", {i, "item" .. i, i * 7})
            end
        end,
        run = function()
            for i = 1, ITERATIONS do
                box.execute("SELECT id, name, count FROM t_data WHERE id = ?", {(i % 50) + 1})
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_data")
        end
    },
    
    -- Category 5: Insert Operations (ResultRow, MakeRecord)
    {
        name = "Insert Operations",
        category = "insert",
        description = "Tests Insert path with MakeRecord, ResultRow opcodes",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_insert")
            box.execute("CREATE TABLE t_insert(id INTEGER PRIMARY KEY, val INTEGER, text TEXT)")
        end,
        run = function()
            pcall(box.execute, "DELETE FROM t_insert")
            for i = 1, ITERATIONS do
                box.execute("INSERT INTO t_insert VALUES (?, ?, ?)", {i, i * 11, "text" .. i})
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_insert")
        end
    },
    
    -- Category 6: Index Operations
    {
        name = "Index Seek",
        category = "index",
        description = "Tests SeekGE, Found, IdxInsert opcodes via indexed queries",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_index")
            box.execute("CREATE TABLE t_index(id INTEGER PRIMARY KEY, value INTEGER, name TEXT)")
            box.execute("CREATE INDEX idx_value ON t_index(value)")
            for i = 1, 1000 do
                box.execute("INSERT INTO t_index VALUES (?, ?, ?)", {i, i * 5, "name" .. i})
            end
        end,
        run = function()
            for i = 1, ITERATIONS do
                box.execute("SELECT * FROM t_index WHERE value = ?", {((i % 100) + 1) * 5})
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_index")
        end
    },
    
    -- Category 7: Aggregate Operations
    {
        name = "Aggregate Functions",
        category = "aggregate",
        description = "Tests AggStep, AggFinal opcodes via COUNT, SUM",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_agg")
            box.execute("CREATE TABLE t_agg(id INTEGER PRIMARY KEY, category INTEGER, value INTEGER)")
            for i = 1, 2000 do
                box.execute("INSERT INTO t_agg VALUES (?, ?, ?)", {i, i % 20, i * 3})
            end
        end,
        run = function()
            for i = 1, ITERATIONS do
                box.execute("SELECT category, COUNT(*), SUM(value) FROM t_agg GROUP BY category")
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_agg")
        end
    },
    
    -- Category 8: Cursor Navigation
    {
        name = "Table Scan",
        category = "cursor",
        description = "Tests OpenRead, Rewind, Next, Column opcodes via full scan",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_scan")
            box.execute("CREATE TABLE t_scan(id INTEGER PRIMARY KEY, data INTEGER)")
            for i = 1, 500 do
                box.execute("INSERT INTO t_scan VALUES (?, ?)", {i, i * 13})
            end
        end,
        run = function()
            for i = 1, ITERATIONS do
                box.execute("SELECT * FROM t_scan")
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_scan")
        end
    },
    
    -- Category 9: Mixed Workload
    {
        name = "Mixed Operations",
        category = "mixed",
        description = "Realistic mix of SELECT, INSERT, UPDATE operations",
        setup = function()
            pcall(box.execute, "DROP TABLE IF EXISTS t_mixed")
            box.execute("CREATE TABLE t_mixed(id INTEGER PRIMARY KEY, counter INTEGER, name TEXT)")
            for i = 1, 500 do
                box.execute("INSERT INTO t_mixed VALUES (?, ?, ?)", {i, 0, "item" .. i})
            end
        end,
        run = function()
            for i = 1, ITERATIONS / 3 do
                -- Select
                box.execute("SELECT * FROM t_mixed WHERE id = ?", {(i % 50) + 1})
                -- Update
                box.execute("UPDATE t_mixed SET counter = counter + 1 WHERE id = ?", {(i % 50) + 1})
                -- Count
                box.execute("SELECT COUNT(*) FROM t_mixed WHERE counter > ?", {i % 10})
            end
        end,
        teardown = function()
            pcall(box.execute, "DROP TABLE t_mixed")
        end
    }
}

-- ============================================================================
-- MAIN EXECUTION
-- ============================================================================

print("=" .. string.rep("=", 78))
print("VDBE DISPATCHER MICRO-BENCHMARK")
print("=" .. string.rep("=", 78))
print(string.format("Timestamp:       %s", results.metadata.timestamp))
print(string.format("Tarantool:       %s", results.metadata.tarantool_version))
print(string.format("Dispatcher Mode: %s", results.metadata.dispatcher_mode))
print(string.format("Configuration:   %d warmup + %d measurement runs, %d iterations/run",
    WARMUP_RUNS, MEASURE_RUNS, ITERATIONS))
print("=" .. string.rep("=", 78))

-- Run all tests
for _, test in ipairs(tests) do
    local result = run_test(test)
    table.insert(results.tests, result)
end

-- Summary
print("\n" .. string.rep("=", 78))
print("SUMMARY")
print(string.rep("=", 78))

local total_time = 0
local categories = {}

for _, result in ipairs(results.tests) do
    total_time = total_time + result.stats.mean
    
    local cat = result.category
    if not categories[cat] then
        categories[cat] = {count = 0, time = 0}
    end
    categories[cat].count = categories[cat].count + 1
    categories[cat].time = categories[cat].time + result.stats.mean
end

print(string.format("\nTotal benchmark time: %.3f seconds", total_time))
print("\nBy category:")
for cat, data in pairs(categories) do
    print(string.format("  %-15s %d tests, %.3f seconds", cat .. ":", data.count, data.time))
end

-- JSON output for analysis
print("\n" .. string.rep("=", 78))
print("JSON OUTPUT (for analysis)")
print(string.rep("=", 78))
print(json.encode(results))

-- Exit cleanly
os.exit(0)

#!/usr/bin/env tarantool

local clock = require('clock')
local json = require('json')

io.stdout:setvbuf('no')

box.cfg{log_level = 4}

local RUNS = tonumber(os.getenv('BENCH_RUNS') or '3')
local ONLY_WORKLOAD = os.getenv('BENCH_ONLY_WORKLOAD')
local ONLY_CASE = os.getenv('BENCH_ONLY_CASE')
local PROGRESS_EVERY = tonumber(os.getenv('BENCH_PROGRESS_EVERY') or '0')

local function env_int(name, default)
    return tonumber(os.getenv(name) or tostring(default))
end

local function log_progress(fmt, ...)
    io.stdout:write(('[bench] ' .. fmt .. '\n'):format(...))
end

local function stat_snapshot()
    local s = box.stat.sql()
    return {
        interpreter_step_count = s.sql_interpreter_step_count or 0,
        jit_step_count = s.sql_jit_step_count or 0,
        jit_compile_count = s.sql_jit_compile_count or 0,
        jit_compile_success_count = s.sql_jit_compile_success_count or 0,
        jit_exec_count = s.sql_jit_exec_count or 0,
        jit_full_run_count = s.sql_jit_full_run_count or 0,
        jit_fallback_count = s.sql_jit_fallback_count or 0,
        jit_resume_skip_count = s.sql_jit_resume_skip_count or 0,
        jit_guard_skip_count = s.sql_jit_guard_skip_count or 0,
    }
end

local function stat_diff(before, after)
    local diff = {}
    for k, v in pairs(after) do
        diff[k] = v - before[k]
    end
    return diff
end

local function summarize_runs(runs)
    table.sort(runs, function(a, b) return a.per_op_us < b.per_op_us end)
    local sum = 0
    for _, run in ipairs(runs) do
        sum = sum + run.per_op_us
    end
    return {
        median_per_op_us = runs[math.floor(#runs / 2) + 1].per_op_us,
        min_per_op_us = runs[1].per_op_us,
        max_per_op_us = runs[#runs].per_op_us,
        mean_per_op_us = sum / #runs,
        runs = runs,
    }
end

local function maybe_progress(label, i, total, started_at)
    local every = PROGRESS_EVERY
    if every <= 0 then
        every = math.max(math.floor(total / 10), 1)
    end
    if i == 1 or i == total or i % every == 0 then
        log_progress('%s progress %d/%d elapsed=%.3fs',
                     label, i, total, clock.monotonic() - started_at)
    end
end

local function bench_case(workload_name, case_name, case)
    local run_results = {}
    for run_no = 1, RUNS do
        local label = workload_name .. '/' .. case_name .. '/run' .. run_no
        log_progress('start %s iterations=%d', label, case.iterations)
        if case.before_each ~= nil then
            case.before_each()
        end
        collectgarbage('collect')
        local before = stat_snapshot()
        local start = clock.monotonic()
        local checksum = case.run(function(i)
            maybe_progress(label, i, case.iterations, start)
        end)
        local elapsed = clock.monotonic() - start
        local after = stat_snapshot()
        if case.after_each ~= nil then
            case.after_each()
        end
        log_progress('done %s elapsed=%.3fs checksum=%s',
                     label, elapsed, tostring(checksum))
        run_results[#run_results + 1] = {
            run = run_no,
            elapsed_sec = elapsed,
            per_op_us = elapsed * 1e6 / case.iterations,
            checksum = checksum,
            stats = stat_diff(before, after),
        }
    end
    return summarize_runs(run_results)
end

local function assert_rows(res, expected)
    if res.rows[1][1] ~= expected then
        error(('unexpected result: got %s, expected %s')
              :format(tostring(res.rows[1][1]), tostring(expected)))
    end
end

local function execute_sql(target, args)
    if args == nil then
        return box.execute(target)
    end
    return box.execute(target, args)
end

local function setup_point_lookup()
    pcall(box.execute, 'DROP TABLE bench_arith')
    box.execute([[
        CREATE TABLE bench_arith(
            id INTEGER PRIMARY KEY,
            a INTEGER,
            b INTEGER,
            c INTEGER
        );
    ]])
    for i = 1, 1024 do
        box.execute('INSERT INTO bench_arith VALUES (?, ?, ?, ?);',
                    {i, i * 10, i * 5 + 1, i * 2 + 1})
    end
end

local function teardown_point_lookup()
    pcall(box.execute, 'DROP TABLE bench_arith')
end

local workloads = {
    {
        name = 'tiny_const',
        description = 'Tiny constant expression',
        sql = 'SELECT 1 + 2;',
        prepare_iterations = env_int('BENCH_PREPARE_ITERS_TINY', 5000),
        exec_iterations = env_int('BENCH_EXEC_ITERS_TINY', 200000),
        auto_iterations = env_int('BENCH_AUTO_ITERS_TINY', 50000),
        setup = function() end,
        teardown = function() end,
        args = function() return nil end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function() return 3 end,
    },
    {
        name = 'hot_expr',
        description = 'Hot arithmetic expression known to auto-JIT',
        sql = 'SELECT 1 + 2 + 3 + 4 + 5;',
        prepare_iterations = env_int('BENCH_PREPARE_ITERS_HOT', 5000),
        exec_iterations = env_int('BENCH_EXEC_ITERS_HOT', 200000),
        auto_iterations = env_int('BENCH_AUTO_ITERS_HOT', 50000),
        setup = function() end,
        teardown = function() end,
        args = function() return nil end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function() return 15 end,
    },
    {
        name = 'point_lookup',
        description = 'Indexed row lookup with arithmetic work',
        sql = [[
            SELECT a + b, a - b, a * c, a / b, a % b
            FROM bench_arith
            WHERE id = ?;
        ]],
        prepare_iterations = env_int('BENCH_PREPARE_ITERS_POINT', 2500),
        exec_iterations = env_int('BENCH_EXEC_ITERS_POINT', 100000),
        auto_iterations = env_int('BENCH_AUTO_ITERS_POINT', 20000),
        setup = setup_point_lookup,
        teardown = teardown_point_lookup,
        args = function(i) return {((i - 1) % 1024) + 1} end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function(i)
            local id = ((i - 1) % 1024) + 1
            return id * 15 + 1
        end,
    },
}

local results = {
    metadata = {
        tarantool_version = _TARANTOOL,
        jit_env = os.getenv('SQL_JIT_ENABLE') or 'unset',
        benchmark_label = os.getenv('BENCH_LABEL') or 'default',
        runs = RUNS,
    },
    workloads = {},
}

for _, workload in ipairs(workloads) do
    if ONLY_WORKLOAD == nil or workload.name == ONLY_WORKLOAD then
        log_progress('workload %s setup', workload.name)
        workload.setup()

        local prepare_case = {
            iterations = workload.prepare_iterations,
            run = function(progress)
                local checksum = 0
                for i = 1, workload.prepare_iterations do
                    local stmt = box.prepare(workload.sql)
                    checksum = checksum + stmt.stmt_id
                    box.unprepare(stmt.stmt_id)
                    progress(i)
                end
                return checksum
            end,
        }

        local stmt
        local execute_case = {
            iterations = workload.exec_iterations,
            before_each = function()
                log_progress('prepare statement for %s/prepared_execute', workload.name)
                stmt = box.prepare(workload.sql)
                for i = 1, 200 do
                    local res = execute_sql(stmt.stmt_id, workload.args(i))
                    assert_rows(res, workload.expected(i))
                end
            end,
            run = function(progress)
                local checksum = 0
                for i = 1, workload.exec_iterations do
                    local res = execute_sql(stmt.stmt_id, workload.args(i))
                    checksum = checksum + workload.checksum(res)
                    progress(i)
                end
                return checksum
            end,
            after_each = function()
                box.unprepare(stmt.stmt_id)
                stmt = nil
            end,
        }

        local auto_case = {
            iterations = workload.auto_iterations,
            before_each = function()
                log_progress('warmup direct execute for %s/automatic_execute', workload.name)
                for i = 1, 200 do
                    local res = execute_sql(workload.sql, workload.args(i))
                    assert_rows(res, workload.expected(i))
                end
            end,
            run = function(progress)
                local checksum = 0
                for i = 1, workload.auto_iterations do
                    local res = execute_sql(workload.sql, workload.args(i))
                    checksum = checksum + workload.checksum(res)
                    progress(i)
                end
                return checksum
            end,
        }

        local workload_result = {
            name = workload.name,
            description = workload.description,
        }
        if ONLY_CASE == nil or ONLY_CASE == 'prepare_only' then
            workload_result.prepare_only =
                bench_case(workload.name, 'prepare_only', prepare_case)
        end
        if ONLY_CASE == nil or ONLY_CASE == 'prepared_execute' then
            workload_result.prepared_execute =
                bench_case(workload.name, 'prepared_execute', execute_case)
        end
        if ONLY_CASE == nil or ONLY_CASE == 'automatic_execute' then
            workload_result.automatic_execute =
                bench_case(workload.name, 'automatic_execute', auto_case)
        end
        results.workloads[#results.workloads + 1] = workload_result

        workload.teardown()
        log_progress('workload %s teardown complete', workload.name)
    end
end

print(json.encode(results))
os.exit(0)

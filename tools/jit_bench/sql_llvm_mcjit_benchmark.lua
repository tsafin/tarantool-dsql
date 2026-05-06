#!/usr/bin/env tarantool

local clock = require('clock')
local json = require('json')
local bit = require('bit')

io.stdout:setvbuf('no')
io.stderr:setvbuf('no')

box.cfg{log_level = 4}

local RUNS = tonumber(os.getenv('BENCH_RUNS') or '3')
local ONLY_WORKLOAD = os.getenv('BENCH_ONLY_WORKLOAD')
local ONLY_CASE = os.getenv('BENCH_ONLY_CASE')
local PROGRESS_EVERY = tonumber(os.getenv('BENCH_PROGRESS_EVERY') or '0')
local DISCARD_RESULTS = os.getenv('BENCH_DISCARD_RESULTS') == '1'

local function env_int(name, default)
    return tonumber(os.getenv(name) or tostring(default))
end

local function env_int_compat(name, legacy_name, default)
    return tonumber(os.getenv(name) or os.getenv(legacy_name) or tostring(default))
end

local function log_progress(fmt, ...)
    io.stderr:write(('[bench] ' .. fmt .. '\n'):format(...))
end

local function stat_snapshot()
    local s = box.stat.sql()
    local cnp_opcode_count = {}
    if s.cnp_opcode_profile ~= nil and s.cnp_opcode_profile.count ~= nil then
        for k, v in pairs(s.cnp_opcode_profile.count) do
            cnp_opcode_count[k] = v
        end
    end
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
        cnp_compile_count = s.sql_cnp_compile_count or 0,
        cnp_compile_success_count = s.sql_cnp_compile_success_count or 0,
        cnp_exec_count = s.sql_cnp_exec_count or 0,
        cnp_step_count = s.sql_cnp_step_count or 0,
        cnp_fallback_count = s.sql_cnp_fallback_count or 0,
        cnp_resume_count = s.sql_cnp_resume_count or 0,
        cnp_pc_jump_count = s.sql_cnp_pc_jump_count or 0,
        cnp_row_return_count = s.sql_cnp_row_return_count or 0,
        cnp_done_return_count = s.sql_cnp_done_return_count or 0,
        cnp_error_return_count = s.sql_cnp_error_return_count or 0,
        cnp_compiled_bytes = s.sql_cnp_compiled_bytes or 0,
        cnp_opcode_count = cnp_opcode_count,
    }
end

local function stat_diff(before, after)
    local diff = {}
    for k, v in pairs(after) do
        if type(v) == 'table' then
            local table_diff = {}
            local before_table = before[k] or {}
            for kk, vv in pairs(v) do
                local delta = vv - (before_table[kk] or 0)
                if delta ~= 0 then
                    table_diff[kk] = delta
                end
            end
            diff[k] = table_diff
        else
            diff[k] = v - before[k]
        end
    end
    return diff
end

local function summarize_runs(runs)
    table.sort(runs, function(a, b) return a.per_op_us < b.per_op_us end)
    local sum = 0
    local elapsed_sum = 0
    for _, run in ipairs(runs) do
        sum = sum + run.per_op_us
        elapsed_sum = elapsed_sum + run.elapsed_sec
    end
    return {
        median_per_op_us = runs[math.floor(#runs / 2) + 1].per_op_us,
        min_per_op_us = runs[1].per_op_us,
        max_per_op_us = runs[#runs].per_op_us,
        mean_per_op_us = sum / #runs,
        mean_elapsed_sec = elapsed_sum / #runs,
        min_elapsed_sec = runs[1].elapsed_sec,
        max_elapsed_sec = runs[#runs].elapsed_sec,
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

local function execute_sql_no_result(target, args)
    if args == nil then
        return box.internal.execute_no_result(target)
    end
    return box.internal.execute_no_result(target, args)
end

local function setup_bench_arith(with_scan_hint_indexes)
    pcall(box.execute, 'DROP TABLE bench_arith')
    box.execute([[
        CREATE TABLE bench_arith(
            id INTEGER PRIMARY KEY,
            a INTEGER,
            b INTEGER,
            c INTEGER,
            d INTEGER,
            e INTEGER,
            f INTEGER,
            g INTEGER,
            h INTEGER,
            i INTEGER,
            j INTEGER,
            k INTEGER
        );
    ]])
    if with_scan_hint_indexes then
        box.execute('CREATE INDEX bench_arith_d ON bench_arith(d);')
        box.execute('CREATE INDEX bench_arith_e ON bench_arith(e);')
        box.execute('CREATE INDEX bench_arith_g ON bench_arith(g);')
        box.execute('CREATE INDEX bench_arith_h ON bench_arith(h);')
        box.execute('CREATE INDEX bench_arith_i ON bench_arith(i);')
        box.execute('CREATE INDEX bench_arith_j ON bench_arith(j);')
        box.execute('CREATE INDEX bench_arith_k ON bench_arith(k);')
    end
    for i = 1, 1024 do
        local a = i * 10
        local b = i * 5 + 1
        local c = i * 2 + 1
        local d = i * 3 + 7
        local e = i * 4 + 9
        local f = i * 6 + 11
        local g = i * 8 + 13
        local h = i * 9 + 15
        local ii = i * 11 + 17
        local j = i * 12 + 19
        local k = i * 14 + 23
        box.execute('INSERT INTO bench_arith VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);',
                    {i, a, b, c, d, e, f, g, h, ii, j, k})
    end
end

local function bench_arith_wide_value(i, fieldno)
    return i * (fieldno + 3) + fieldno * 7 + (fieldno % 5)
end

local function setup_bench_arith_wide_sparse(with_scan_hint_indexes)
    pcall(box.execute, 'DROP TABLE bench_arith')
    local defs = {'id INTEGER PRIMARY KEY'}
    for fieldno = 1, 50 do
        defs[#defs + 1] = string.format('f%02d INTEGER', fieldno)
    end
    box.execute(string.format([[
        CREATE TABLE bench_arith(
            %s
        );
    ]], table.concat(defs, ',\n            ')))
    if with_scan_hint_indexes then
        box.execute('CREATE INDEX bench_arith_f20 ON bench_arith(f20);')
        box.execute('CREATE INDEX bench_arith_f24 ON bench_arith(f24);')
        box.execute('CREATE INDEX bench_arith_f37 ON bench_arith(f37);')
        box.execute('CREATE INDEX bench_arith_f46 ON bench_arith(f46);')
        box.execute('CREATE INDEX bench_arith_f48 ON bench_arith(f48);')
    end
    local placeholders = {'?'}
    for fieldno = 1, 50 do
        placeholders[#placeholders + 1] = '?'
    end
    local insert_sql = string.format('INSERT INTO bench_arith VALUES (%s);',
                                     table.concat(placeholders, ', '))
    for i = 1, 1024 do
        local row = {i}
        for fieldno = 1, 50 do
            row[#row + 1] = bench_arith_wide_value(i, fieldno)
        end
        box.execute(insert_sql, row)
    end
end

local function setup_point_lookup()
    setup_bench_arith(false)
end

local function setup_row_prefetch()
    setup_bench_arith(true)
end

local function teardown_point_lookup()
    pcall(box.execute, 'DROP TABLE bench_arith')
end

local wide_scan_expected

local function setup_wide_scan(with_scan_hint_indexes)
    setup_bench_arith_wide_sparse(with_scan_hint_indexes)
    wide_scan_expected = {}
    local window = 128
    for start_id = 1, 1024 do
        local sum1 = 0
        local sum2 = 0
        local cnt = 0
        local max_mod = nil
        local finish_id = math.min(start_id + window - 1, 1024)
        for i = start_id, finish_id do
            sum1 = sum1 + bench_arith_wide_value(i, 3) +
                bench_arith_wide_value(i, 20) +
                bench_arith_wide_value(i, 29) +
                bench_arith_wide_value(i, 37)
            sum2 = sum2 + bench_arith_wide_value(i, 41) +
                bench_arith_wide_value(i, 46) +
                bench_arith_wide_value(i, 48) +
                bench_arith_wide_value(i, 49)
            cnt = cnt + 1
            local mod = bench_arith_wide_value(i, 50) % 97
            if max_mod == nil or mod > max_mod then
                max_mod = mod
            end
        end
        wide_scan_expected[start_id] = {sum1, sum2, cnt, max_mod}
    end
end

local function setup_wide_scan_nohint()
    setup_wide_scan(false)
end

local function setup_wide_scan_hint()
    setup_wide_scan(true)
end

local function teardown_wide_scan()
    wide_scan_expected = nil
    teardown_point_lookup()
end

local WIDE_SCAN_SQL = [[
    SELECT sum(f03 + f20 + f29 + f37),
           sum(f41 + f46 + f48 + f49),
           count(*),
           max(f50 % 97)
    FROM bench_arith
    WHERE id BETWEEN ? AND ?;
]]

local builtin_scan_expected

local function setup_builtin_scan()
    pcall(box.execute, 'DROP TABLE bench_text')
    box.execute([[
        CREATE TABLE bench_text(
            id INTEGER PRIMARY KEY,
            s1 STRING,
            s2 STRING,
            n1 INTEGER,
            n2 INTEGER,
            s3 STRING,
            s4 STRING,
            n3 INTEGER,
            n4 INTEGER,
            s5 STRING,
            s6 STRING
        );
    ]])
    box.execute('CREATE INDEX bench_text_s3 ON bench_text(s3);')
    box.execute('CREATE INDEX bench_text_s4 ON bench_text(s4);')
    box.execute('CREATE INDEX bench_text_n1 ON bench_text(n1);')
    box.execute('CREATE INDEX bench_text_n2 ON bench_text(n2);')
    box.execute('CREATE INDEX bench_text_n3 ON bench_text(n3);')
    box.execute('CREATE INDEX bench_text_n4 ON bench_text(n4);')
    box.execute('CREATE INDEX bench_text_s5 ON bench_text(s5);')
    box.execute('CREATE INDEX bench_text_s6 ON bench_text(s6);')

    builtin_scan_expected = {}
    local rows = {}
    for i = 1, 2048 do
        local s1 = string.format('row-%04d-xx-%d', i, i % 11)
        local s2 = string.format('alpha-%d-zeta-%d', i % 17, i % 5)
        local n1 = i * 13
        local n2 = i * 7 + 3
        local s3 = string.format('gamma-%04d-theta-%d', i, i % 13)
        local s4 = string.format('omega-%d-sigma-%04d', i % 19, i)
        local n3 = i * 5 + 21
        local n4 = i * 9 + 29
        local s5 = string.format('lambda-%d-kappa-%d', i % 23, i % 7)
        local s6 = string.format('phi-%04d-rho-%d', i, i % 3)
        box.execute('INSERT INTO bench_text VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);',
                    {i, s1, s2, n1, n2, s3, s4, n3, n4, s5, s6})
        rows[i] = {
            s1 = s1, s2 = s2, n1 = n1, n2 = n2,
            s3 = s3, s4 = s4, n3 = n3, n4 = n4, s5 = s5, s6 = s6,
        }
    end

    local window = 128
    for start_id = 1, 2048 do
        local total = 0
        local finish_id = math.min(start_id + window - 1, 2048)
        for i = start_id, finish_id do
            local row = rows[i]
            total = total + #string.sub(row.s3, 2, 6) +
                    #string.upper(row.s4) +
                    math.abs(row.n3 - row.n4) +
                    #string.lower(row.s5) +
                    #string.sub(row.s6, 3, 8) +
                    math.abs(row.n1 - row.n2)
        end
        builtin_scan_expected[start_id] = total
    end
end

local function teardown_builtin_scan()
    builtin_scan_expected = nil
    pcall(box.execute, 'DROP TABLE bench_text')
end

local sort_window_expected

local function setup_sort_window()
    setup_point_lookup()
    sort_window_expected = {}
    local window = 256
    local limit = 32
    for start_id = 1, 1024 do
        local rows = {}
        local finish_id = math.min(start_id + window - 1, 1024)
        for i = start_id, finish_id do
            local a = i * 10
            local b = i * 5 + 1
            local c = i * 2 + 1
            rows[#rows + 1] = {
                score = (a * 17 + b * 7 - c * 3) % 257,
                b = b,
                id = i,
            }
        end
        table.sort(rows, function(lhs, rhs)
            if lhs.score ~= rhs.score then
                return lhs.score > rhs.score
            end
            if lhs.b ~= rhs.b then
                return lhs.b < rhs.b
            end
            return lhs.id > rhs.id
        end)
        local sum_score = 0
        local sum_id = 0
        local top = math.min(limit, #rows)
        for i = 1, top do
            sum_score = sum_score + rows[i].score
            sum_id = sum_id + rows[i].id
        end
        sort_window_expected[start_id] = {sum_score, sum_id}
    end
end

local function teardown_sort_window()
    sort_window_expected = nil
    teardown_point_lookup()
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
    {
        name = 'bitwise_mix',
        description = 'Indexed row lookup with denser immediate-heavy bitwise work',
        sql = [[
            SELECT (((a & b) | c) & ((a << 1) | (b >> 1))),
                   ((a | b) & (c << 2)),
                   ((a >> 1) | (b << 1)),
                   ((~a) & 1023),
                   ((a & (~b)) | (c >> 1)),
                   ((a << 2) & (b | 255)),
                   ((a & 1023) | ((~b) & 255)),
                   (((a << 1) & 1023) | ((b >> 1) & 255)),
                   (((c << 2) | (a & 255)) & ((~b) & 1023))
            FROM bench_arith
            WHERE id = ?;
        ]],
        prepare_iterations = env_int('BENCH_PREPARE_ITERS_BITWISE', 2500),
        exec_iterations = env_int('BENCH_EXEC_ITERS_BITWISE', 100000),
        auto_iterations = env_int('BENCH_AUTO_ITERS_BITWISE', 20000),
        setup = setup_point_lookup,
        teardown = teardown_point_lookup,
        args = function(i) return {((i - 1) % 1024) + 1} end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function(i)
            local id = ((i - 1) % 1024) + 1
            local a = id * 10
            local b = id * 5 + 1
            local c = id * 2 + 1
            return bit.band(bit.bor(bit.band(a, b), c),
                            bit.bor(bit.lshift(a, 1), bit.rshift(b, 1)))
        end,
    },
    {
        name = 'row_prefetch',
        description = 'Indexed row lookup with high-field leader and later back-edges',
        sql = [[
            SELECT a + b + k + c + e + g
            FROM bench_arith
            WHERE id = ?;
        ]],
        prepare_iterations = env_int('BENCH_PREPARE_ITERS_ROW_PREFETCH', 2500),
        exec_iterations = env_int('BENCH_EXEC_ITERS_ROW_PREFETCH', 100000),
        auto_iterations = env_int('BENCH_AUTO_ITERS_ROW_PREFETCH', 100000),
        setup = setup_row_prefetch,
        teardown = teardown_point_lookup,
        args = function(i) return {((i - 1) % 1024) + 1} end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function(i)
            local id = ((i - 1) % 1024) + 1
            local a = id * 10
            local b = id * 5 + 1
            local c = id * 2 + 1
            local e = id * 4 + 9
            local g = id * 8 + 13
            local k = id * 14 + 23
            return a + b + k + c + e + g
        end,
    },
    {
        name = 'wide_scan_nohint',
        description = 'Wide sparse range aggregation without scan hints',
        sql = WIDE_SCAN_SQL,
        prepare_iterations = env_int_compat('BENCH_PREPARE_ITERS_WIDE_SCAN',
                                            'BENCH_PREPARE_ITERS_AGG', 500),
        exec_iterations = env_int_compat('BENCH_EXEC_ITERS_WIDE_SCAN',
                                         'BENCH_EXEC_ITERS_AGG', 5000),
        auto_iterations = env_int_compat('BENCH_AUTO_ITERS_WIDE_SCAN',
                                         'BENCH_AUTO_ITERS_AGG', 5000),
        setup = setup_wide_scan_nohint,
        teardown = teardown_wide_scan,
        args = function(i)
            local start_id = ((i - 1) % 1024) + 1
            return {start_id, math.min(start_id + 127, 1024)}
        end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function(i)
            local start_id = ((i - 1) % 1024) + 1
            return wide_scan_expected[start_id][1]
        end,
    },
    {
        name = 'wide_scan_hint',
        description = 'Wide sparse range aggregation with hints for middle/right fields',
        sql = WIDE_SCAN_SQL,
        prepare_iterations = env_int_compat('BENCH_PREPARE_ITERS_WIDE_SCAN',
                                            'BENCH_PREPARE_ITERS_AGG', 500),
        exec_iterations = env_int_compat('BENCH_EXEC_ITERS_WIDE_SCAN',
                                         'BENCH_EXEC_ITERS_AGG', 5000),
        auto_iterations = env_int_compat('BENCH_AUTO_ITERS_WIDE_SCAN',
                                         'BENCH_AUTO_ITERS_AGG', 5000),
        setup = setup_wide_scan_hint,
        teardown = teardown_wide_scan,
        args = function(i)
            local start_id = ((i - 1) % 1024) + 1
            return {start_id, math.min(start_id + 127, 1024)}
        end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function(i)
            local start_id = ((i - 1) % 1024) + 1
            return wide_scan_expected[start_id][1]
        end,
    },
    {
        name = 'builtin_scan',
        description = 'Indexed range text builtin scan with aggregation',
        sql = [[
            SELECT sum(length(substr(s3, 2, 5)) + length(upper(s4)) +
                       abs(n3 - n4) + length(lower(s5)) +
                       length(substr(s6, 3, 6)) + abs(n1 - n2))
            FROM bench_text
            WHERE id BETWEEN ? AND ?;
        ]],
        prepare_iterations = env_int('BENCH_PREPARE_ITERS_BUILTIN', 300),
        exec_iterations = env_int('BENCH_EXEC_ITERS_BUILTIN', 3000),
        auto_iterations = env_int('BENCH_AUTO_ITERS_BUILTIN', 3000),
        setup = setup_builtin_scan,
        teardown = teardown_builtin_scan,
        args = function(i)
            local start_id = ((i - 1) % 2048) + 1
            return {start_id, math.min(start_id + 127, 2048)}
        end,
        checksum = function(res) return res.rows[1][1] end,
        expected = function(i)
            local start_id = ((i - 1) % 2048) + 1
            return builtin_scan_expected[start_id]
        end,
    },
    {
        name = 'sort_window',
        description = 'Indexed range top-K sort with computed keys',
        sql = [[
            SELECT sum(score), sum(src_id)
            FROM (
                SELECT ((a * 17 + b * 7 - c * 3) % 257) AS score,
                       id AS src_id
                FROM bench_arith
                WHERE id BETWEEN ? AND ?
                ORDER BY score DESC, b ASC, id DESC
                LIMIT 32
            );
        ]],
        prepare_iterations = env_int('BENCH_PREPARE_ITERS_SORT', 200),
        exec_iterations = env_int('BENCH_EXEC_ITERS_SORT', 2000),
        auto_iterations = env_int('BENCH_AUTO_ITERS_SORT', 2000),
        setup = setup_sort_window,
        teardown = teardown_sort_window,
        args = function(i)
            local start_id = ((i - 1) % 1024) + 1
            return {start_id, math.min(start_id + 255, 1024)}
        end,
        checksum = function(res)
            return res.rows[1][1] + res.rows[1][2]
        end,
        expected = function(i)
            local start_id = ((i - 1) % 1024) + 1
            return sort_window_expected[start_id][1]
        end,
    },
}

local results = {
    metadata = {
        tarantool_version = _TARANTOOL,
        jit_env = os.getenv('SQL_JIT_ENABLE') or 'unset',
        dispatcher_env = os.getenv('VDBE_DISPATCHER') or 'default',
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
                    if DISCARD_RESULTS then
                        assert(execute_sql_no_result(stmt.stmt_id,
                                                     workload.args(i)))
                    else
                        local res = execute_sql(stmt.stmt_id, workload.args(i))
                        checksum = checksum + workload.checksum(res)
                    end
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
                    if DISCARD_RESULTS then
                        assert(execute_sql_no_result(workload.sql,
                                                     workload.args(i)))
                    else
                        local res = execute_sql(workload.sql, workload.args(i))
                        checksum = checksum + workload.checksum(res)
                    end
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

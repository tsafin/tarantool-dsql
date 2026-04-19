local server = require('luatest.server')
local t = require('luatest')

local g = t.group()
local g_jit = t.group('sql_jit')

local function opcode_count(profile, ...)
    for i = 1, select('#', ...) do
        local name = select(i, ...)
        local value = profile[name]
        if value ~= nil then
            return value
        end
    end
    return 0
end

g.before_all(function()
    g.server = server:new({alias = 'sql_stats'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g_jit.before_all(function()
    g_jit.server = server:new({
        alias = 'sql_stats_jit',
        env = {
            SQL_JIT_ENABLE = '1',
            VDBE_DISPATCHER = 'generated',
        },
    })
    g_jit.server:start()
end)

g_jit.after_all(function()
    g_jit.server:stop()
end)

g.test_sql_stats_shape_and_growth = function()
    g.server:exec(function()
        local function sum_values(map)
            local total = 0
            for _, value in pairs(map) do
                total = total + value
            end
            return total
        end

        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        t.assert_type(before.sql_interpreter_step_count, 'number')
        t.assert_type(before.sql_jit_step_count, 'number')
        t.assert_type(before.sql_jit_compile_count, 'number')
        t.assert_type(before.sql_jit_compile_success_count, 'number')
        t.assert_type(before.sql_jit_exec_count, 'number')
        t.assert_type(before.sql_jit_full_run_count, 'number')
        t.assert_type(before.sql_jit_fallback_count, 'number')
        t.assert_type(before.sql_jit_resume_skip_count, 'number')
        t.assert_type(before.sql_jit_guard_skip_count, 'number')
        t.assert_type(before.sql_opcode_profile_enabled, 'number')
        if before.sql_opcode_profile_enabled ~= 0 then
            t.assert_type(before.interpreter_opcode_profile, 'table')
            t.assert_type(before.interpreter_opcode_profile.count, 'table')
            t.assert_type(before.interpreter_opcode_profile.time_us, 'table')
            t.assert_type(before.jit_opcode_profile, 'table')
            t.assert_type(before.jit_opcode_profile.count, 'table')
            t.assert_type(before.jit_opcode_profile.time_us, 'table')
        end

        t.assert_equals(box.execute([[SELECT a + 1 FROM t WHERE id = 2;]]).rows,
                        {{21}})

        local after = box.stat.sql()
        t.assert_gt(after.sql_interpreter_step_count,
                    before.sql_interpreter_step_count)
        t.assert_ge(after.sql_jit_step_count, before.sql_jit_step_count)
        t.assert_ge(after.sql_jit_compile_count, before.sql_jit_compile_count)
        if before.sql_opcode_profile_enabled ~= 0 then
            t.assert_gt(sum_values(after.interpreter_opcode_profile.count),
                        sum_values(before.interpreter_opcode_profile.count))
        end

        box.execute([[DROP TABLE t;]])
    end)
end

g_jit.test_sql_jit_exec_count_growth = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local result = box.execute([[SELECT 1 + 2 + 3 + 4 + 5;]])
        local after = box.stat.sql()
        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_compile_success = after.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            before_fallback = before.sql_jit_fallback_count,
            after_fallback = after.sql_jit_fallback_count,
        }
    end)

    t.assert_equals(res.rows, {{15}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_compile_success, res.before_compile_success)
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    t.assert_ge(res.after_fallback, res.before_fallback)
end

g_jit.test_sql_jit_session_setting = function()
    local res = g_jit.server:exec(function()
        local settings = box.space._session_settings
        local before_value = settings:get('sql_jit').value
        box.execute([[SET SESSION "sql_jit" = false;]])
        local disabled_value = settings:get('sql_jit').value
        local before_disabled = box.stat.sql()
        local disabled = box.execute([[SELECT 1 + 2 + 3 + 4 + 5;]])
        local after_disabled = box.stat.sql()
        box.execute([[SET SESSION "sql_jit" = true;]])
        local restored_value = settings:get('sql_jit').value
        return {
            before_value = before_value,
            disabled_value = disabled_value,
            restored_value = restored_value,
            rows = disabled.rows,
            before_compile = before_disabled.sql_jit_compile_count,
            after_disabled_compile = after_disabled.sql_jit_compile_count,
            before_exec = before_disabled.sql_jit_exec_count,
            after_disabled_exec = after_disabled.sql_jit_exec_count,
        }
    end)

    t.assert_equals(res.before_value, true)
    t.assert_equals(res.disabled_value, false)
    t.assert_equals(res.restored_value, true)
    t.assert_equals(res.rows, {{15}})
    t.assert_equals(res.after_disabled_compile, res.before_compile)
    t.assert_equals(res.after_disabled_exec, res.before_exec)
end

g_jit.test_sql_jit_row_shape_negative_cache = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local first = box.execute([[SELECT 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9;]])
        local after_first = box.stat.sql()
        local second = box.execute([[SELECT 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1;]])
        local after_second = box.stat.sql()
        return {
            first = first.rows,
            second = second.rows,
            before_compile = before.sql_jit_compile_count,
            after_first_compile = after_first.sql_jit_compile_count,
            after_second_compile = after_second.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_first_compile_success = after_first.sql_jit_compile_success_count,
            after_second_compile_success = after_second.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_first_exec = after_first.sql_jit_exec_count,
            after_second_exec = after_second.sql_jit_exec_count,
        }
    end)

    t.assert_equals(res.first, {{45}})
    t.assert_equals(res.second, {{45}})
    if res.after_first_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_first_compile_success, res.before_compile_success)
    t.assert_gt(res.after_first_exec, res.before_exec)
    t.assert_equals(res.after_second_compile, res.after_first_compile)
    t.assert_equals(res.after_second_compile_success,
                    res.after_first_compile_success)
    t.assert_equals(res.after_second_exec, res.after_first_exec)
end

g_jit.test_sql_jit_prepare_forces_small_statement = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local stmt = box.prepare([[SELECT 1 + 2;]])
        local after_prepare = box.stat.sql()
        local result = box.execute(stmt.stmt_id)
        local after_execute = box.stat.sql()
        box.unprepare(stmt.stmt_id)
        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_prepare_compile = after_prepare.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_prepare_compile_success = after_prepare.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_prepare_exec = after_prepare.sql_jit_exec_count,
            after_execute_exec = after_execute.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_execute_steps = after_execute.sql_jit_step_count,
        }
    end)

    t.assert_equals(res.rows, {{3}})
    if res.after_prepare_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_prepare_compile_success, res.before_compile_success)
    t.assert_equals(res.after_prepare_exec, res.before_exec)
    t.assert_gt(res.after_execute_exec, res.after_prepare_exec)
    t.assert_gt(res.after_execute_steps, res.before_steps)
end

g_jit.test_sql_jit_control_flow_opcodes = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local if_stmt = box.prepare([[
            SELECT CASE WHEN NOT ? THEN 10 ELSE 20 END + 1 + 2 + 3;
        ]])
        local ifnot_stmt = box.prepare([[
            SELECT CASE WHEN ? THEN 10 ELSE 20 END + 1 + 2 + 3;
        ]])
        local is_null_stmt = box.prepare([[
            SELECT 1 WHERE ? IS NOT NULL AND 1 + 2 + 3 + 4 = 10;
        ]])
        local not_null_stmt = box.prepare([[
            SELECT 1 WHERE ? IS NULL AND 1 + 2 + 3 + 4 = 10;
        ]])
        local results = {
            if_op = box.execute(if_stmt.stmt_id, {false}).rows,
            ifnot_op = box.execute(ifnot_stmt.stmt_id, {true}).rows,
            is_null_op = box.execute(is_null_stmt.stmt_id, {1}).rows,
            not_null_op = box.execute(not_null_stmt.stmt_id, {box.NULL}).rows,
        }
        local after = box.stat.sql()
        box.unprepare(if_stmt.stmt_id)
        box.unprepare(ifnot_stmt.stmt_id)
        box.unprepare(is_null_stmt.stmt_id)
        box.unprepare(not_null_stmt.stmt_id)
        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end
        return {
            results = results,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.results.if_op, {{16}})
    t.assert_equals(res.results.ifnot_op, {{16}})
    t.assert_equals(res.results.is_null_op, {{1}})
    t.assert_equals(res.results.not_null_op, {{1}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    if res.profile ~= nil then
        local before = res.profile.before
        local after = res.profile.after
        local before_total =
            opcode_count(before, 'If') +
            opcode_count(before, 'IfNot') +
            opcode_count(before, 'IsNull', 'NotUsed_174') +
            opcode_count(before, 'NotNull', 'NotUsed_175')
        local after_total =
            opcode_count(after, 'If') +
            opcode_count(after, 'IfNot') +
            opcode_count(after, 'IsNull', 'NotUsed_174') +
            opcode_count(after, 'NotNull', 'NotUsed_175')
        t.assert_gt(after_total, before_total)
    end
end

g_jit.test_sql_jit_column_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT);]])
        box.execute([[INSERT INTO t VALUES (2, 20, 200);]])

        local before = box.stat.sql()
        local result = box.execute([[SELECT a + b + id + id FROM t;]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{224}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.Column or 0, res.profile.before.Column or 0)
    end
end

g_jit.test_sql_jit_materialization_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE dst (id INT PRIMARY KEY, a INT, b INT);]])
        box.execute([[INSERT INTO dst VALUES (1, 10, 100);]])

        local before = box.stat.sql()
        box.execute([[UPDATE dst SET a = a + 1;]])
        local after = box.stat.sql()
        local updated = box.execute([[SELECT id, a, b FROM dst;]]).rows

        box.execute([[DROP TABLE dst;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            updated = updated,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.updated, {{1, 11, 100}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.ApplyType or 0,
                    res.profile.before.ApplyType or 0)
        t.assert_gt(res.profile.after.MakeRecord or 0,
                    res.profile.before.MakeRecord or 0)
        t.assert_gt(res.profile.after.RowData or 0,
                    res.profile.before.RowData or 0)
    end
end

g_jit.test_sql_jit_aggregate_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE src (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO src VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        local result = box.execute([[SELECT sum(a + id + 1) FROM src;]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE src;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{69}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.AggStep or 0, res.profile.before.AggStep or 0)
        t.assert_gt(res.profile.after.AggFinal or 0, res.profile.before.AggFinal or 0)
    end
end

g_jit.test_sql_jit_control_flow_round2_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40);]])

        local before = box.stat.sql()
        local offset_stmt = box.prepare([[
            SELECT 100 + 1
            WHERE EXISTS(SELECT id + a + 1 FROM t LIMIT 2 OFFSET 1);
        ]])
        local once_stmt = box.prepare([[
            SELECT 100 + 1
            WHERE EXISTS(SELECT (SELECT 40 + 2) + id FROM t LIMIT 1);
        ]])
        local offset_result = box.execute(offset_stmt.stmt_id).rows
        local once_result = box.execute(once_stmt.stmt_id).rows
        local after = box.stat.sql()
        box.unprepare(offset_stmt.stmt_id)
        box.unprepare(once_stmt.stmt_id)

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            offset_result = offset_result,
            once_result = once_result,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.offset_result, {{101}})
    t.assert_equals(res.once_result, {{101}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.MustBeInt or 0,
                    res.profile.before.MustBeInt or 0)
        t.assert_gt(res.profile.after.OffsetLimit or 0,
                    res.profile.before.OffsetLimit or 0)
        t.assert_gt(res.profile.after.IfPos or 0,
                    res.profile.before.IfPos or 0)
        t.assert_gt(res.profile.after.Once or 0,
                    res.profile.before.Once or 0)
        t.assert_gt(res.profile.after.DecrJumpZero or 0,
                    res.profile.before.DecrJumpZero or 0)
    end
end

g_jit.test_sql_jit_cast_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a TEXT, b INT);]])
        box.execute([[INSERT INTO t VALUES
            (1, '10', 100),
            (2, '20', 200),
            (3, '30', 300);
        ]])

        local before = box.stat.sql()
        local result = box.execute([[
            SELECT CAST(a AS INTEGER) + b + id + id FROM t;
        ]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{112}, {224}, {336}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.Cast or 0, res.profile.before.Cast or 0)
    end
end

g_jit.test_sql_jit_coroutine_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        local stmt = box.prepare([[
            SELECT sum(x + 1)
            FROM (SELECT id + a + 1 AS x FROM t LIMIT 2);
        ]])
        local result = box.execute(stmt.stmt_id)
        local after = box.stat.sql()
        box.unprepare(stmt.stmt_id)

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{37}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.InitCoroutine or 0,
                    res.profile.before.InitCoroutine or 0)
        t.assert_gt(res.profile.after.Yield or 0,
                    res.profile.before.Yield or 0)
        t.assert_gt(res.profile.after.EndCoroutine or 0,
                    res.profile.before.EndCoroutine or 0)
    end
end

g_jit.test_sql_jit_sorter_gosub_return_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        local result = box.execute([[
            SELECT sum(x)
            FROM (
                SELECT id + a + 1 AS x FROM t
                UNION ALL
                SELECT id + a + 2 FROM t
                ORDER BY 1
            );
        ]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{141}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.Permutation or 0,
                    res.profile.before.Permutation or 0)
        t.assert_gt(res.profile.after.Compare or 0,
                    res.profile.before.Compare or 0)
        t.assert_gt(res.profile.after.SorterNext or 0,
                    res.profile.before.SorterNext or 0)
        t.assert_gt(res.profile.after.Gosub or 0,
                    res.profile.before.Gosub or 0)
        t.assert_gt(res.profile.after.Return or 0,
                    res.profile.before.Return or 0)
    end
end

g_jit.test_sql_jit_ttransaction_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])

        local before = box.stat.sql()
        local insert_result = box.execute([[INSERT INTO t VALUES (1, 10);]])
        local after = box.stat.sql()
        local rows = box.execute([[SELECT id, a FROM t;]]).rows

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            row_count = insert_result.row_count,
            rows = rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.row_count, 1)
    t.assert_equals(res.rows, {{1, 10}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.TTransaction or 0,
                    res.profile.before.TTransaction or 0)
    end
end

g_jit.test_sql_jit_program_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[CREATE TABLE t1(x INTEGER PRIMARY KEY);]])
        box.execute([[CREATE TABLE t2(y INTEGER PRIMARY KEY);]])
        box.execute([[
            CREATE TRIGGER tr AFTER INSERT ON t1 FOR EACH ROW
            BEGIN
                INSERT INTO t2 VALUES(new.x + 1);
            END;
        ]])

        local before = box.stat.sql()
        local stmt = box.prepare([[INSERT INTO t1 VALUES(10);]])
        local after_prepare = box.stat.sql()
        box.execute(stmt.stmt_id)
        local after = box.stat.sql()
        local rows = box.execute([[SELECT y FROM t2 WHERE y = 11;]]).rows
        box.unprepare(stmt.stmt_id)

        box.execute([[DROP TABLE t1;]])
        box.execute([[DROP TABLE t2;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            before_compile = before.sql_jit_compile_count,
            after_compile = after_prepare.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_compile_success = after_prepare.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            rows = rows,
            profile = profile,
        }
    end)

    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_equals(res.rows, {{11}})
    t.assert_equals(res.after_compile_success, res.before_compile_success)
    t.assert_equals(res.after_exec, res.before_exec)
    t.assert_equals(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_equals(res.profile.after.Program or 0,
                        res.profile.before.Program or 0)
        t.assert_equals(res.profile.after.Param or 0,
                        res.profile.before.Param or 0)
    end
end

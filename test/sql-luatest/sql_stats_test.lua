local server = require('luatest.server')
local t = require('luatest')

local g = t.group()
local g_jit = t.group('sql_jit')

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
    t.assert_gt(res.after_fallback, res.before_fallback)
end

g_jit.test_sql_jit_control_flow_opcodes = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local results = {
            if_ifnot = box.execute([[SELECT 1 WHERE (1, 2) != (1, 3);]]).rows,
            not_null = box.execute([[
                SELECT 1 WHERE NULL IS NULL AND 1 + 2 + 3 + 4 = 10;
            ]]).rows,
            is_null = box.execute([[
                SELECT 1 WHERE 1 + 2 + 3 + 4 IS NULL OR 1 = 1;
            ]]).rows,
        }
        local after = box.stat.sql()
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

    t.assert_equals(res.results.if_ifnot, {{1}})
    t.assert_equals(res.results.not_null, {{1}})
    t.assert_equals(res.results.is_null, {{1}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    if res.profile ~= nil then
        local before = res.profile.before
        local after = res.profile.after
        t.assert_gt(after.If or 0, before.If or 0)
        t.assert_gt(after.IfNot or 0, before.IfNot or 0)
        t.assert_gt(after.NotNull or 0, before.NotNull or 0)
        t.assert_gt(after.IsNull or 0, before.IsNull or 0)
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

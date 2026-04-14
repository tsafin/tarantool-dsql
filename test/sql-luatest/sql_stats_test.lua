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
        local result = box.execute([[SELECT 1 + 2;]])
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

    t.assert_equals(res.rows, {{3}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_compile_success, res.before_compile_success)
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    t.assert_gt(res.after_fallback, res.before_fallback)
end

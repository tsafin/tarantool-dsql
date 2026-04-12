local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'sql_stats'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
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

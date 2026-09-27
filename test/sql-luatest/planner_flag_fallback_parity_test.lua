local server = require('luatest.server')
local t = require('luatest')

local g = t.group('sql_new_planner_fallback_parity')

g.before_all(function()
    g.server = server:new({alias = 'm37_fallback_parity'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_unsupported_routes_preserve_rows_and_reasons = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        local queries = {
            {
                sql = [[SELECT abs(v) FROM planner_fallback_parity
                        WHERE id > 0]],
                reason = 'UNSUPPORTED_FUNCTION',
            },
            {
                sql = [[SELECT id FROM planner_fallback_parity NOT INDEXED
                        WHERE id > 0]],
                reason = 'UNSUPPORTED_ACCESS_HINT',
            },
        }

        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_fallback_parity_' .. engine
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, v INTEGER) ' ..
                         "WITH ENGINE = '%s'"):format(name, engine))
            box.execute(('INSERT INTO %s VALUES (1, -7), (2, 0), (3, 9)')
                        :format(name))
            for _, query in ipairs(queries) do
                local sql = query.sql:gsub('planner_fallback_parity', name)
                local function run(flag)
                    box.execute(('SET SESSION "sql_new_planner_single_table" = %s')
                                :format(flag and 'true' or 'false'))
                    local explain, explain_err = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. sql)
                    t.assert(explain_err == nil and explain ~= nil,
                             ('EXPLAIN failed on %s: %s')
                             :format(engine, tostring(explain_err)))
                    t.assert_equals(explain.rows[1][3], 'fallback')
                    t.assert_equals(explain.rows[2][3], query.reason)
                    local rows, execute_err = box.execute(sql)
                    t.assert(execute_err == nil and rows ~= nil,
                             ('query failed on %s: %s')
                             :format(engine, tostring(execute_err)))
                    return rows.rows
                end

                local counter = 'sql_planner_fallback_' .. query.reason ..
                    '_total'
                local before = box.stat.sql()[counter]
                local disabled = run(false)
                local after_disabled = box.stat.sql()[counter]
                t.assert_equals(after_disabled, before + 2,
                                'off EXPLAIN and execution each count fallback')

                local enabled_before = box.stat.sql()[counter]
                local enabled = run(true)
                local enabled_after = box.stat.sql()[counter]
                t.assert_equals(enabled_after, enabled_before + 2,
                                'on EXPLAIN and execution each count fallback')
                t.assert_equals(enabled, disabled,
                                ('enabled rows differ on %s'):format(engine))

                local disabled_again = run(false)
                t.assert_equals(disabled_again, disabled,
                                ('second disabled rows differ on %s')
                                :format(engine))
                box.execute(('SET SESSION "sql_new_planner_single_table" = false'))
            end
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

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
        -- The capture adapter executes one internal snapshot EXPLAIN after
        -- each successful SELECT. Its fallback counter increment is global,
        -- so include that observer-only increment in captured runs.
        local capture_counter_adjustment =
            os.getenv('SQL_BASELINE_OUT') ~= nil and 1 or 0
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
            {
                sql = [[SELECT COUNT(*) FROM planner_fallback_parity
                        WHERE v + 1 > 1]],
                off_reason = 'UNSUPPORTED_AGGREGATE',
                on_reason = 'UNSUPPORTED_FILTER',
            },
            {
                sql = [[SELECT * FROM planner_fallback_parity
                        WHERE a = X'123456']],
                off_route = 'current_where_c',
                on_route = 'new_planner',
            },
            {
                sql = [[SELECT id FROM planner_fallback_parity
                        WHERE v + 1 > 1]],
                off_route = 'current_where_c',
                on_route = 'new_planner',
            },
            {
                sql = [[SELECT id FROM planner_fallback_parity
                        WHERE v = NULL]],
                off_route = 'current_where_c',
                on_route = 'fallback',
                on_reason = 'NO_ACCESS_PATH',
            },
        }

        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_fallback_parity_' .. engine
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, v INTEGER, ' ..
                         "a SCALAR) WITH ENGINE = '%s'"):format(name, engine))
            box.execute(("INSERT INTO %s VALUES (1, -7, X'123456'), " ..
                         "(2, 0, X'CDEF12'), (3, 9, X'7890AB')"):format(name))
            box.execute(('CREATE INDEX %s_v_idx ON %s (v)')
                        :format(name, name))
            for _, query in ipairs(queries) do
                local sql = query.sql:gsub('planner_fallback_parity', name)
                local function reason_for(flag)
                    return flag and (query.on_reason or query.reason) or
                        (query.off_reason or query.reason)
                end
                local function route_for(flag)
                    return flag and (query.on_route or 'fallback') or
                        (query.off_route or 'fallback')
                end
                local function counter_value(counter)
                    local stats = box.stat.sql()
                    if counter == nil then
                        return stats.sql_planner_fallback_total
                    end
                    return stats[counter]
                end
                local function run(flag)
                    box.execute(('SET SESSION "sql_new_planner_single_table" = %s')
                                :format(flag and 'true' or 'false'))
                    local explain, explain_err = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. sql)
                    t.assert(explain_err == nil and explain ~= nil,
                             ('EXPLAIN failed on %s: %s')
                             :format(engine, tostring(explain_err)))
                    t.assert_equals(explain.rows[1][3], route_for(flag),
                            ('unexpected route for %s: %s')
                            :format(sql, tostring(explain.rows[1][3])))
                    t.assert_equals(explain.rows[2][3], reason_for(flag))
                    local rows, execute_err = box.execute(sql)
                    t.assert(execute_err == nil and rows ~= nil,
                             ('query failed on %s: %s')
                             :format(engine, tostring(execute_err)))
                    return rows.rows
                end

                local off_counter = reason_for(false) ~= nil and
                    'sql_planner_fallback_' .. reason_for(false) .. '_total'
                    or nil
                local before = counter_value(off_counter)
                local disabled = run(false)
                local after_disabled = counter_value(off_counter)
                t.assert_equals(after_disabled, before +
                    (off_counter ~= nil and 2 + capture_counter_adjustment or 0),
                    'off EXPLAIN and execution fallback accounting')

                local on_counter = reason_for(true) ~= nil and
                    'sql_planner_fallback_' .. reason_for(true) .. '_total'
                    or nil
                local enabled_before = counter_value(on_counter)
                local enabled = run(true)
                local enabled_after = counter_value(on_counter)
                t.assert_equals(enabled_after, enabled_before +
                    (on_counter ~= nil and 2 + capture_counter_adjustment or 0),
                    'on EXPLAIN and execution fallback accounting')
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

local server = require('luatest.server')
local t = require('luatest')

local g = t.group('planner_scalar_filter')

g.before_all(function()
    g.server = server:new({alias = 'planner_scalar_filter'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_non_primary_null_filters_off_on_off = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_null_filter_' .. engine
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, v STRING) ' ..
                         "WITH ENGINE = '%s'"):format(name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(1, NULL), (2, 'x'), (3, NULL), (4, 'y')")
                        :format(name))
            local queries = {
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{1}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NOT NULL ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}, {4}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL AND id > 1 ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NOT NULL AND ' ..
                           'id < 4 ORDER BY id DESC'):format(name),
                    expected = {{2}},
                },
            }
            local function capture(enabled)
                local results = {}
                box.execute(('SET SESSION "sql_new_planner_single_table" = %s')
                            :format(enabled and 'true' or 'false'))
                for i, query in ipairs(queries) do
                    local explain, err = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. query.sql)
                    t.assert(err == nil, err and err.message)
                    local route = explain.rows[1][3]
                    if enabled then
                        t.assert_equals(route, 'new_planner',
                                        ('query %d route on %s: %s / %s')
                                        :format(i, engine, route,
                                                tostring(explain.rows[1][4])))
                    else
                        t.assert(route == 'current_where_c' or
                                 route == 'fallback')
                    end
                    local result
                    result, err = box.execute(query.sql)
                    t.assert(err == nil, err and err.message)
                    results[i] = result.rows
                    t.assert_equals(results[i], query.expected,
                                    ('query %d result on %s'):format(i, engine))
                end
                return results
            end
            local off = capture(false)
            local on = capture(true)
            t.assert_equals(on, off)
            local off_again = capture(false)
            t.assert_equals(off_again, off)
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

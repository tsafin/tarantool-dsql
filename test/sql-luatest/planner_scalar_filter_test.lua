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
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, v STRING, ' ..
                         'w STRING, s SCALAR) ' ..
                         "WITH ENGINE = '%s'"):format(name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(1, NULL, 'a', true), (2, 'x', NULL, false), " ..
                         "(3, NULL, NULL, NULL), (4, 'y', 'z', true)")
                        :format(name))
            local comparison_name = name .. '_comparison'
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, ' ..
                         'a INTEGER, b INTEGER, s STRING, t STRING) ' ..
                         "WITH ENGINE = '%s'")
                        :format(comparison_name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(1, 1, 1, 'a', 'a'), (2, 1, 2, 'a', 'b'), " ..
                         "(3, 2, 1, 'b', 'a'), (4, NULL, 1, NULL, 'a'), " ..
                         "(5, 1, NULL, 'a', NULL)"):format(comparison_name))
            local composite_name = name .. '_composite'
            box.execute(('CREATE TABLE %s (a INTEGER, b INTEGER, v STRING, ' ..
                         'w STRING, PRIMARY KEY (a, b)) ' ..
                         "WITH ENGINE = '%s'"):format(composite_name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(1, 10, NULL, 'a'), (1, 11, 'x', NULL), " ..
                         '(2, 20, NULL, NULL)'):format(composite_name))
            local queries = {
                {
                    sql = ('SELECT a, b FROM %s WHERE a = 1 AND b = 10 ' ..
                           'AND v IS NULL AND w IS NOT NULL')
                          :format(composite_name),
                    expected = {{1, 10}},
                },
                {
                    sql = ('SELECT a, b FROM %s WHERE b = 11 AND ' ..
                           'a = 1 AND v IS NULL AND w IS NOT NULL')
                          :format(composite_name),
                    expected = {},
                },
                {
                    sql = ('SELECT a, b FROM %s WHERE a IS NOT NULL AND ' ..
                           'a = 1 AND b = 10 AND v IS NULL')
                          :format(composite_name),
                    expected = {{1, 10}},
                },
                {
                    sql = ('SELECT a, b FROM %s WHERE a IS NULL AND ' ..
                           'a = 1 AND b = 10'):format(composite_name),
                    expected = {},
                    enabled_route = 'fallback',
                    enabled_reason = 'UNSUPPORTED_FILTER',
                },
                {
                    sql = ('SELECT a, b FROM %s WHERE a IS NOT NULL AND ' ..
                           'b IS NOT NULL ORDER BY a, b'):format(composite_name),
                    expected = {{1, 10}, {1, 11}, {2, 20}},
                },
                {
                    sql = ("SELECT a, b FROM %s WHERE a IS NULL AND v = 'x'")
                          :format(composite_name),
                    expected = {},
                    enabled_route = 'fallback',
                    enabled_reason = 'UNSUPPORTED_FILTER',
                },
                {
                    sql = ('SELECT a, b FROM %s WHERE a = 1 AND b > 0 ' ..
                           'AND v IS NULL AND w IS NOT NULL')
                          :format(composite_name),
                    expected = {{1, 10}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{1}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL AND ' ..
                           'w IS NOT NULL ORDER BY id ASC'):format(name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NOT NULL ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}, {4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v = 'x' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v = 'x' || '' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE 'x' || '' = v " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v BETWEEN 'y' AND 'z' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v NOT BETWEEN 'y' " ..
                           "AND 'z' ORDER BY id ASC"):format(name),
                    expected = {{2}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v IN ('x', 'y') " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}, {4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE w NOT IN ('a', 'b') " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v IN ('x', NULL) " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v IN ('x') OR " ..
                           "w IN ('z') ORDER BY id ASC"):format(name),
                    expected = {{2}, {4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v = 'x' OR w = 'z' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}, {4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE NOT (v = 'x') " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE NOT (v = 'x' OR " ..
                           "w = 'a') ORDER BY id ASC"):format(name),
                    expected = {{4}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL OR w = \'z\' ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{1}, {3}, {4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE (v = 'x' OR w = 'z') " ..
                           'AND id > 1 ORDER BY id ASC'):format(name),
                    expected = {{2}, {4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE id = 1 OR v = 'x' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{1}, {2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE s = true ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{1}, {4}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE s = false ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v = NULL')
                          :format(name),
                    expected = {},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v <> 'x' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE v > 'x' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE 'x' < v " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{4}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE id > 1 AND v = 'x' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE id > 2 AND v = 'x' " ..
                           'ORDER BY id ASC'):format(name),
                    expected = {},
                },
                {
                    sql = ("SELECT id FROM %s WHERE id = 2 AND v = 'x'")
                          :format(name),
                    expected = {{2}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE id = 2 AND v = 'y'")
                          :format(name),
                    expected = {},
                },
                {
                    sql = ("SELECT a, b FROM %s WHERE a = 1 AND b = 11 " ..
                           "AND v = 'x'"):format(composite_name),
                    expected = {{1, 11}},
                },
                {
                    sql = ("SELECT a, b FROM %s WHERE a = 1 AND b > 10 " ..
                           "AND v = 'x' ORDER BY a, b")
                          :format(composite_name),
                    expected = {{1, 11}},
                },
                {
                    sql = ("SELECT a, b FROM %s WHERE a = 1 AND v = 'x' " ..
                           'ORDER BY a, b'):format(composite_name),
                    expected = {{1, 11}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL AND id > 1 ' ..
                           'ORDER BY id ASC'):format(name),
                    expected = {{3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 1 AND ' ..
                           'v IS NULL'):format(name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 2 AND ' ..
                           'v IS NULL'):format(name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 2 AND ' ..
                           'v IS NOT NULL'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 2 AND ' ..
                           'v IS NOT NULL LIMIT 1 OFFSET 1'):format(name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 1 AND ' ..
                           'v IS NULL AND w IS NOT NULL'):format(name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE w IS NOT NULL AND ' ..
                           'id = 2 AND v IS NOT NULL'):format(name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 99 AND ' ..
                           'v IS NULL AND w IS NOT NULL'):format(name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id > 0 AND ' ..
                           'v IS NULL AND w IS NOT NULL'):format(name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id > 0 AND id < 4 ' ..
                           'AND v IS NULL AND w IS NOT NULL'):format(name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL AND ' ..
                           'w IS NULL AND id > 0 AND id < 4'):format(name),
                    expected = {{3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL AND ' ..
                           'v IS NOT NULL'):format(name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NOT NULL AND ' ..
                           'id < 4 ORDER BY id DESC'):format(name),
                    expected = {{2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL AND id > 0 ' ..
                           'ORDER BY id ASC LIMIT 1 OFFSET 1'):format(name),
                    expected = {{3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE v IS NULL AND ' ..
                           'v IS NOT NULL'):format(name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a = b ORDER BY id')
                          :format(comparison_name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a <> b ORDER BY id')
                          :format(comparison_name),
                    expected = {{2}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a < b ORDER BY id')
                          :format(comparison_name),
                    expected = {{2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE t > s ORDER BY id')
                          :format(comparison_name),
                    expected = {{2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a = b OR s = t ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 2 AND a < b')
                          :format(comparison_name),
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
                        t.assert_equals(route, query.enabled_route or
                                        'new_planner',
                                        ('query %d route on %s: %s / %s')
                                        :format(i, engine, route,
                                                tostring(explain.rows[2][3])))
                        if query.enabled_reason ~= nil then
                            t.assert_equals(explain.rows[2][3],
                                            query.enabled_reason)
                        end
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
            box.execute(('DROP TABLE %s'):format(composite_name))
            box.execute(('DROP TABLE %s'):format(comparison_name))
        end
    end)
end

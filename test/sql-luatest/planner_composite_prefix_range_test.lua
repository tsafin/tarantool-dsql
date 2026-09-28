local server = require('luatest.server')
local t = require('luatest')

local g = t.group('planner_composite_prefix_range')

g.before_all(function()
    g.server = server:new({alias = 'planner_prefix_range'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_composite_prefix_equality_then_range_off_on_off = function()
    g.server:exec(function()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_prefix_range_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b UNSIGNED, c INTEGER, ' ..
                         'v STRING, n STRING, PRIMARY KEY (a, b, c)) ' ..
                         'WITH ENGINE = \'%s\'')
                        :format(name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         '(1, 10, 2, \'a\', \'x\'), (1, 20, 3, \'b\', NULL), ' ..
                         '(1, 20, 1, \'c\', \'x\'), (1, 30, 2, \'d\', NULL), ' ..
                         '(1, 40, 1, \'e\', NULL), (2, 20, 1, \'f\', NULL), ' ..
                         '(1, 9223372036854775808, 2, \'g\', NULL), ' ..
                         '(1, 18446744073709551615, 1, \'max\', NULL)')
                        :format(name))
            local queries = {
                ('SELECT v FROM %s WHERE a = 1 AND b >= 20 ' ..
                 'ORDER BY a ASC, b ASC, c ASC'):format(name),
                ('SELECT a, b, c, v FROM %s WHERE a = 1 AND b < 30 ' ..
                 'ORDER BY a ASC, b ASC, c ASC'):format(name),
                ('SELECT a, b, c, v FROM %s WHERE a = 1 AND b >= 20 ' ..
                 'AND b < 40 ORDER BY a ASC, b ASC, c ASC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND ' ..
                 'b > 9223372036854775807 ORDER BY a ASC, b ASC, c ASC')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 1 AND ' ..
                 '20 < b ORDER BY a ASC, b ASC, c ASC'):format(name),
                ('SELECT a, b, c, v FROM %s WHERE a = 1 AND b < 30 ' ..
                 'ORDER BY b DESC, c DESC'):format(name),
                ('SELECT a, b, c, v FROM %s WHERE a = 1 AND b >= 20 ' ..
                 'AND b < 40 ORDER BY b DESC, c DESC'):format(name),
                ('SELECT a, b, c, v FROM %s WHERE a = 1 AND b <= 20 ' ..
                 'ORDER BY b DESC, c DESC'):format(name),
                ('SELECT a, b, c, v FROM %s WHERE a = 1 AND b > 20 ' ..
                 'AND b <= 40 ORDER BY b DESC, c DESC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b >= 20 ' ..
                 'ORDER BY b DESC, c DESC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b > 20 ' ..
                 'ORDER BY b DESC, c DESC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND ' ..
                 'b >= 18446744073709551615 ORDER BY b DESC, c DESC')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 3 AND b >= 20 ' ..
                 'ORDER BY b DESC, c DESC'):format(name),
                ('SELECT a, b, c, v FROM %s WHERE a = 1 ' ..
                 'AND b BETWEEN 20 AND 40 ORDER BY b ASC, c ASC')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 1 ' ..
                 'AND b BETWEEN 20 AND 40 ORDER BY b DESC, c DESC')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 1 ' ..
                 'AND b BETWEEN 9223372036854775808 AND ' ..
                 '18446744073709551615 ORDER BY b ASC, c ASC')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b >= 20 AND b < 40 ' ..
                 'AND n IS NULL ORDER BY a ASC, b ASC, c ASC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b >= 20 AND b < 40 ' ..
                 'AND n IS NOT NULL ORDER BY b DESC, c DESC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b >= 10 AND b < 40 ' ..
                 'AND n IS NULL ORDER BY b ASC, c ASC LIMIT 1 OFFSET 1')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b = 20 AND n IS NULL ' ..
                 'ORDER BY c ASC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b = 20 ' ..
                 'ORDER BY c DESC'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b = 20 ' ..
                 'ORDER BY c DESC LIMIT 1 OFFSET 1'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b >= 10 AND b > 20 ' ..
                 'AND b <= 40 AND b < 50 ORDER BY b ASC, c ASC')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b >= 20 AND b > 20 ' ..
                 'AND b <= 40 AND b < 40 ORDER BY b DESC, c DESC')
                    :format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b > 50 AND b >= 40 ' ..
                 'AND b <= 40 ORDER BY b ASC, c ASC'):format(name),
            }
            local expected = {
                {{'c'}, {'b'}, {'d'}, {'e'}, {'g'}, {'max'}},
                {{1, 10, 2, 'a'}, {1, 20, 1, 'c'}, {1, 20, 3, 'b'}},
                {{1, 20, 1, 'c'}, {1, 20, 3, 'b'}, {1, 30, 2, 'd'}},
                {{'g'}, {'max'}},
                {{'d'}, {'e'}, {'g'}, {'max'}},
                {{1, 20, 3, 'b'}, {1, 20, 1, 'c'}, {1, 10, 2, 'a'}},
                {{1, 30, 2, 'd'}, {1, 20, 3, 'b'}, {1, 20, 1, 'c'}},
                {{1, 20, 3, 'b'}, {1, 20, 1, 'c'}, {1, 10, 2, 'a'}},
                {{1, 40, 1, 'e'}, {1, 30, 2, 'd'}},
                {{'max'}, {'g'}, {'e'}, {'d'}, {'b'}, {'c'}},
                {{'max'}, {'g'}, {'e'}, {'d'}},
                {{'max'}},
                {},
                {{1, 20, 1, 'c'}, {1, 20, 3, 'b'}, {1, 30, 2, 'd'},
                 {1, 40, 1, 'e'}},
                {{'e'}, {'d'}, {'b'}, {'c'}},
                {{'g'}, {'max'}},
                {{'b'}, {'d'}},
                {{'c'}},
                {{'d'}},
                {{'b'}},
                {{'b'}, {'c'}},
                {{'c'}},
                {{'d'}, {'e'}},
                {{'d'}},
                {},
            }
            local function capture(enabled)
                local rows = {}
                for i, sql in ipairs(queries) do
                    local explain, err = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. sql)
                    t.assert(err == nil, err and err.message)
                    local route = explain.rows[1][3]
                    if enabled then
                        t.assert_equals(route, 'new_planner',
                                        ('query %d missed suffix-range lowering on %s (%s)')
                                        :format(i, engine,
                                                tostring(explain.rows[2][3])))
                    else
                        t.assert(route == 'current_where_c' or
                                 route == 'fallback',
                                 ('unexpected disabled route %s for query %d on %s')
                                 :format(tostring(route), i, engine))
                    end
                    if not enabled and route == 'fallback' then
                        t.assert(type(explain.rows[2][3]) == 'string' and
                                 #explain.rows[2][3] > 0,
                                 ('missing fallback reason for query %d on %s')
                                 :format(i, engine))
                    end
                    local result
                    result, err = box.execute(sql)
                    t.assert(err == nil and result ~= nil,
                             ('query %d on %s: %s')
                             :format(i, engine, tostring(err)))
                    rows[i] = result.rows
                    t.assert_equals(rows[i], expected[i],
                                    ('query %d result on %s route=%s')
                                    :format(i, engine, tostring(route)))
                end
                return rows
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local off = capture(false)
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            local on = capture(true)
            t.assert_equals(on, off, 'enabled route changed results on ' .. engine)
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local off_again = capture(false)
            t.assert_equals(off_again, off,
                            'second disabled run changed results on ' .. engine)
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

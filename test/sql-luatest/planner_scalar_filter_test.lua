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
                         'a INTEGER, b INTEGER, s STRING, t STRING, ' ..
                         'k INTEGER, uk UNSIGNED) ' ..
                         "WITH ENGINE = '%s'")
                        :format(comparison_name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(1, 1, 1, 'a', 'a', 7, 10), " ..
                         "(2, 1, 2, 'a', 'b', 7, 10), " ..
                         "(3, 2, 1, 'b', 'a', 8, " ..
                         '18446744073709551615), ' ..
                         "(4, NULL, 1, NULL, 'a', NULL, NULL), " ..
                         "(5, 1, NULL, 'a', NULL, 7, 10)")
                        :format(comparison_name))
            local pattern_name = name .. '_pattern'
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, ' ..
                         's STRING) WITH ENGINE = \'%s\'')
                        :format(pattern_name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(1, 'a%%'), (2, 'a_'), (3, 'aX'), " ..
                         "(4, '100%%'), (5, NULL)")
                        :format(pattern_name))
            box.execute(('CREATE INDEX %s_k ON %s (k)')
                        :format(comparison_name, comparison_name))
            box.execute(('CREATE INDEX %s_uk ON %s (uk)')
                        :format(comparison_name, comparison_name))
            local unsigned_desc_name = name .. '_unsigned_desc'
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, ' ..
                         'uk UNSIGNED) WITH ENGINE = \'%s\'')
                        :format(unsigned_desc_name, engine))
            box.execute(('CREATE INDEX %s_uk ON %s (uk DESC)')
                        :format(unsigned_desc_name, unsigned_desc_name))
            box.execute(('INSERT INTO %s VALUES ' ..
                         '(1, 10), (2, 10), ' ..
                         '(3, 18446744073709551615), (4, NULL)')
                        :format(unsigned_desc_name))
            local composite_name = name .. '_composite'
            box.execute(('CREATE TABLE %s (a INTEGER, b INTEGER, v STRING, ' ..
                         'w STRING, PRIMARY KEY (a, b)) ' ..
                         "WITH ENGINE = '%s'"):format(composite_name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(1, 10, NULL, 'a'), (1, 11, 'x', NULL), " ..
                         '(2, 20, NULL, NULL)'):format(composite_name))
            local secondary_name = name .. '_secondary_composite'
            box.execute(('CREATE TABLE %s (tenant INTEGER, id INTEGER, ' ..
                         'x INTEGER, y UNSIGNED, note STRING, ' ..
                         'PRIMARY KEY (tenant, id)) WITH ENGINE = \'%s\'')
                        :format(secondary_name, engine))
            box.execute(('CREATE INDEX %s_xy ON %s (x, y)')
                        :format(secondary_name, secondary_name))
            box.execute(('CREATE INDEX %s_xydesc ON %s ' ..
                         '(x DESC, y DESC)')
                        :format(secondary_name, secondary_name))
            box.space[secondary_name]:create_index('xnotemixed', {
                parts = {{3, 'integer', sort_order = 'asc'},
                         {5, 'string', sort_order = 'desc'}},
            })
            box.execute(('INSERT INTO %s VALUES ' ..
                         '(1, 1, 7, 10, \'a\'), ' ..
                         '(1, 2, 7, 10, \'b\'), ' ..
                         '(2, 1, 7, 18446744073709551615, \'c\'), ' ..
                         '(2, 2, 8, 10, \'d\')'):format(secondary_name))
            local secondary_tertiary_name = name .. '_secondary_tertiary'
            box.execute(('CREATE TABLE %s (tenant INTEGER, id INTEGER, ' ..
                         'x INTEGER, y UNSIGNED, z INTEGER, ' ..
                         'PRIMARY KEY (tenant, id)) WITH ENGINE = \'%s\'')
                        :format(secondary_tertiary_name, engine))
            box.execute(('CREATE INDEX %s_xyz ON %s (x, y, z)')
                        :format(secondary_tertiary_name,
                                secondary_tertiary_name))
            box.execute(('INSERT INTO %s VALUES ' ..
                         '(1, 1, 7, 10, 1), (1, 2, 7, 10, 3), ' ..
                         '(2, 1, 7, 10, 2), (2, 2, 7, 11, 1), ' ..
                         '(3, 1, 8, 10, 4)')
                        :format(secondary_tertiary_name))
            local nullable_secondary_name = name .. '_secondary_nullable'
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, ' ..
                         'x INTEGER) WITH ENGINE = \'%s\'')
                        :format(nullable_secondary_name, engine))
            box.execute(('CREATE INDEX %s_x ON %s (x)')
                        :format(nullable_secondary_name,
                                nullable_secondary_name))
            box.execute(('INSERT INTO %s VALUES (1, 3), (2, NULL), (3, 7)')
                        :format(nullable_secondary_name))
            local descending_secondary_name = name .. '_secondary_desc'
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, ' ..
                         'x INTEGER, y INTEGER) WITH ENGINE = \'%s\'')
                        :format(descending_secondary_name, engine))
            box.execute(('CREATE INDEX %s_xy ON %s (x DESC, y DESC)')
                        :format(descending_secondary_name,
                                descending_secondary_name))
            box.execute(('INSERT INTO %s VALUES ' ..
                         '(1, 7, 10), (2, 7, 11), (3, 8, 10), ' ..
                         '(4, NULL, 12)')
                        :format(descending_secondary_name))
            local queries = {
                {
                    sql = ("SELECT id FROM %s WHERE s LIKE 'a!%%' " ..
                           "ESCAPE '!' ORDER BY id")
                          :format(pattern_name),
                    expected = {{1}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ("SELECT id FROM %s WHERE s NOT LIKE 'a!%%' " ..
                           "ESCAPE '!' ORDER BY id")
                          :format(pattern_name),
                    expected = {{2}, {3}, {4}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ("SELECT id FROM %s WHERE s LIKE '100!%%' " ..
                           "ESCAPE '!' ORDER BY id")
                          :format(pattern_name),
                    expected = {{4}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE y = 10 ' ..
                           'AND x = 7'):format(secondary_tertiary_name),
                    expected = {{1, 1}, {1, 2}, {2, 1}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    unordered = true,
                },
                {
                    sql = ('SELECT z, tenant, id FROM %s WHERE y = 10 ' ..
                           'AND x = 7 ORDER BY z DESC')
                          :format(secondary_tertiary_name),
                    expected = {{3, 1, 2}, {2, 2, 1}, {1, 1, 1}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    expected_order_column = 1,
                    expected_order_desc = true,
                },
                {
                    sql = ('SELECT y, z, tenant, id FROM %s WHERE x = 7 ' ..
                           'ORDER BY y, z'):format(secondary_tertiary_name),
                    expected = {{10, 1, 1, 1}, {10, 2, 2, 1},
                                {10, 3, 1, 2}, {11, 1, 2, 2}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    expected_order_columns = {1, 2},
                },
                {
                    sql = ('SELECT y, z, tenant, id FROM %s WHERE x = 7 ' ..
                           'ORDER BY y, z LIMIT 2 OFFSET 1')
                          :format(secondary_tertiary_name),
                    expected = {{10, 2, 2, 1}, {10, 3, 1, 2}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    expected_order_columns = {1, 2},
                },
                {
                    sql = ('SELECT x, note FROM %s WHERE x = 7 ' ..
                           'ORDER BY note DESC'):format(secondary_name),
                    expected = {{7, 'c'}, {7, 'b'}, {7, 'a'}},
                    expected_index = 'xnotemixed',
                    expected_order_column = 2,
                    expected_order_desc = true,
                },
                {
                    sql = ('SELECT z, tenant, id FROM %s WHERE z < 4 ' ..
                           'AND y = 10 AND x = 7 AND z >= 2 ' ..
                           'ORDER BY z DESC'):format(secondary_tertiary_name),
                    expected = {{3, 1, 2}, {2, 2, 1}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    expected_order_column = 1,
                    expected_order_desc = true,
                },
                {
                    sql = ('SELECT y, z, tenant, id FROM %s WHERE x = 7 ' ..
                           'AND y >= 10 ORDER BY y, z LIMIT 2 OFFSET 1')
                          :format(secondary_tertiary_name),
                    expected = {{10, 2, 2, 1}, {10, 3, 1, 2}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    expected_order_columns = {1, 2},
                },
                {
                    sql = ('SELECT y, z, tenant, id FROM %s WHERE x = 7 ' ..
                           'AND y >= 10 AND y < 11 ' ..
                           'ORDER BY y DESC, z DESC LIMIT 2 OFFSET 1')
                          :format(secondary_tertiary_name),
                    expected = {{10, 2, 2, 1}, {10, 1, 1, 1}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    expected_order_columns = {1, 2},
                    expected_order_desc = true,
                },
                {
                    sql = ('SELECT x, y, z, tenant, id FROM %s WHERE x = 7 ' ..
                           'AND y >= 10 AND y < 11 ' ..
                           'ORDER BY x ASC, y DESC, z DESC')
                          :format(secondary_tertiary_name),
                    expected = {{7, 10, 3, 1, 2}, {7, 10, 2, 2, 1},
                                {7, 10, 1, 1, 1}},
                    expected_index = secondary_tertiary_name .. '_xyz',
                    expected_order_columns = {2, 3},
                    expected_order_desc = true,
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE x = 7 ' ..
                           'AND y >= 10 AND y < 11 ORDER BY y ASC')
                          :format(secondary_name),
                    expected = {{1, 1}, {1, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE x = 7 ' ..
                           'AND y < 11 ORDER BY y DESC')
                          :format(secondary_name),
                    expected = {{1, 1}, {1, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y FROM %s WHERE x = 7 ' ..
                           'AND y >= 10 AND y < 12 ORDER BY y DESC')
                          :format(descending_secondary_name),
                    expected = {{7, 11}, {7, 10}},
                    expected_index = descending_secondary_name .. '_xy',
                    expected_order_column = 2,
                    expected_order_desc = true,
                },
                {
                    sql = ('SELECT x, note, tenant, id FROM %s ' ..
                           'ORDER BY x ASC, note DESC'):format(secondary_name),
                    expected = {{7, 'a', 1, 1}, {7, 'b', 1, 2},
                                {7, 'c', 2, 1}, {8, 'd', 2, 2}},
                    expected_index = 'xnotemixed',
                    expected_sort = {{column = 1, desc = false},
                                     {column = 2, desc = true}},
                    unordered = true,
                },
                {
                    sql = ('SELECT x, note, tenant, id FROM %s ' ..
                           'ORDER BY x DESC, note ASC'):format(secondary_name),
                    expected = {{7, 'a', 1, 1}, {7, 'b', 1, 2},
                                {7, 'c', 2, 1}, {8, 'd', 2, 2}},
                    expected_index = 'xnotemixed',
                    expected_sort = {{column = 1, desc = true},
                                     {column = 2, desc = false}},
                    unordered = true,
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE x = 7 AND y = 10')
                          :format(secondary_name),
                    expected = {{1, 1}, {1, 2}},
                    expected_index = secondary_name .. '_xy',
                    unordered = true,
                },
                {
                    sql = ('SELECT id FROM %s WHERE y = 10 AND x = 7 ' ..
                           "AND note = 'b'"):format(secondary_name),
                    expected = {{2}},
                    unordered = true,
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE y = 10 AND x = 99')
                          :format(secondary_name),
                    expected = {},
                    unordered = true,
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE x = 7 AND ' ..
                           'y = 18446744073709551615'):format(secondary_name),
                    expected = {{2, 1}},
                    unordered = true,
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE x = 7')
                          :format(secondary_name),
                    expected = {{1, 1}, {1, 2}, {2, 1}},
                    expected_index = secondary_name .. '_xy',
                    unordered = true,
                },
                {
                    sql = ('SELECT tenant, id FROM %s WHERE x >= 8')
                          :format(secondary_name),
                    expected = {{2, 2}},
                    expected_index = secondary_name .. '_xy',
                    unordered = true,
                },
                {
                    sql = ('SELECT uk, id FROM %s WHERE uk <= ' ..
                           '18446744073709551615 ORDER BY uk DESC')
                          :format(unsigned_desc_name),
                    expected = {{1}, {2}, {3}},
                    expected_index = unsigned_desc_name .. '_uk',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    expected_result_columns = {2},
                    unordered = true,
                },
                {
                    sql = ('SELECT uk, id FROM %s WHERE uk < ' ..
                           '18446744073709551615 ORDER BY uk DESC')
                          :format(unsigned_desc_name),
                    expected = {{1}, {2}},
                    expected_index = unsigned_desc_name .. '_uk',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    expected_result_columns = {2},
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE x >= 7 ' ..
                           'ORDER BY x ASC'):format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}, {8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE x >= 7 ' ..
                           'ORDER BY x ASC LIMIT 1 OFFSET 3')
                          :format(secondary_name),
                    expected = {{8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE x <= 7 ' ..
                           'ORDER BY x DESC'):format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE x >= 7 ' ..
                           'AND x < 9 ORDER BY x ASC'):format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}, {8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE x >= 7 ' ..
                           'AND x < 9 ORDER BY x DESC'):format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}, {8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE x >= 7 ' ..
                           'AND x < 9 ORDER BY x DESC LIMIT 1')
                          :format(secondary_name),
                    expected = {{8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s ORDER BY x ASC')
                          :format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}, {8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s ORDER BY x DESC')
                          :format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}, {8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s ORDER BY x ASC ' ..
                           'LIMIT 1 OFFSET 3'):format(secondary_name),
                    expected = {{8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE note = \'c\' ' ..
                           'ORDER BY x ASC'):format(secondary_name),
                    expected = {{7, 2, 1}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, tenant, id FROM %s WHERE note <> \'c\' ' ..
                           'ORDER BY x ASC LIMIT 1 OFFSET 1')
                          :format(secondary_name),
                    expected = {{7, 1, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ("SELECT x, tenant, id FROM %s " ..
                           "WHERE note IN ('b', 'd') ORDER BY x ASC " ..
                           'LIMIT 1 OFFSET 1'):format(secondary_name),
                    expected = {{8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, id FROM %s ORDER BY x ASC ' ..
                           'LIMIT 1'):format(nullable_secondary_name),
                    expected = {{box.NULL, 2}},
                    expected_index = nullable_secondary_name .. '_x',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, id FROM %s ORDER BY x DESC ' ..
                           'LIMIT 1 OFFSET 2'):format(nullable_secondary_name),
                    expected = {{box.NULL, 2}},
                    expected_index = nullable_secondary_name .. '_x',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y, tenant, id FROM %s ' ..
                           'ORDER BY x ASC, y ASC'):format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}, {8, 2, 2}},
                    expected_index = secondary_name .. '_xy',
                    expected_order_columns = {1, 2},
                    expected_result_columns = {1, 3, 4},
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y, tenant, id FROM %s ' ..
                           'ORDER BY x DESC, y DESC'):format(secondary_name),
                    expected = {{7, 1, 1}, {7, 1, 2}, {7, 2, 1}, {8, 2, 2}},
                    expected_order_columns = {1, 2},
                    expected_result_columns = {1, 3, 4},
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y FROM %s ' ..
                           'ORDER BY x DESC, y DESC')
                          :format(descending_secondary_name),
                    expected = {{8, 10}, {7, 11}, {7, 10},
                                {box.NULL, 12}},
                },
                {
                    sql = ('SELECT x, y FROM %s ' ..
                           'ORDER BY x ASC, y ASC')
                          :format(descending_secondary_name),
                    expected = {{box.NULL, 12}, {7, 10},
                                {7, 11}, {8, 10}},
                },
                {
                    sql = ('SELECT x, y FROM %s WHERE x >= 7 ' ..
                           'ORDER BY x ASC'):format(descending_secondary_name),
                    expected = {{7, 10}, {7, 11}, {8, 10}},
                    expected_index = descending_secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y FROM %s WHERE x > 7 ' ..
                           'ORDER BY x ASC'):format(descending_secondary_name),
                    expected = {{8, 10}},
                    expected_index = descending_secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y FROM %s WHERE x <= 7 ' ..
                           'ORDER BY x DESC'):format(descending_secondary_name),
                    expected = {{7, 10}, {7, 11}},
                    expected_index = descending_secondary_name .. '_xy',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y FROM %s WHERE x >= 7 AND x < 8 ' ..
                           'ORDER BY x DESC')
                          :format(descending_secondary_name),
                    expected = {{7, 10}, {7, 11}},
                    expected_index = descending_secondary_name .. '_xy',
                    expected_order_column = 1,
                    expected_order_desc = true,
                    unordered = true,
                },
                {
                    sql = ('SELECT x, y FROM %s WHERE x > 7 AND x <= 8 ' ..
                           'ORDER BY x ASC')
                          :format(descending_secondary_name),
                    expected = {{8, 10}},
                    expected_index = descending_secondary_name .. '_xy',
                    expected_order_column = 1,
                    unordered = true,
                },
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
                    sql = ('SELECT a, b FROM %s WHERE a >= 1 AND a <= 2 ' ..
                           'AND b >= 11'):format(composite_name),
                    expected = {{1, 11}, {2, 20}},
                    enabled_route = 'new_planner',
                    unordered = true,
                },
                {
                    sql = ('SELECT a, b FROM %s WHERE b = 20 AND ' ..
                           'a <= 2 AND a >= 1'):format(composite_name),
                    expected = {{2, 20}},
                    enabled_route = 'new_planner',
                    unordered = true,
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
                    sql = ('SELECT id FROM %s WHERE s = ? ORDER BY id')
                          :format(comparison_name),
                    params = {'a'},
                    expected = {{1}, {2}, {5}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE s IN (?, ?) ' ..
                           'ORDER BY id'):format(comparison_name),
                    params = {'a', 'b'},
                    expected = {{1}, {2}, {3}, {5}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE s BETWEEN ? AND ? ' ..
                           'ORDER BY id'):format(comparison_name),
                    params = {'a', 'b'},
                    expected = {{1}, {2}, {3}, {5}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = ?')
                          :format(comparison_name),
                    params = {1},
                    expected = {{1}},
                    enabled_route = 'fallback',
                    enabled_reason = 'UNSUPPORTED_FILTER',
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
                    sql = ('SELECT id FROM %s WHERE a + b = 3 ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{2}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a + b = id ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a + b BETWEEN 2 AND 3 ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{1}, {2}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a + b IN (2, 3) ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{1}, {2}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a IN (b) ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a BETWEEN b AND 2 ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{1}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE (a + b = 3) OR ' ..
                           '(a = b) ORDER BY id'):format(comparison_name),
                    expected = {{1}, {2}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a <= b ORDER BY id')
                          :format(comparison_name),
                    expected = {{1}, {2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a >= b ORDER BY id')
                          :format(comparison_name),
                    expected = {{1}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE t > s ORDER BY id')
                          :format(comparison_name),
                    expected = {{2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a + b IS NULL ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{4}, {5}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE a + b IS NOT NULL ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{1}, {2}, {3}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE ABS(a) IS NULL ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{4}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE abs(a) > 1 ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{3}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE ' ..
                           's COLLATE "unicode_ci" = \'A\' ORDER BY id')
                          :format(comparison_name),
                    expected = {{1}, {2}, {5}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE ' ..
                           's COLLATE "binary" = \'A\' ORDER BY id')
                          :format(comparison_name),
                    expected = {},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE ' ..
                           's COLLATE "unicode_ci" IS NULL ORDER BY id')
                          :format(comparison_name),
                    expected = {{4}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE s LIKE \'a%%\' ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{1}, {2}, {5}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE s NOT LIKE \'a%%\' ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{3}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE random() IS NULL ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {},
                    enabled_route = 'fallback',
                    enabled_reason = 'UNSUPPORTED_NONDETERMINISTIC',
                },
                {
                    sql = ('SELECT ABS(a) FROM %s WHERE id = 1')
                          :format(comparison_name),
                    expected = {{1}},
                    enabled_route = 'new_planner',
                },
                -- Projection expressions are emitted by the regular SQL
                -- expression bytecode path, not only when the function call
                -- is the complete result expression.
                {
                    sql = ('SELECT ABS(a) + b FROM %s WHERE id >= 1 ' ..
                           'AND id <= 3 ORDER BY id')
                          :format(comparison_name),
                    expected = {{2}, {3}, {3}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT ABS(a + b) * 2 FROM %s WHERE id >= 1 ' ..
                           'AND id <= 3 ORDER BY id')
                          :format(comparison_name),
                    expected = {{4}, {6}, {6}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE id <= 3 ORDER BY id')
                          :format(comparison_name),
                    expected = {{1}, {2}, {3}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE id < 3 ORDER BY id')
                          :format(comparison_name),
                    expected = {{1}, {2}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE id >= 3 ' ..
                           'ORDER BY id DESC'):format(comparison_name),
                    expected = {{5}, {4}, {3}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE id > 3 ' ..
                           'ORDER BY id DESC'):format(comparison_name),
                    expected = {{5}, {4}},
                    enabled_route = 'new_planner',
                },
                {
                    sql = ('SELECT id FROM %s WHERE id <= 3 ' ..
                           'ORDER BY ABS(a)'):format(comparison_name),
                    expected = {{1}, {2}, {3}},
                    unordered = true,
                    enabled_route = 'fallback',
                    enabled_reason = 'UNSUPPORTED_FUNCTION',
                },
                {
                    sql = ('SELECT id FROM %s WHERE NOT (a = b) ' ..
                           'ORDER BY id'):format(comparison_name),
                    expected = {{2}, {3}},
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
                {
                    sql = ('SELECT id, k FROM %s WHERE k = 7')
                          :format(comparison_name),
                    expected = {{1, 7}, {2, 7}, {5, 7}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE k > 7')
                          :format(comparison_name),
                    expected = {{3}},
                    expected_index = comparison_name .. '_k',
                    unordered = true,
                },
                {
                    sql = ('SELECT id FROM %s WHERE 7 <= k AND k < 8')
                          :format(comparison_name),
                    expected = {{1}, {2}, {5}},
                    expected_index = comparison_name .. '_k',
                    unordered = true,
                },
                {
                    sql = ('SELECT id FROM %s WHERE k > 6 AND k >= 7')
                          :format(comparison_name),
                    expected = {{1}, {2}, {3}, {5}},
                    expected_index = comparison_name .. '_k',
                    unordered = true,
                },
                {
                    sql = ('SELECT id FROM %s WHERE k >= 7 ' ..
                           'LIMIT 1 OFFSET 1'):format(comparison_name),
                    expected = {{2}},
                    expected_index = comparison_name .. '_k',
                    unordered = true,
                },
                {
                    sql = ('SELECT id FROM %s WHERE uk >= ' ..
                           '18446744073709551615')
                          :format(comparison_name),
                    expected = {{3}},
                    expected_index = comparison_name .. '_uk',
                    unordered = true,
                },
                {
                    sql = ('SELECT id FROM %s WHERE uk <= 10')
                          :format(comparison_name),
                    expected = {{1}, {2}, {5}},
                    expected_index = comparison_name .. '_uk',
                    unordered = true,
                },
                {
                    sql = ('SELECT id FROM %s WHERE 7 = k')
                          :format(comparison_name),
                    expected = {{1}, {2}, {5}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE k = 99')
                          :format(comparison_name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE k = NULL')
                          :format(comparison_name),
                    expected = {},
                    enabled_route = 'fallback',
                    enabled_reason = 'NO_ACCESS_PATH',
                },
                {
                    sql = ('SELECT id FROM %s WHERE k = 7 AND k = 8')
                          :format(comparison_name),
                    expected = {},
                },
                {
                    sql = ('SELECT id FROM %s WHERE id = 5 AND uk = 10')
                          :format(comparison_name),
                    expected = {{5}},
                },
                {
                    sql = ("SELECT id FROM %s WHERE k = 7 AND t = 'a'")
                          :format(comparison_name),
                    expected = {{1}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE k = 7 AND t IS NULL')
                          :format(comparison_name),
                    expected = {{5}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE k = 7 ' ..
                           'LIMIT 1 OFFSET 1'):format(comparison_name),
                    expected = {{2}},
                },
                {
                    sql = ('SELECT id FROM %s WHERE uk = ' ..
                           '18446744073709551615'):format(comparison_name),
                    expected = {{3}},
                },
            }
            local function execute(sql, params)
                if params == nil then
                    return box.execute(sql)
                end
                return box.execute(sql, params)
            end
            local function capture(enabled)
                local results = {}
                box.execute(('SET SESSION "sql_new_planner_single_table" = %s')
                            :format(enabled and 'true' or 'false'))
                for i, query in ipairs(queries) do
                    local explain, err = execute(
                        [[EXPLAIN (planner = 'summary') ]] .. query.sql,
                        query.params)
                    t.assert(err == nil, err and err.message)
                    local route = explain.rows[1][3]
                    if enabled then
                        t.assert_equals(route, query.enabled_route or
                                        'new_planner',
                                        ('query %d route on %s: %s / %s [%s]')
                                        :format(i, engine, route,
                                                tostring(explain.rows[2][3]),
                                                query.sql))
                        if query.expected_index ~= nil then
                            local plan, plan_err = execute(
                                'EXPLAIN QUERY PLAN ' .. query.sql,
                                query.params)
                            t.assert(plan_err == nil,
                                     plan_err and plan_err.message)
                            local plan_text = ''
                            for _, row in ipairs(plan.rows) do
                                for _, value in ipairs(row) do
                                    plan_text = plan_text .. tostring(value) .. ' '
                                end
                            end
                            t.assert(string.find(plan_text,
                                                 query.expected_index, 1,
                                                 true) ~= nil,
                                      'composite secondary index not selected: ' ..
                                      plan_text .. ' [' .. query.sql .. ']')
                        end
                        if query.enabled_reason ~= nil then
                            t.assert_equals(explain.rows[2][3],
                                            query.enabled_reason)
                        end
                    else
                        t.assert(route == 'current_where_c' or
                                 route == 'fallback')
                    end
                    local result
                    result, err = execute(query.sql, query.params)
                    t.assert(err == nil, err and err.message)
                    if query.expected_order_column ~= nil then
                        for row_no = 2, #result.rows do
                            local prev = result.rows[row_no - 1]
                                [query.expected_order_column]
                            local current = result.rows[row_no]
                                [query.expected_order_column]
                            if query.expected_order_desc then
                                t.assert(prev >= current)
                            else
                                t.assert(prev <= current)
                            end
                        end
                    end
                    if query.expected_order_columns ~= nil then
                        local columns = query.expected_order_columns
                        for row_no = 2, #result.rows do
                            local prev = result.rows[row_no - 1]
                            local current = result.rows[row_no]
                            for _, column in ipairs(columns) do
                                if prev[column] ~= current[column] then
                                    if query.expected_order_desc then
                                        t.assert(prev[column] > current[column])
                                    else
                                        t.assert(prev[column] < current[column])
                                    end
                                    break
                                end
                            end
                        end
                    end
                    if query.expected_sort ~= nil then
                        for row_no = 2, #result.rows do
                            local prev = result.rows[row_no - 1]
                            local current = result.rows[row_no]
                            for _, term in ipairs(query.expected_sort) do
                                if prev[term.column] ~= current[term.column] then
                                    if term.desc then
                                        t.assert(prev[term.column] >
                                                 current[term.column])
                                    else
                                        t.assert(prev[term.column] <
                                                 current[term.column])
                                    end
                                    break
                                end
                            end
                        end
                    end
                    if query.unordered then
                        table.sort(result.rows, function(a, b)
                            for column = 1, #a do
                                if a[column] ~= b[column] then
                                    return a[column] < b[column]
                                end
                            end
                            return false
                        end)
                    end
                    if query.expected_result_columns ~= nil then
                        local projected = {}
                        for row_no, row in ipairs(result.rows) do
                            projected[row_no] = {}
                            for _, column in ipairs(query.expected_result_columns) do
                                table.insert(projected[row_no], row[column])
                            end
                        end
                        results[i] = projected
                    else
                        results[i] = result.rows
                    end
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
            local parameter_sql = ('SELECT id FROM %s WHERE s = ? ' ..
                                   'ORDER BY id'):format(comparison_name)
            box.execute('SET SESSION "sql_new_planner_single_table" = true')
            local parameter_explain = execute(
                [[EXPLAIN (planner = 'summary') ]] .. parameter_sql, {'a'})
            t.assert_equals(parameter_explain.rows[1][3], 'new_planner')
            local parameter_stmt = box.prepare(parameter_sql)
            local parameter_cases = {
                {{'a'}, {{1}, {2}, {5}}},
                {{'b'}, {{3}}},
                {{box.NULL}, {}},
            }
            for _, case in ipairs(parameter_cases) do
                local result = box.execute(parameter_stmt.stmt_id, case[1])
                t.assert_equals(result.rows, case[2])
            end
            box.unprepare(parameter_stmt.stmt_id)
            box.execute(('DROP TABLE %s'):format(name))
            box.execute(('DROP TABLE %s'):format(composite_name))
            box.execute(('DROP TABLE %s'):format(secondary_name))
            box.execute(('DROP TABLE %s'):format(secondary_tertiary_name))
            box.execute(('DROP TABLE %s'):format(nullable_secondary_name))
            box.execute(('DROP TABLE %s'):format(comparison_name))
            box.execute(('DROP TABLE %s'):format(pattern_name))
            box.execute(('DROP TABLE %s'):format(unsigned_desc_name))
        end
    end)
end

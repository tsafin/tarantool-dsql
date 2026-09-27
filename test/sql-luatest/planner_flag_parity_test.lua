local server = require('luatest.server')
local t = require('luatest')

local g = t.group('sql_new_planner_flag_parity')

g.before_all(function()
    g.server = server:new({alias = 'm37_parity'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_unsigned_primary_key_off_on_off_parity = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        local function rows_equal(left, right)
            if #left ~= #right then
                return false
            end
            for i = 1, #left do
                if #left[i] ~= #right[i] then
                    return false
                end
                for j = 1, #left[i] do
                    if left[i][j] ~= right[i][j] then
                        return false
                    end
                end
            end
            return true
        end

        local queries = {
            [[SELECT id, v FROM planner_flag_parity_memtx]],
            [[SELECT v FROM planner_flag_parity_memtx WHERE id = 18446744073709551615]],
            [[SELECT v FROM planner_flag_parity_memtx WHERE 9223372036854775808 = id]],
            [[SELECT id, v FROM planner_flag_parity_memtx WHERE id > 9223372036854775807 ORDER BY id ASC]],
            [[SELECT id FROM planner_flag_parity_memtx WHERE id >= 9223372036854775808 ORDER BY id ASC LIMIT 1]],
            [[SELECT v FROM planner_flag_parity_memtx WHERE id <= 9223372036854775807 ORDER BY id DESC LIMIT 2]],
            [[SELECT id, v FROM planner_flag_parity_memtx WHERE id > 9223372036854775807 AND id <= 18446744073709551615 ORDER BY id DESC]],
            [[SELECT id FROM planner_flag_parity_memtx WHERE id >= 0 AND id < 9223372036854775808 ORDER BY id ASC LIMIT 2 OFFSET 1]],
        }
        local disabled_fallback_queries = {
            [2] = true, [3] = true, [4] = true, [5] = true,
            [7] = true, [8] = true,
        }
        local engines = {'memtx', 'vinyl'}
        for _, engine in ipairs(engines) do
            local name = 'planner_flag_parity_' .. engine
            box.execute(('CREATE TABLE %s (id UNSIGNED PRIMARY KEY, v INTEGER) ' ..
                         "WITH ENGINE = '%s'"):format(name, engine))
            box.execute(('INSERT INTO %s VALUES (0, 0), (1, 10), ' ..
                         '(9223372036854775807, NULL), (9223372036854775808, 30), ' ..
                         '(18446744073709551615, 40)'):format(name))

            local engine_queries = {}
            for i, sql in ipairs(queries) do
                engine_queries[i] = sql:gsub('planner_flag_parity_memtx', name)
            end

            local function capture(expected_route)
                local result = {}
                for i, sql in ipairs(engine_queries) do
                    local explain = box.execute([[EXPLAIN (planner = 'summary') ]] .. sql)
                    if expected_route == nil then
                        if disabled_fallback_queries[i] then
                            t.assert_equals(explain.rows[1][3], 'fallback')
                            t.assert_equals(explain.rows[2][3],
                                            'UNSUPPORTED_EXPRESSION')
                        else
                            t.assert_equals(explain.rows[1][3], 'current_where_c',
                                            ('disabled query %d on %s: %s/%s')
                                            :format(i, engine,
                                                    tostring(explain.rows[1][3]),
                                                    tostring(explain.rows[2][3])))
                        end
                    else
                        t.assert_equals(explain.rows[1][3], expected_route,
                                        ('query %d has unexpected route on %s')
                                        :format(i, engine))
                    end
                    local response, err = box.execute(sql)
                    t.assert(err == nil and response ~= nil,
                             ('query %d failed on %s: %s'):format(i, engine,
                                                                tostring(err)))
                    result[i] = response.rows
                end
                return result
            end

            -- The feature is default-off for a fresh session; pin that
            -- contract separately from the explicit off/on/off transitions.
            local default_disabled = capture(nil)
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local disabled_first = capture(nil)
            for i = 1, #engine_queries do
                t.assert(rows_equal(default_disabled[i], disabled_first[i]),
                         ('default route result differs for query %d on %s')
                         :format(i, engine))
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            local enabled = capture('new_planner')
            for i = 1, #engine_queries do
                t.assert(rows_equal(disabled_first[i], enabled[i]),
                         ('enabled result differs for query %d on %s'):format(i, engine))
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local disabled_again = capture(nil)
            for i = 1, #engine_queries do
                t.assert(rows_equal(disabled_first[i], disabled_again[i]),
                         ('second disabled result differs for query %d on %s'):format(i, engine))
            end
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

g.test_feature_flag_is_session_local = function()
    local netbox = require('net.box')
    local first = netbox.connect(g.server.net_box_uri)
    local second = netbox.connect(g.server.net_box_uri)
    local table_name = 'planner_flag_session_local'
    first:execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY)'):format(table_name))
    first:execute(('INSERT INTO %s VALUES (1)'):format(table_name))
    local query = ('SELECT id FROM %s'):format(table_name)
    local explain = [[EXPLAIN (planner = 'summary') ]] .. query

    local function route(conn)
        return conn:execute(explain).rows[1][3]
    end

    -- A fresh connection starts with the default-off setting. Enabling the
    -- experimental route on one connection must not affect another session.
    t.assert_equals(route(first), 'current_where_c')
    t.assert_equals(route(second), 'current_where_c')
    first:execute([[SET SESSION "sql_new_planner_single_table" = true]])
    t.assert_equals(route(first), 'new_planner')
    t.assert_equals(route(second), 'current_where_c')

    -- The second session can opt in independently; turning the first off
    -- likewise must not change the second session's route.
    second:execute([[SET SESSION "sql_new_planner_single_table" = true]])
    first:execute([[SET SESSION "sql_new_planner_single_table" = false]])
    t.assert_equals(route(first), 'current_where_c')
    t.assert_equals(route(second), 'new_planner')

    first:execute(('DROP TABLE %s'):format(table_name))
    first:close()
    second:close()
end

g.test_one_sided_range_wrong_order_falls_back = function()
    g.server:exec(function()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_flag_range_direction_' .. engine
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY) ' ..
                         "WITH ENGINE = '%s'"):format(name, engine))
            box.execute(('INSERT INTO %s VALUES (1), (2), (3)'):format(name))
            local queries = {
                {
                    ('SELECT id FROM %s WHERE id > 1 ORDER BY id DESC')
                        :format(name),
                    {{3}, {2}},
                },
                {
                    ('SELECT id FROM %s WHERE id < 3 ORDER BY id ASC')
                        :format(name),
                    {{1}, {2}},
                },
            }

            local function capture(enabled)
                box.execute(('SET SESSION "sql_new_planner_single_table" = %s')
                            :format(enabled and 'true' or 'false'))
                local rows = {}
                for i, item in ipairs(queries) do
                    local explain = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. item[1])
                    if enabled then
                        t.assert_equals(explain.rows[1][3], 'fallback')
                        t.assert_equals(explain.rows[2][3],
                                        'UNSUPPORTED_FILTER')
                    else
                        t.assert_equals(explain.rows[1][3], 'fallback')
                        t.assert_equals(explain.rows[2][3],
                                        'UNSUPPORTED_EXPRESSION')
                    end
                    rows[i] = box.execute(item[1]).rows
                    t.assert_equals(rows[i], item[2])
                end
                return rows
            end

            local disabled = capture(false)
            local enabled = capture(true)
            t.assert_equals(enabled, disabled)
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

g.test_text_primary_key_scan_off_on_off_parity = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_flag_text_' .. engine
            box.execute(('CREATE TABLE %s (id TEXT PRIMARY KEY, v INTEGER) ' ..
                         "WITH ENGINE = '%s'"):format(name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "('b', NULL), ('a', 10), ('c', 30)"):format(name))
            local queries = {
                ('SELECT id, v FROM %s'):format(name),
                ('SELECT id FROM %s ORDER BY id DESC LIMIT 2'):format(name),
            }
            local function capture(routes)
                local rows = {}
                for i, sql in ipairs(queries) do
                    local explain = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. sql)
                    t.assert_equals(explain.rows[1][3], routes[i][1],
                                    ('query %d route on %s: %s/%s')
                                    :format(i, engine,
                                            tostring(explain.rows[1][3]),
                                            tostring(explain.rows[2][3])))
                    t.assert_equals(explain.rows[2][3], routes[i][2],
                                    ('query %d reason on %s')
                                    :format(i, engine))
                    rows[i] = box.execute(sql).rows
                end
                return rows
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local disabled_before = box.stat.sql()
            local disabled = capture({
                {'current_where_c'},
                {'fallback', 'UNSUPPORTED_EXPRESSION'},
            })
            local disabled_after = box.stat.sql()
            t.assert_equals(disabled_after.sql_planner_fallback_total,
                            disabled_before.sql_planner_fallback_total + 2)
            t.assert_equals(
                disabled_after.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total,
                disabled_before.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total + 2)
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            local enabled_before = box.stat.sql()
            local enabled = capture({
                {'new_planner'},
                {'new_planner'},
            })
            local enabled_after = box.stat.sql()
            t.assert_equals(enabled_after.sql_planner_fallback_total,
                            enabled_before.sql_planner_fallback_total)
            t.assert_equals(
                enabled_after.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total,
                enabled_before.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total)
            t.assert_equals(enabled[2], {{'c'}, {'b'}})
            t.assert_equals(disabled[2], {{'c'}, {'b'}})
            for i = 1, #queries do
                table.sort(enabled[i], function(a, b) return a[1] < b[1] end)
                table.sort(disabled[i], function(a, b) return a[1] < b[1] end)
                t.assert_equals(enabled[i], disabled[i],
                                ('text-key rows differ for query %d on %s')
                                :format(i, engine))
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local disabled_again = capture({
                {'current_where_c'},
                {'fallback', 'UNSUPPORTED_EXPRESSION'},
            })
            for i = 1, #queries do
                table.sort(disabled_again[i],
                           function(a, b) return a[1] < b[1] end)
                t.assert_equals(disabled_again[i], disabled[i],
                                ('repeat rows differ for query %d on %s')
                                :format(i, engine))
            end
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

g.test_composite_primary_key_order_off_on_off_parity = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_flag_composite_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b INTEGER, v STRING, ' ..
                         'PRIMARY KEY (a, b)) WITH ENGINE = \'%s\'')
                        :format(name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         "(2, 20, 'd'), (1, 30, 'c'), (2, 10, 'e'), " ..
                         "(1, 10, 'a'), (1, 20, 'b')"):format(name))
            local queries = {
                ('SELECT a, b FROM %s ORDER BY a ASC'):format(name),
                ('SELECT a, b FROM %s ORDER BY a ASC, b ASC'):format(name),
                ('SELECT a, b FROM %s ORDER BY a DESC, b DESC'):format(name),
                ('SELECT a, b FROM %s ORDER BY a DESC, b DESC LIMIT 2 OFFSET 1')
                    :format(name),
                ('SELECT a, b FROM %s ORDER BY a ASC LIMIT 3 OFFSET 1')
                    :format(name),
                ('SELECT a, b FROM %s ORDER BY a DESC, b DESC LIMIT 0')
                    :format(name),
                ('SELECT a, b FROM %s ORDER BY a ASC, b DESC'):format(name),
            }

            local function capture(enabled)
                box.execute(('SET SESSION "sql_new_planner_single_table" = %s')
                            :format(enabled and 'true' or 'false'))
                local results = {}
                for i, sql in ipairs(queries) do
                    local explain = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. sql)
                    if enabled and i < 7 then
                        t.assert_equals(explain.rows[1][3], 'new_planner')
                        t.assert_equals(explain.rows[2][3], nil)
                    elseif not enabled or i == 7 then
                        t.assert_equals(explain.rows[1][3], 'fallback')
                        t.assert_equals(explain.rows[2][3],
                                        'UNSUPPORTED_EXPRESSION')
                    else
                        t.assert_equals(explain.rows[1][3], 'current_where_c')
                        t.assert_equals(explain.rows[2][3], nil)
                    end
                    results[i] = box.execute(sql).rows
                end
                return results
            end

            local off_before = box.stat.sql()
            local default_off = capture(false)
            local off_after = box.stat.sql()
            t.assert_equals(off_after.sql_planner_fallback_total,
                            off_before.sql_planner_fallback_total + 14)
            t.assert_equals(
                off_after.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total,
                off_before.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total + 14)
            local enabled_before = box.stat.sql()
            local enabled = capture(true)
            local enabled_after = box.stat.sql()
            t.assert_equals(enabled_after.sql_planner_fallback_total,
                            enabled_before.sql_planner_fallback_total + 2)
            t.assert_equals(
                enabled_after.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total,
                enabled_before.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total + 2)
            local off_again_before = box.stat.sql()
            local off_again = capture(false)
            local off_again_after = box.stat.sql()
            t.assert_equals(off_again_after.sql_planner_fallback_total,
                            off_again_before.sql_planner_fallback_total + 14)
            t.assert_equals(
                off_again_after.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total,
                off_again_before.sql_planner_fallback_UNSUPPORTED_EXPRESSION_total + 14)
            local expected = {
                {{1, 10}, {1, 20}, {1, 30}, {2, 10}, {2, 20}},
                {{1, 10}, {1, 20}, {1, 30}, {2, 10}, {2, 20}},
                {{2, 20}, {2, 10}, {1, 30}, {1, 20}, {1, 10}},
                {{2, 10}, {1, 30}},
                {{1, 20}, {1, 30}, {2, 10}},
                {},
                {{1, 30}, {1, 20}, {1, 10}, {2, 20}, {2, 10}},
            }
            for i = 1, #queries do
                t.assert_equals(enabled[i], expected[i],
                                ('enabled query %d order differs on %s')
                                :format(i, engine))
                table.sort(default_off[i], function(a, b)
                    if a[1] ~= b[1] then return a[1] < b[1] end
                    return a[2] < b[2]
                end)
                table.sort(off_again[i], function(a, b)
                    if a[1] ~= b[1] then return a[1] < b[1] end
                    return a[2] < b[2]
                end)
                table.sort(enabled[i], function(a, b)
                    if a[1] ~= b[1] then return a[1] < b[1] end
                    return a[2] < b[2]
                end)
                t.assert_equals(enabled[i], default_off[i])
                t.assert_equals(off_again[i], default_off[i])
            end
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

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

g.test_unordered_hash_primary_scan_off_on_off = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        local space = box.schema.space.create('planner_flag_hash_scan', {
            engine = 'memtx',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'string'},
            },
        })
        space:create_index('pk', {
            type = 'hash',
            parts = {{field = 'id', type = 'unsigned'}},
        })
        space:insert({1, 'one'})
        space:insert({2, 'two'})
        space:insert({3, 'three'})

        local function run(sql)
            local explain = [[EXPLAIN (planner = 'summary') ]] .. sql
            local explain_result, explain_err = box.execute(explain)
            t.assert(explain_err == nil and explain_result ~= nil,
                     ('hash primary EXPLAIN failed: %s')
                     :format(tostring(explain_err)))
            t.assert_equals(explain_result.rows[1][3], 'new_planner')
            local result, err = box.execute(sql)
            t.assert(err == nil and result ~= nil,
                     ('hash primary scan failed: %s'):format(tostring(err)))
            return result.rows
        end

        local sql = [[SELECT id, value FROM planner_flag_hash_scan]]
        box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
        local _, off_err = box.execute(sql)
        t.assert_equals(off_err.message,
                        'SQL does not support using non-TREE index type. ' ..
                        'Please, use INDEXED BY clause to force using proper index.')
        box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
        local on_rows = run(sql)
        t.assert_equals(on_rows, {{1, 'one'}, {2, 'two'}, {3, 'three'}})
        t.assert_equals(run([[SELECT id, value FROM planner_flag_hash_scan
                              WHERE id IS NOT NULL]]), on_rows)
        t.assert_equals(run([[SELECT id, value FROM planner_flag_hash_scan
                              WHERE id IS NULL]]), {})
        for _, unsupported in ipairs({
            [[SELECT id FROM planner_flag_hash_scan WHERE id = 2]],
            [[SELECT id FROM planner_flag_hash_scan ORDER BY id]],
        }) do
            local _, unsupported_err = box.execute(unsupported)
            t.assert_equals(unsupported_err.message, off_err.message,
                            'HASH point/order route must remain unsupported')
        end
        box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
        local _, off_again_err = box.execute(sql)
        t.assert_equals(off_again_err.message, off_err.message)
        space:drop()
    end)
end

g.test_composite_primary_key_prefix_ranges_off_on_off = function()
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
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_composite_prefix_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b INTEGER, v STRING, ' ..
                         'PRIMARY KEY (a, b)) WITH ENGINE = \'%s\'')
                        :format(name, engine))
            box.execute(('INSERT INTO %s VALUES (2, 20, \'d\'), ' ..
                         '(1, 30, \'c\'), (2, 10, \'e\'), (1, 10, \'a\'), ' ..
                         '(1, 20, \'b\')'):format(name))
            local queries = {
                ('SELECT a, b, v FROM %s WHERE a = 1'):format(name),
                ('SELECT a, b FROM %s WHERE a >= 2 ' ..
                 'ORDER BY a ASC, b ASC'):format(name),
                ('SELECT a, b FROM %s WHERE a < 2 ' ..
                 'ORDER BY a DESC, b DESC'):format(name),
                ('SELECT a, b FROM %s WHERE a >= 1 AND a < 2 ' ..
                 'ORDER BY a ASC, b ASC'):format(name),
                ('SELECT a, b FROM %s WHERE a = 1 ' ..
                 'ORDER BY a DESC, b DESC'):format(name),
            }
            local function capture(route)
                local results = {}
                for i, sql in ipairs(queries) do
                    local explain, err = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. sql)
                    t.assert(err == nil, err and err.message)
                    local actual_route = explain.rows[1][3]
                    if route == 'current_where_c' then
                        t.assert(actual_route == 'current_where_c' or
                                 actual_route == 'fallback',
                                 ('query %d on %s has unsafe disabled route %s')
                                 :format(i, engine, tostring(actual_route)))
                        if actual_route == 'fallback' then
                            t.assert(type(explain.rows[2][3]) == 'string' and
                                     #explain.rows[2][3] > 0,
                                     ('query %d on %s lacks fallback reason')
                                     :format(i, engine))
                        end
                    else
                        t.assert_equals(actual_route, route,
                                        ('query %d on %s')
                                        :format(i, engine))
                    end
                    local result
                    result, err = box.execute(sql)
                    t.assert(err == nil and result ~= nil,
                             ('query %d on %s: %s')
                             :format(i, engine, tostring(err)))
                    results[i] = result.rows
                end
                return results
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local off = capture('current_where_c')
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            local on = capture('new_planner')
            for i = 1, #queries do
                t.assert(rows_equal(off[i], on[i]),
                         ('composite prefix query %d changed on %s')
                         :format(i, engine))
            end
            t.assert_equals(#on[1], 3)
            t.assert_equals(on[1][1], {1, 10, 'a'})
            t.assert_equals(on[1][2], {1, 20, 'b'})
            t.assert_equals(on[1][3], {1, 30, 'c'})
            t.assert_equals(on[5][1], {1, 30})
            t.assert_equals(on[5][3], {1, 10})
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local off_again = capture('current_where_c')
            for i = 1, #queries do
                t.assert(rows_equal(off[i], off_again[i]),
                         ('second disabled query %d changed on %s')
                         :format(i, engine))
            end
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

g.test_composite_prefix_equality_then_range_off_on_off = function()
    g.server:exec(function()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_prefix_range_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b UNSIGNED, c INTEGER, ' ..
                         'v STRING, PRIMARY KEY (a, b, c)) WITH ENGINE = \'%s\'')
                        :format(name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         '(1, 10, 2, \'a\'), (1, 20, 3, \'b\'), ' ..
                         '(1, 20, 1, \'c\'), (1, 30, 2, \'d\'), ' ..
                         '(1, 40, 1, \'e\'), (2, 20, 1, \'f\'), ' ..
                         '(1, 9223372036854775808, 2, \'g\'), ' ..
                         '(1, 18446744073709551615, 1, \'max\')')
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
            }
            local expected = {
                {{'c'}, {'b'}, {'d'}, {'e'}, {'g'}, {'max'}},
                {{1, 10, 2, 'a'}, {1, 20, 1, 'c'}, {1, 20, 3, 'b'}},
                {{1, 20, 1, 'c'}, {1, 20, 3, 'b'}, {1, 30, 2, 'd'}},
                {{'g'}, {'max'}},
                {{'d'}, {'e'}, {'g'}, {'max'}},
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
                                        ('query %d did not use suffix-range lowering on %s (%s)')
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
                                    ('query %d ordering/result on %s route=%s')
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

g.test_composite_primary_key_point_lookup_off_on_off = function()
    g.server:exec(function()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_composite_point_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b UNSIGNED, v STRING, ' ..
                         'PRIMARY KEY (a, b)) WITH ENGINE = \'%s\'')
                        :format(name, engine))
            box.execute(('INSERT INTO %s VALUES (1, 18446744073709551615, \'max\'), ' ..
                         '(1, 7, \'seven\'), (2, 9, \'other\')'):format(name))
            local queries = {
                ('SELECT v FROM %s WHERE a = 1 AND b = 18446744073709551615'):format(name),
                ('SELECT v FROM %s WHERE 7 = b AND 1 = a'):format(name),
                ('SELECT v FROM %s WHERE a = 1 AND b = 8'):format(name),
            }
            local function capture(expected_route)
                local rows = {}
                for i, sql in ipairs(queries) do
                    local explain, err = box.execute([[EXPLAIN (planner = 'summary') ]] .. sql)
                    t.assert(err == nil, err and err.message)
                    local route = explain.rows[1][3]
                    if expected_route == 'current_where_c' then
                        t.assert(route == 'current_where_c' or route == 'fallback',
                                 ('query %d on %s has unexpected disabled route %s')
                                 :format(i, engine, tostring(route)))
                        if route == 'fallback' then
                            t.assert(type(explain.rows[2][3]) == 'string' and
                                     #explain.rows[2][3] > 0,
                                     ('query %d on %s lacks fallback reason')
                                     :format(i, engine))
                        end
                    else
                        t.assert_equals(route, expected_route)
                    end
                    local result
                    result, err = box.execute(sql)
                    t.assert(err == nil, err and err.message)
                    rows[i] = result.rows
                end
                return rows
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local off = capture('current_where_c')
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            local on = capture('new_planner')
            t.assert_equals(on, off)
            t.assert_equals(on[1], {{'max'}})
            t.assert_equals(on[2], {{'seven'}})
            t.assert_equals(on[3], {})
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            t.assert_equals(capture('current_where_c'), off)
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

g.test_three_part_composite_primary_key_point_lookup = function()
    g.server:exec(function()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_composite_point3_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b UNSIGNED, c INTEGER, ' ..
                         'v STRING, PRIMARY KEY (a, b, c)) WITH ENGINE = \'%s\'')
                        :format(name, engine))
            box.execute(('INSERT INTO %s VALUES (1, 7, 3, \'hit\'), ' ..
                         '(1, 7, 4, \'other\')'):format(name))
            local point = ('SELECT v FROM %s WHERE c = 3 AND a = 1 AND b = 7')
                :format(name)
            local limited_point = point .. ' LIMIT 1'
            local offset_point = point .. ' LIMIT 1 OFFSET 1'
            local incomplete = ('SELECT v FROM %s WHERE a = 1 AND b = 7 AND c > 2')
                :format(name)
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local off, err = box.execute(point)
            t.assert(err == nil, err and err.message)
            local limited_queries = {limited_point, offset_point}
            local off_limited = {}
            for i, sql in ipairs(limited_queries) do
                local result
                result, err = box.execute(sql)
                t.assert(err == nil, err and err.message)
                off_limited[i] = result.rows
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            local explain
            explain, err = box.execute([[EXPLAIN (planner = 'summary') ]] .. point)
            t.assert(err == nil, err and err.message)
            t.assert_equals(explain.rows[1][3], 'new_planner',
                            tostring(explain.rows[2] and explain.rows[2][3]))
            local on
            on, err = box.execute(point)
            t.assert(err == nil, err and err.message)
            t.assert_equals(on.rows, off.rows)
            t.assert_equals(on.rows, {{'hit'}})
            for i, sql in ipairs(limited_queries) do
                explain, err = box.execute(
                    [[EXPLAIN (planner = 'summary') ]] .. sql)
                t.assert(err == nil, err and err.message)
                t.assert_equals(explain.rows[1][3], 'new_planner')
                local limited
                limited, err = box.execute(sql)
                t.assert(err == nil, err and err.message)
                t.assert_equals(limited.rows, off_limited[i])
                t.assert_equals(limited.rows, sql == limited_point and
                                {{'hit'}} or {})
            end

            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            explain, err = box.execute([[EXPLAIN (planner = 'summary') ]] .. point)
            t.assert(err == nil, err and err.message)
            local disabled_route = explain.rows[1][3]
            t.assert(disabled_route == 'current_where_c' or
                     disabled_route == 'fallback')
            if disabled_route == 'fallback' then
                t.assert(type(explain.rows[2][3]) == 'string' and
                         #explain.rows[2][3] > 0)
            end
            local off_again
            off_again, err = box.execute(point)
            t.assert(err == nil, err and err.message)
            t.assert_equals(off_again.rows, off.rows)

            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])

            explain, err = box.execute([[EXPLAIN (planner = 'summary') ]] ..
                                       incomplete)
            t.assert(err == nil, err and err.message)
            t.assert_equals(explain.rows[1][3], 'new_planner')
            local suffix_range, suffix_range_err = box.execute(incomplete)
            t.assert(suffix_range_err == nil,
                     suffix_range_err and suffix_range_err.message)
            t.assert_equals(suffix_range.rows, {{'hit'}, {'other'}})
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
end

g.test_composite_primary_key_multi_part_prefix_scan = function()
    g.server:exec(function()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'planner_composite_prefix2_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b UNSIGNED, c INTEGER, ' ..
                         'v STRING, PRIMARY KEY (a, b, c)) WITH ENGINE = \'%s\'')
                        :format(name, engine))
            box.execute(('INSERT INTO %s VALUES (1, 7, 4, \'b\'), ' ..
                         '(2, 7, 1, \'d\'), (1, 7, 2, \'a\'), ' ..
                         '(1, 8, 3, \'c\'), ' ..
                         '(1, 18446744073709551615, 9, \'max\')')
                        :format(name))
            local queries = {
                ('SELECT c, v FROM %s WHERE a = 1 AND b = 7'):format(name),
                ('SELECT c, v FROM %s WHERE 7 = b AND 1 = a'):format(name),
                ('SELECT c, v FROM %s WHERE a = 1 AND b = 99'):format(name),
                ('SELECT c, v FROM %s WHERE a = 1 AND ' ..
                 'b = 18446744073709551615'):format(name),
            }
            local limited_queries = {
                queries[1] .. ' LIMIT 1',
                queries[1] .. ' LIMIT 0',
                queries[1] .. ' LIMIT 1 OFFSET 1',
            }
            local ordered = ('SELECT c, v FROM %s WHERE a = 1 AND b = 7 ' ..
                             'ORDER BY c ASC'):format(name)
            local ordered_full_key_prefix = ('SELECT a, b, c, v FROM %s ' ..
                'WHERE a = 1 AND b = 7 ORDER BY a ASC, b ASC, c ASC')
                :format(name)
            local function capture(enabled)
                local rows = {}
                for i, sql in ipairs(queries) do
                    local explain, err = box.execute(
                        [[EXPLAIN (planner = 'summary') ]] .. sql)
                    t.assert(err == nil, err and err.message)
                    if enabled then
                        t.assert_equals(explain.rows[1][3], 'new_planner')
                    else
                        local route = explain.rows[1][3]
                        t.assert(route == 'current_where_c' or route == 'fallback')
                        if route == 'fallback' then
                            t.assert(type(explain.rows[2][3]) == 'string' and
                                     #explain.rows[2][3] > 0)
                        end
                    end
                    local result
                    result, err = box.execute(sql)
                    t.assert(err == nil, err and err.message)
                    rows[i] = result.rows
                end
                return rows
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local off = capture(false)
            local ordered_off = box.execute(ordered).rows
            local full_key_order_off = box.execute(
                ordered_full_key_prefix).rows
            local off_limited = {}
            for i, sql in ipairs(limited_queries) do
                local result, err = box.execute(sql)
                t.assert(err == nil, err and err.message)
                off_limited[i] = result.rows
            end
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            local on = capture(true)
            t.assert_equals(on, off)
            t.assert_equals(on[1], {{2, 'a'}, {4, 'b'}})
            t.assert_equals(on[3], {})
            t.assert_equals(on[4], {{9, 'max'}})

            local ordered_explain, ordered_err = box.execute(
                [[EXPLAIN (planner = 'summary') ]] .. ordered)
            t.assert(ordered_err == nil, ordered_err and ordered_err.message)
            t.assert_equals(ordered_explain.rows[1][3], 'new_planner')
            local ordered_on = box.execute(ordered)
            t.assert_equals(ordered_on.rows, ordered_off)
            t.assert_equals(ordered_on.rows, {{2, 'a'}, {4, 'b'}})

            local full_key_order_explain, full_key_order_err = box.execute(
                [[EXPLAIN (planner = 'summary') ]] ..
                ordered_full_key_prefix)
            t.assert(full_key_order_err == nil,
                     full_key_order_err and full_key_order_err.message)
            t.assert_equals(full_key_order_explain.rows[1][3], 'new_planner')
            local full_key_order_on = box.execute(ordered_full_key_prefix)
            t.assert_equals(full_key_order_on.rows, full_key_order_off)
            t.assert_equals(full_key_order_on.rows,
                {{1, 7, 2, 'a'}, {1, 7, 4, 'b'}})

            local nonleading = ('SELECT c, v FROM %s WHERE b = 7 AND c = 2')
                :format(name)
            local explain, err = box.execute(
                [[EXPLAIN (planner = 'summary') ]] .. nonleading)
            t.assert(err == nil, err and err.message)
            t.assert_equals(explain.rows[1][3], 'fallback')
            t.assert(type(explain.rows[2][3]) == 'string' and
                     #explain.rows[2][3] > 0)
            local descending = ('SELECT c, v FROM %s WHERE a = 1 AND b = 7 ' ..
                                'ORDER BY c DESC'):format(name)
            explain, err = box.execute(
                [[EXPLAIN (planner = 'summary') ]] .. descending)
            t.assert(err == nil, err and err.message)
            t.assert_equals(explain.rows[1][3], 'fallback')

            local suffix_name = 'planner_composite_suffix2_' .. engine
            box.execute(('CREATE TABLE %s (a INTEGER, b UNSIGNED, ' ..
                         'c INTEGER, d INTEGER, v STRING, ' ..
                         'PRIMARY KEY (a, b, c, d)) WITH ENGINE = \'%s\'')
                        :format(suffix_name, engine))
            box.execute(('INSERT INTO %s VALUES ' ..
                         '(1, 7, 2, 9, \'late\'), ' ..
                         '(1, 7, 2, 1, \'early\'), ' ..
                         '(1, 7, 3, 0, \'next\'), ' ..
                         '(2, 7, 1, 0, \'other\')'):format(suffix_name))
            local suffix_order = ('SELECT c, d, v FROM %s ' ..
                'WHERE a = 1 AND b = 7 ORDER BY c ASC, d ASC')
                :format(suffix_name)
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local suffix_off, suffix_err = box.execute(suffix_order)
            t.assert(suffix_err == nil, suffix_err and suffix_err.message)
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            explain, err = box.execute(
                [[EXPLAIN (planner = 'summary') ]] .. suffix_order)
            t.assert(err == nil, err and err.message)
            t.assert_equals(explain.rows[1][3], 'new_planner')
            local suffix_on
            suffix_on, err = box.execute(suffix_order)
            t.assert(err == nil, err and err.message)
            t.assert_equals(suffix_on.rows, suffix_off.rows)
            t.assert_equals(suffix_on.rows,
                {{2, 1, 'early'}, {2, 9, 'late'}, {3, 0, 'next'}})
            local suffix_desc = suffix_order:gsub(
                'ORDER BY c ASC, d ASC', 'ORDER BY c DESC, d DESC')
            explain, err = box.execute(
                [[EXPLAIN (planner = 'summary') ]] .. suffix_desc)
            t.assert(err == nil, err and err.message)
            t.assert_equals(explain.rows[1][3], 'fallback')
            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            box.execute(('DROP TABLE %s'):format(suffix_name))
            box.execute([[SET SESSION "sql_new_planner_single_table" = true]])

            for i, sql in ipairs(limited_queries) do
                explain, err = box.execute(
                    [[EXPLAIN (planner = 'summary') ]] .. sql)
                t.assert(err == nil, err and err.message)
                t.assert_equals(explain.rows[1][3], 'new_planner')
                local limited_on
                limited_on, err = box.execute(sql)
                t.assert(err == nil, err and err.message)
                t.assert_equals(limited_on.rows, off_limited[i])
                t.assert_equals(#limited_on.rows, i == 2 and 0 or 1)
            end

            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            t.assert_equals(capture(false), off)
            box.execute(('DROP TABLE %s'):format(name))
        end
    end)
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

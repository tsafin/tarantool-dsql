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

            box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            local disabled_first = capture(nil)
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

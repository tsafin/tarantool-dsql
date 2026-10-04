local server = require('luatest.server')
local t = require('luatest')

local g = t.group('sql_dp_oracle_legality')

g.before_all(function()
    g.server = server:new({alias = 'sql_dp_oracle_legality', env = {
        SQL_PATH_SOLVER_ORACLE_MAX_RELATIONS = '4',
        SQL_PATH_SOLVER_WIDTH_TWO = '1',
        SQL_PATH_SOLVER_WIDTH_MANY = '1',
    }})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_ineligible_joins_keep_bounded_solver = function()
    local result = g.server:exec(function()
        box.execute('SET SESSION "sql_seq_scan" = true')
        for _, name in ipairs({'dpl_a', 'dpl_b', 'dpl_c'}) do
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, ' ..
                         'k INTEGER)'):format(name))
            box.execute(('INSERT INTO %s VALUES (1, 1)'):format(name))
            box.execute(('CREATE INDEX %s_k ON %s(k)'):format(name, name))
        end
        local queries = {
            left = [[SELECT a.id FROM dpl_a AS a LEFT JOIN dpl_b AS b
                ON a.id = b.id JOIN dpl_c AS c ON a.id = c.id]],
            cross = [[SELECT a.id FROM dpl_a AS a CROSS JOIN dpl_b AS b
                JOIN dpl_c AS c ON a.id = c.id]],
            natural = [[SELECT id FROM dpl_a NATURAL JOIN dpl_b
                NATURAL JOIN dpl_c]],
            using = [[SELECT id FROM dpl_a JOIN dpl_b USING(id)
                JOIN dpl_c USING(id)]],
        }
        local out = {}
        for name, sql in pairs(queries) do
            local before = box.stat.sql()
            local rows = box.execute(sql).rows
            local after = box.stat.sql()
            out[name] = {
                rows = rows,
                truncated = after.sql_planner_paths_truncated_total -
                    before.sql_planner_paths_truncated_total,
                dominated = after.sql_planner_paths_dominated_total -
                    before.sql_planner_paths_dominated_total,
            }
        end
        for _, name in ipairs({'dpl_c', 'dpl_b', 'dpl_a'}) do
            box.execute('DROP TABLE ' .. name)
        end
        return out
    end)
    for _, shape in ipairs({'left', 'cross', 'natural', 'using'}) do
        t.assert_equals(result[shape].rows, {{1}})
    end
    -- NATURAL/USING are freely reorderable enough to exercise pruning;
    -- observing it proves that opt-in exact enumeration was not used.
    t.assert_gt(result.natural.truncated, 0)
    t.assert_gt(result.using.truncated, 0)
end

g.test_relation_limit_falls_back_to_beam = function()
    local result = g.server:exec(function()
        box.execute('SET SESSION "sql_seq_scan" = true')
        for _, name in ipairs({'dpf_a', 'dpf_b', 'dpf_c', 'dpf_d', 'dpf_e'}) do
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, k INTEGER)'):
                        format(name))
            box.execute(('INSERT INTO %s VALUES (1, 1)'):format(name))
        end
        local sql = [[SELECT a.id FROM dpf_a AS a
            JOIN dpf_b AS b ON a.id = b.id
            JOIN dpf_c AS c ON a.id = c.id
            JOIN dpf_d AS d ON a.id = d.id
            JOIN dpf_e AS e ON a.id = e.id]]
        local before = box.stat.sql()
        local rows = box.execute(sql).rows
        local after = box.stat.sql()
        for _, name in ipairs({'dpf_e', 'dpf_d', 'dpf_c', 'dpf_b', 'dpf_a'}) do
            box.execute('DROP TABLE ' .. name)
        end
        return {rows = rows,
                truncated = after.sql_planner_paths_truncated_total -
                    before.sql_planner_paths_truncated_total}
    end)
    t.assert_equals(result.rows, {{1}})
    t.assert_gt(result.truncated, 0)
end

g.test_nested_select_keeps_bounded_solver = function()
    local result = g.server:exec(function()
        box.execute('SET SESSION "sql_seq_scan" = true')
        for _, name in ipairs({'dpn_a', 'dpn_b', 'dpn_c'}) do
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, k INTEGER)'):
                        format(name))
            box.execute(('INSERT INTO %s VALUES (1, 1), (2, 2)'):
                        format(name))
            box.execute(('CREATE INDEX %s_k ON %s(k)'):format(name, name))
        end
        local sql = [[SELECT (SELECT COUNT(*) FROM dpn_a AS a
            JOIN dpn_b AS b ON a.k = b.k
            JOIN dpn_c AS c ON b.k = c.k)]]
        local before = box.stat.sql()
        local rows = box.execute(sql).rows
        local after = box.stat.sql()
        for _, name in ipairs({'dpn_c', 'dpn_b', 'dpn_a'}) do
            box.execute('DROP TABLE ' .. name)
        end
        return {
            rows = rows,
            truncated = after.sql_planner_paths_truncated_total -
                before.sql_planner_paths_truncated_total,
        }
    end)
    t.assert_equals(result.rows, {{2}})
    t.assert_gt(result.truncated, 0)
end

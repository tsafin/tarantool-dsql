local server = require('luatest.server')
local t = require('luatest')

local g = t.group('sql_dp_exact_oracle')

g.before_all(function()
    g.beam = server:new({alias = 'sql_dp_beam', env = {
        VDBE_DISPATCHER = 'generated',
        SQL_PATH_SOLVER_WIDTH_MANY = '1',
    }})
    g.oracle = server:new({alias = 'sql_dp_oracle', env = {
        VDBE_DISPATCHER = 'generated',
        SQL_PATH_SOLVER_ORACLE_MAX_RELATIONS = '4',
    }})
    g.beam:start()
    g.oracle:start()
end)

g.after_all(function()
    g.oracle:stop()
    g.beam:stop()
end)

local function run_fixture(srv, engine)
    return srv:exec(function(eng)
        box.execute('SET SESSION "sql_seq_scan" = true')
        for _, name in ipairs({'dpo_a', 'dpo_b', 'dpo_c', 'dpo_d'}) do
            box.execute(('CREATE TABLE %s (id INTEGER PRIMARY KEY, k INTEGER) ' ..
                         "WITH ENGINE = '%s'"):format(name, eng))
            box.execute(('CREATE INDEX %s_k ON %s(k)'):format(name, name))
        end
        box.execute('INSERT INTO dpo_a VALUES (1, 1), (2, 1), (3, 2)')
        box.execute('INSERT INTO dpo_b VALUES (1, 1), (2, 2)')
        box.execute('INSERT INTO dpo_c VALUES (1, 1), (2, 1), (3, 2)')
        box.execute('INSERT INTO dpo_d VALUES (1, 1), (2, 1), (3, 2)')
        local sql = [[SELECT a.id FROM dpo_a AS a
            JOIN dpo_b AS b ON a.k = b.k
            JOIN dpo_c AS c ON b.k = c.k
            JOIN dpo_d AS d ON c.k = d.k
            WHERE a.k = 1 ORDER BY a.id]]
        local before = box.stat.sql()
        local result = box.execute(sql)
        local after = box.stat.sql()
        local delta = {
            generated = after.sql_planner_paths_generated_total -
                        before.sql_planner_paths_generated_total,
            dominated = after.sql_planner_paths_dominated_total -
                        before.sql_planner_paths_dominated_total,
            truncated = after.sql_planner_paths_truncated_total -
                        before.sql_planner_paths_truncated_total,
        }
        local plan = box.execute('EXPLAIN QUERY PLAN ' .. sql).rows
        for _, name in ipairs({'dpo_d', 'dpo_c', 'dpo_b', 'dpo_a'}) do
            box.execute('DROP TABLE ' .. name)
        end
        return {rows = result.rows, delta = delta, plan = plan}
    end, {engine})
end

g.test_same_candidates_without_beam_pruning = function()
    for _, engine in ipairs({'memtx', 'vinyl'}) do
        local beam = run_fixture(g.beam, engine)
        local oracle = run_fixture(g.oracle, engine)
        t.assert_equals(beam.rows, {{1}, {1}, {1}, {1},
                                    {2}, {2}, {2}, {2}})
        t.assert_equals(oracle.rows, beam.rows)
        t.assert_gt(oracle.delta.generated, 10)
        t.assert_ge(oracle.delta.generated, beam.delta.generated)
        t.assert_equals(oracle.delta.dominated, 0)
        t.assert_equals(oracle.delta.truncated, 0)
        t.assert_gt(beam.delta.truncated, 0)
        t.assert_gt(#oracle.plan, 0)
    end
end

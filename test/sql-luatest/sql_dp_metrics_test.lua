local server = require('luatest.server')
local t = require('luatest')

local g = t.group('sql_dp_metrics')

g.before_all(function()
    g.server = server:new({alias = 'sql_dp_metrics', env = {
        SQL_PATH_SOLVER_ORACLE_MAX_RELATIONS = '4',
    }})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_exact_frontier_metrics = function()
    local res = g.server:exec(function()
        local build_dir = assert(os.getenv('BUILDDIR'))
        package.cpath = build_dir .. '/test/box/?.so;' .. package.cpath
        local adapter = require('sql_stats_snapshot_test')
        box.execute('SET SESSION "sql_seq_scan" = true')
        for _, name in ipairs({'dpm_a', 'dpm_b', 'dpm_c'}) do
            box.execute(('CREATE TABLE %s (id INT PRIMARY KEY, k INT)'):
                        format(name))
            box.execute(('CREATE INDEX %s_k ON %s(k)'):format(name, name))
            box.execute(('INSERT INTO %s VALUES (1, 1), (2, 2)'):
                        format(name))
        end
        local sql = [[SELECT a.id FROM dpm_a AS a
            JOIN dpm_b AS b ON a.k = b.k
            JOIN dpm_c AS c ON b.k = c.k
            WHERE a.k = 1 ORDER BY a.id]]
        local metrics = adapter.join_planner_metrics(sql)
        local ineligible = adapter.join_planner_metrics(
            'SELECT id FROM dpm_a')
        for _, name in ipairs({'dpm_c', 'dpm_b', 'dpm_a'}) do
            box.execute('DROP TABLE ' .. name)
        end
        return {metrics = metrics, ineligible = ineligible}
    end)
    t.assert_equals(res.ineligible, nil)
    t.assert_gt(res.metrics.generated, 0)
    t.assert_equals(res.metrics.dominated, 0)
    t.assert_equals(res.metrics.truncated, 0)
    t.assert_gt(res.metrics.retained, 0)
    t.assert_gt(res.metrics.peak_frontier, 0)
    t.assert_gt(res.metrics.peak_solver_bytes, 0)
    t.assert_ge(res.metrics.planner_elapsed_us, 0)
end

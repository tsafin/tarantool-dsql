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
        local prefixes = adapter.join_prefix_estimates(sql)
        local actuals = adapter.join_prefix_actuals(sql)
        local ineligible = adapter.join_planner_metrics(
            'SELECT id FROM dpm_a')
        local no_prefix = adapter.join_prefix_estimates(
            'SELECT id FROM dpm_a')
        local left_prefix = adapter.join_prefix_estimates(
            'SELECT a.id FROM dpm_a a LEFT JOIN dpm_b b ON a.k = b.k')
        local cross_prefix = adapter.join_prefix_estimates(
            'SELECT a.id FROM dpm_a a CROSS JOIN dpm_b b WHERE a.k = b.k')
        for _, name in ipairs({'dpm_c', 'dpm_b', 'dpm_a'}) do
            box.execute('DROP TABLE ' .. name)
        end
        return {metrics = metrics, prefixes = prefixes, actuals = actuals,
                ineligible = ineligible, no_prefix = no_prefix,
                left_prefix = left_prefix, cross_prefix = cross_prefix}
    end)
    t.assert_equals(res.ineligible, nil)
    t.assert_equals(res.no_prefix, nil)
    t.assert_equals(res.left_prefix, nil)
    t.assert_equals(res.cross_prefix, nil)
    t.assert_equals(#res.prefixes, 3)
    t.assert_equals(#res.actuals, 3)
    for i = 1, 3 do
        t.assert_equals(res.actuals[i].relation_mask,
                        res.prefixes[i].relation_mask)
        t.assert_equals(res.actuals[i].estimated_rows,
                        res.prefixes[i].estimated_rows)
        t.assert_equals(res.actuals[i].actual_rows, 1)
    end
    t.assert_equals(res.prefixes[3].relation_mask, 7)
    t.assert_gt(res.prefixes[1].estimated_rows, 0)
    t.assert_gt(res.metrics.generated, 0)
    t.assert_equals(res.metrics.dominated, 0)
    t.assert_equals(res.metrics.truncated, 0)
    t.assert_gt(res.metrics.retained, 0)
    t.assert_gt(res.metrics.peak_frontier, 0)
    t.assert_gt(res.metrics.peak_solver_bytes, 0)
    t.assert_ge(res.metrics.planner_elapsed_us, 0)
end

g.test_equivalent_literal_uses_mcv = function()
    local res = g.server:exec(function()
        local build_dir = assert(os.getenv('BUILDDIR'))
        package.cpath = build_dir .. '/test/box/?.so;' .. package.cpath
        local adapter = require('sql_stats_snapshot_test')
        adapter.clear()
        box.execute('SET SESSION "sql_seq_scan" = true')
        local result = {}
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local a = 'dpe_a_' .. engine
            local b = 'dpe_b_' .. engine
            box.execute(('CREATE TABLE %s (id INT PRIMARY KEY, k INT) ' ..
                         "WITH ENGINE = '%s'"):format(a, engine))
            box.execute(('CREATE TABLE %s (id INT PRIMARY KEY, k INT) ' ..
                         "WITH ENGINE = '%s'"):format(b, engine))
            box.execute(('CREATE INDEX %s_k ON %s(k)'):format(a, a))
            box.execute(('CREATE INDEX %s_k ON %s(k)'):format(b, b))
            for id = 1, 40 do
                box.execute(('INSERT INTO %s VALUES (?, ?)'):format(a),
                            {id, id <= 30 and 1 or id})
            end
            for id = 1, 60 do
                box.execute(('INSERT INTO %s VALUES (?, ?)'):format(b),
                            {id, id <= 40 and 1 or id})
            end
            box.execute('ANALYZE ' .. a)
            box.execute('ANALYZE ' .. b)
            local sql = ('SELECT a.id, b.id FROM %s a JOIN %s b ' ..
                         'ON a.k = b.k WHERE a.k = 1'):format(a, b)
            result[engine] = adapter.join_prefix_actuals(sql)
            box.execute('DROP TABLE ' .. b)
            box.execute('DROP TABLE ' .. a)
        end
        adapter.clear()
        return result
    end)
    for _, engine in ipairs({'memtx', 'vinyl'}) do
        local prefixes = res[engine]
        t.assert_equals(#prefixes, 2)
        t.assert_equals(prefixes[1].relation_mask, 1)
        t.assert_ge(prefixes[1].estimated_rows, 20)
        t.assert_ge(prefixes[2].estimated_rows, 50)
        t.assert_equals(prefixes[1].actual_rows, 30)
        t.assert_equals(prefixes[2].actual_rows, 1200)
    end
end

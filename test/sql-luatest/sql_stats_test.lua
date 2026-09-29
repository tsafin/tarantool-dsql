local server = require('luatest.server')
local t = require('luatest')

local g = t.group()
local g_jit = t.group('sql_jit')
local g_budget = t.group('sql_planner_budget')

local function opcode_count(profile, ...)
    for i = 1, select('#', ...) do
        local name = select(i, ...)
        local value = profile[name]
        if value ~= nil then
            return value
        end
    end
    return 0
end

g.before_all(function()
    g.server = server:new({
        alias = 'sql_stats',
        env = { VDBE_DISPATCHER = 'generated' },
    })
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g_jit.before_all(function()
    g_jit.server = server:new({
        alias = 'sql_stats_jit',
        env = {
            SQL_JIT_ENABLE = '1',
            VDBE_DISPATCHER = 'generated',
        },
    })
    g_jit.server:start()
end)

g_jit.after_all(function()
    g_jit.server:stop()
end)

g_budget.before_all(function()
    g_budget.server = server:new({
        alias = 'sql_planner_budget',
        env = {
            SQL_PATH_SOLVER_WIDTH_ONE = '2',
            SQL_PATH_SOLVER_WIDTH_TWO = '7',
            SQL_PATH_SOLVER_WIDTH_MANY = '11',
        },
    })
    g_budget.server:start()
end)

g_budget.after_all(function()
    g_budget.server:stop()
end)

g.test_shared_view_multi_relation_candidate = function()
    local res = g.server:exec(function()
        local adapter = package.loaded.sql_stats_tx_context_test
        if adapter == nil then
            return {test_wrapper_unavailable = true}
        end
        local memtx = box.schema.space.create('sql_stats_multi_memtx', {
            engine = 'memtx',
        })
        memtx:create_index('primary', {
            parts = {{field = 1, type = 'unsigned'}},
        })
        memtx:create_index('by_value', {
            unique = false,
            parts = {{field = 2, type = 'unsigned'}},
        })
        local vinyl = box.schema.space.create('sql_stats_multi_vinyl', {
            engine = 'vinyl',
        })
        vinyl:create_index('primary', {
            parts = {{field = 1, type = 'unsigned'}},
        })
        vinyl:create_index('by_value', {
            unique = false,
            parts = {{field = 2, type = 'unsigned'}},
        })
        for i = 1, 8 do
            memtx:insert({i, i % 3})
            vinyl:insert({i, i % 3})
        end
        local result = adapter.collect_multirelation(memtx.id, vinyl.id)
        memtx:drop()
        vinyl:drop()
        return result
    end)

    if res.test_wrapper_unavailable then
        t.skip('SQL stats live wrapper requires a TEST_BUILD server')
    end
    t.assert_equals(res.published_two_relations, true)
    t.assert_equals(res.native_hash_provenance, true)
    t.assert_equals(res.publish_rc, 0)
    t.assert_equals(res.first_relation_rows, 8)
    t.assert_equals(res.second_relation_rows, 8)
    t.assert_equals(res.later_relation_failed_closed, true)
    t.assert_equals(res.installed_snapshot_preserved, true)
end

g.test_sql_stats_shape_and_growth = function()
    g.server:exec(function()
        local function sum_values(map)
            local total = 0
            for _, value in pairs(map) do
                total = total + value
            end
            return total
        end

        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        t.assert_type(before.sql_interpreter_step_count, 'number')
        t.assert_type(before.sql_jit_step_count, 'number')
        t.assert_type(before.sql_jit_compile_count, 'number')
        t.assert_type(before.sql_jit_compile_success_count, 'number')
        t.assert_type(before.sql_jit_exec_count, 'number')
        t.assert_type(before.sql_jit_full_run_count, 'number')
        t.assert_type(before.sql_jit_fallback_count, 'number')
        t.assert_type(before.sql_jit_resume_skip_count, 'number')
        t.assert_type(before.sql_jit_guard_skip_count, 'number')
        t.assert_type(before.sql_opcode_profile_enabled, 'number')
        t.assert_type(before.sql_planner_paths_generated_total, 'number')
        t.assert_type(before.sql_planner_paths_dominated_total, 'number')
        t.assert_type(before.sql_planner_paths_truncated_total, 'number')
        t.assert_type(before.sql_planner_paths_retained_total, 'number')
        if before.sql_opcode_profile_enabled ~= 0 then
            t.assert_type(before.interpreter_opcode_profile, 'table')
            t.assert_type(before.interpreter_opcode_profile.count, 'table')
            t.assert_type(before.interpreter_opcode_profile.time_us, 'table')
            t.assert_type(before.jit_opcode_profile, 'table')
            t.assert_type(before.jit_opcode_profile.count, 'table')
            t.assert_type(before.jit_opcode_profile.time_us, 'table')
        end

        t.assert_equals(box.execute([[SELECT a + 1 FROM t WHERE id = 2;]]).rows,
                        {{21}})

        local after = box.stat.sql()
        t.assert_gt(after.sql_interpreter_step_count,
                    before.sql_interpreter_step_count)
        t.assert_ge(after.sql_jit_step_count, before.sql_jit_step_count)
        t.assert_ge(after.sql_jit_compile_count, before.sql_jit_compile_count)
        t.assert_gt(after.sql_planner_paths_generated_total,
                    before.sql_planner_paths_generated_total)
        t.assert_gt(after.sql_planner_paths_retained_total,
                    before.sql_planner_paths_retained_total)
        if before.sql_opcode_profile_enabled ~= 0 then
            t.assert_gt(sum_values(after.interpreter_opcode_profile.count),
                        sum_values(before.interpreter_opcode_profile.count))
        end

        box.execute([[DROP TABLE t;]])
    end)
end

g.test_sql_statement_compile_count = function()
    local res = g.server:exec(function()
        local before = box.stat.sql()
        local stmt = box.prepare([[SELECT 177013 + 1;]])
        local after_prepare = box.stat.sql()
        local result = box.execute(stmt.stmt_id)
        local after_execute = box.stat.sql()
        box.unprepare(stmt.stmt_id)
        return {
            before = before.sql_statement_compiles_total,
            after_prepare = after_prepare.sql_statement_compiles_total,
            after_execute = after_execute.sql_statement_compiles_total,
            rows = result.rows,
        }
    end)

    t.assert_type(res.before, 'number')
    t.assert_equals(res.after_prepare, res.before + 1)
    t.assert_equals(res.after_execute, res.after_prepare)
    t.assert_equals(res.rows, {{177014}})
end

g.test_snapshot_estimate_adapter = function()
    local res = g.server:exec(function()
        local build_dir = os.getenv('BUILDDIR')
        if build_dir ~= nil then
            package.cpath = build_dir .. '/test/box/?.so;' .. package.cpath
        end
        local ok, adapter = pcall(require, 'sql_stats_snapshot_test')
        if not ok then
            return {test_wrapper_unavailable = true}
        end
        adapter.clear()
        box.execute([[CREATE TABLE sql_stats_adapter_t
                      (id INT PRIMARY KEY, a INT);]])
        box.execute([[CREATE INDEX sql_stats_adapter_ix
                      ON sql_stats_adapter_t (a);]])
        box.execute([[INSERT INTO sql_stats_adapter_t VALUES
                      (1, 1), (2, 1), (3, 1), (4, 2),
                      (5, 2), (6, 2), (7, 3), (8, 3);]])
        box.execute([[CREATE TABLE sql_stats_skew_t
                      (id INT PRIMARY KEY, a INT);]])
        box.execute([[CREATE INDEX sql_stats_skew_ix
                      ON sql_stats_skew_t (a);]])
        box.execute([[INSERT INTO sql_stats_skew_t VALUES
                      (1, 1), (2, 1), (3, 1), (4, 1),
                      (5, 1), (6, 1), (7, 1), (8, 1),
                      (9, 2), (10, 3), (11, 4);]])
        local space = box.space.sql_stats_adapter_t
        local index_id = space.index.sql_stats_adapter_ix.id
        local baseline = adapter.estimates(space.id, index_id)
        local function explain_estimate(marker, predicate, table_name)
            table_name = table_name or 'sql_stats_adapter_t'
            local query = 'EXPLAIN QUERY PLAN SELECT id FROM '..
                          table_name..' WHERE '..predicate..'; -- '..
                          marker
            local plan = box.execute(query).rows
            return tonumber(plan[1][4]:match('~([0-9]+) row'))
        end
        local function capture_e1_observations(configuration, statistics_id)
            local output = os.getenv('E1_SQL_OUTPUT')
            if output == nil then
                return
            end
            local json = require('json')
            local clock = require('clock')
            local metadata = {
                workload_id = os.getenv('E1_WORKLOAD_ID'),
                source_commit = os.getenv('E1_SOURCE_COMMIT'),
                binary_sha256 = os.getenv('E1_BINARY_SHA256'),
                data_sha256 = os.getenv('E1_DATA_SHA256'),
            }
            for name, value in pairs(metadata) do
                assert(value ~= nil and value ~= '',
                       'missing E1 metadata: ' .. name)
            end
            local fixture_material =
                'CREATE TABLE sql_stats_adapter_t (id INT PRIMARY KEY, a INT);\n' ..
                'CREATE INDEX sql_stats_adapter_ix ON sql_stats_adapter_t (a);\n' ..
                'INSERT INTO sql_stats_adapter_t VALUES ' ..
                '(1, 1), (2, 1), (3, 1), (4, 2), ' ..
                '(5, 2), (6, 2), (7, 3), (8, 3);\n' ..
                'CREATE TABLE sql_stats_skew_t (id INT PRIMARY KEY, a INT);\n' ..
                'CREATE INDEX sql_stats_skew_ix ON sql_stats_skew_t (a);\n' ..
                'INSERT INTO sql_stats_skew_t VALUES ' ..
                '(1, 1), (2, 1), (3, 1), (4, 1), (5, 1), (6, 1), (7, 1), (8, 1), ' ..
                '(9, 2), (10, 3), (11, 4);'
            local digest = require('digest')
            assert(string.hex(digest.sha256(fixture_material)) ==
                   metadata.data_sha256,
                   'E1 data hash does not match the live SQL fixture')
            local queries = {
                {id = 'equality-a1', predicate = 'a = 1'},
                {id = 'equality-a2', predicate = 'a = 2'},
                {id = 'equality-a3', predicate = 'a = 3'},
                {id = 'equality-empty', predicate = 'a = 99'},
                {id = 'range-selective', predicate = 'a >= 2'},
                {id = 'range-nonselective', predicate = 'a >= 1'},
                {id = 'skew-hot', table = 'sql_stats_skew_t', predicate = 'a = 1'},
                {id = 'skew-tail', table = 'sql_stats_skew_t', predicate = 'a = 2'},
                {id = 'skew-range', table = 'sql_stats_skew_t', predicate = 'a >= 3'},
                {id = 'skew-empty', table = 'sql_stats_skew_t', predicate = 'a = 99'},
            }
            for _, query in ipairs(queries) do
                local table_name = query.table or 'sql_stats_adapter_t'
                local sql = 'SELECT id FROM '..table_name..' WHERE '..
                            query.predicate..';'
                local stmt = box.prepare(sql)
                local function record(repeat_no, warmup)
                    local estimate = explain_estimate('E1 live capture',
                                                      query.predicate,
                                                      table_name)
                    assert(estimate ~= nil,
                           'missing EXPLAIN row estimate for '..query.id)
                    local started = clock.monotonic()
                    local result = box.execute(stmt.stmt_id)
                    local elapsed_us = math.max(1,
                        math.floor((clock.monotonic() - started) * 1000000))
                    local row = {
                        schema_version = 1,
                        workload_id = metadata.workload_id,
                        query_id = query.id,
                        engine = 'memtx',
                        dispatcher = 'generated',
                        configuration = configuration,
                        source_commit = metadata.source_commit,
                        binary_sha256 = metadata.binary_sha256,
                        data_sha256 = metadata.data_sha256,
                        statistics_id = statistics_id,
                        ['repeat'] = repeat_no,
                        warmup = warmup,
                        elapsed_us = elapsed_us,
                        cardinalities = {{
                            stage_id = 'select-output',
                            estimated_rows = estimate,
                            actual_rows = #result.rows,
                        }},
                    }
                    local file = assert(io.open(output, 'a'))
                    file:write(json.encode(row), '\n')
                    file:close()
                end
                record(0, true)
                for repeat_no = 1, 5 do
                    record(repeat_no, false)
                end
                box.unprepare(stmt.stmt_id)
            end
        end
        local baseline_plan_estimate = explain_estimate('baseline', 'a = 1')
        local cached_plan_before = explain_estimate('stats refresh', 'a = 1')
        local baseline_range_estimate = explain_estimate('baseline range',
                                                         'a >= 2')
        capture_e1_observations('no-stats', 'no-snapshot-v1')
        adapter.install(space.id, index_id, 128, 96, 32, false)
        local cached_plan_after = explain_estimate('stats refresh', 'a = 1')
        local current = adapter.estimates(space.id, index_id)
        local range_estimate = explain_estimate('snapshot range', 'a >= 2')
        adapter.clear()
        local cleared = adapter.estimates(space.id, index_id)
        adapter.install(space.id, index_id, 128, 96, 0, false)
        local missing_prefix = adapter.estimates(space.id, index_id)
        adapter.clear()
        adapter.install(space.id, index_id, 128, 96, 32, true)
        local stale = adapter.estimates(space.id, index_id)
        adapter.install(space.id, index_id, 1000000, 1000000, 1, false)
        local hot_plan_estimate = explain_estimate('hot distribution',
                                                   'a = 1')
        adapter.install(space.id, index_id, 1000000, 1000000, 1000000, false)
        local unique_plan_estimate = explain_estimate('unique distribution',
                                                      'a = 1')
        local actual_equality_rows = #box.execute(
            [[SELECT id FROM sql_stats_adapter_t WHERE a = 1;]]).rows
        adapter.install(space.id, index_id, 8, 8, 3, false)
        local measured_plan_estimate = explain_estimate('measured uniform',
                                                        'a = 1')
        if os.getenv('E1_SQL_OUTPUT') ~= nil then
            adapter.clear()
            box.execute('ANALYZE sql_stats_adapter_t')
            box.execute('ANALYZE sql_stats_skew_t')
            capture_e1_observations('live-analyze', 'volatile-analyze-v1')
        end
        local rows = box.execute([[SELECT id FROM sql_stats_adapter_t
                                   WHERE a = 1;]]).rows
        adapter.clear()
        box.execute([[DROP TABLE sql_stats_skew_t;]])
        box.execute([[DROP TABLE sql_stats_adapter_t;]])
        return {
            baseline = baseline,
            current = current,
            cleared = cleared,
            missing_prefix = missing_prefix,
            stale = stale,
            baseline_plan_estimate = baseline_plan_estimate,
            cached_plan_before = cached_plan_before,
            cached_plan_after = cached_plan_after,
            baseline_range_estimate = baseline_range_estimate,
            range_estimate = range_estimate,
            hot_plan_estimate = hot_plan_estimate,
            unique_plan_estimate = unique_plan_estimate,
            measured_plan_estimate = measured_plan_estimate,
            actual_equality_rows = actual_equality_rows,
            rows = rows,
        }
    end)

    if res.test_wrapper_unavailable then
        t.skip('SQL stats live wrapper requires a TEST_BUILD server')
    end

    t.assert_gt(res.current.relation, res.baseline.relation)
    t.assert_lt(res.current.prefix, res.baseline.prefix)
    t.assert_equals(res.cleared, res.baseline)
    t.assert_gt(res.missing_prefix.relation, res.baseline.relation)
    t.assert_equals(res.missing_prefix.prefix, res.baseline.prefix)
    t.assert_equals(res.stale, res.baseline)
    t.assert_lt(res.cached_plan_after, res.cached_plan_before)
    t.assert_lt(res.range_estimate, res.baseline_range_estimate)
    t.assert_gt(res.hot_plan_estimate, res.baseline_plan_estimate)
    t.assert_lt(res.unique_plan_estimate, res.baseline_plan_estimate)
    local baseline_qerror = math.max(
        res.baseline_plan_estimate / res.actual_equality_rows,
        res.actual_equality_rows / res.baseline_plan_estimate)
    local measured_qerror = math.max(
        res.measured_plan_estimate / res.actual_equality_rows,
        res.actual_equality_rows / res.measured_plan_estimate)
    t.assert_lt(measured_qerror, baseline_qerror)
    t.assert_equals(res.rows, {{1}, {2}, {3}})
end

g.test_prepared_statement_retains_stats_generation = function()
    local res = g.server:exec(function()
        local build_dir = os.getenv('BUILDDIR')
        if build_dir ~= nil then
            package.cpath = build_dir .. '/test/box/?.so;' .. package.cpath
        end
        local ok, adapter = pcall(require, 'sql_stats_snapshot_test')
        if not ok then
            return {test_wrapper_unavailable = true}
        end
        adapter.clear()
        box.execute([[CREATE TABLE sql_stats_stmt_owner_t
                      (id INT PRIMARY KEY);]])
        local space = box.space.sql_stats_stmt_owner_t
        adapter.install(space.id, space.index[0].id, 32, 32, 0, false, 101)
        local old_stmt = adapter.prepare(
            'SELECT id FROM sql_stats_stmt_owner_t WHERE id = 1')
        local old_generation = adapter.stmt_catalog_version(old_stmt)
        adapter.install(space.id, space.index[0].id, 64, 64, 0, false, 202)
        local retained_generation = adapter.stmt_catalog_version(old_stmt)
        local installed_generation = adapter.state().catalog_version
        local new_stmt = adapter.prepare(
            'SELECT id FROM sql_stats_stmt_owner_t WHERE id = 1')
        local new_generation = adapter.stmt_catalog_version(new_stmt)
        adapter.delete_stmt(old_stmt)
        adapter.delete_stmt(new_stmt)
        adapter.clear()
        box.execute([[DROP TABLE sql_stats_stmt_owner_t;]])
        return {
            old_generation = old_generation,
            retained_generation = retained_generation,
            installed_generation = installed_generation,
            new_generation = new_generation,
        }
    end)

    if res.test_wrapper_unavailable then
        t.skip('SQL stats live wrapper requires a TEST_BUILD server')
    end
    t.assert_equals(res.old_generation, 101)
    t.assert_equals(res.retained_generation, 101)
    t.assert_equals(res.installed_generation, 202)
    t.assert_equals(res.new_generation, 202)
end

g.test_transaction_sampler_memtx_and_vinyl = function()
    local res = g.server:exec(function()
        local sampler = package.loaded.sql_stats_tx_context_test
        if sampler == nil then
            return {test_wrapper_unavailable = true}
        end
        local output = {}
        local spaces = {}
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local name = 'sql_stats_tx_context_'..engine
            local space = box.schema.space.create(name, {engine = engine})
            space:format({
                {name = 'id', type = 'unsigned'},
                {name = 'a', type = 'unsigned'},
            })
            space:create_index('primary')
            space:create_index('by_a', {
                unique = false,
                parts = {{field = 2, type = 'unsigned'}},
            })
            for i = 1, 8 do
                space:insert({i, i % 3})
            end
            local secondary_id = space.index.by_a.id
            output[engine] = {
                primary = sampler.sample(space.id, space.index.primary.id),
                secondary = sampler.sample(space.id, secondary_id),
                candidate = sampler.collect_candidate(space.id),
                view_candidate = sampler.collect_view_candidate(space.id,
                                                                false, true),
            }
            output[engine].budget_rejection =
                sampler.collect_view_candidate(space.id, false, true, 1)
            local stale_candidate = sampler.collect_view_candidate(space.id,
                                                                    true)
            if stale_candidate.candidate_built == 1 then
                -- The view retains the old population, but publishing it
                -- after a committed write must fail closed.
                space:insert({9, 9})
                local stale_publish = sampler.publish_held_candidate()
                stale_candidate.publish_after_write = stale_publish.rc
                stale_candidate.preserved_old_snapshot = stale_publish.preserved
                space:delete({9})
            end
            output[engine].stale_view_candidate = stale_candidate
            spaces[engine] = space
        end
        -- One core read view must cover both engines. Keep it open across
        -- deterministic commits, then prove each primary and secondary scan
        -- still sees the same pre-commit population.
        local opened = sampler.visibility_open(spaces.memtx.id,
                                               spaces.vinyl.id)
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local space = spaces[engine]
            box.begin()
            space:delete({1})
            space:delete({2})
            space:insert({9, 9})
            space:insert({10, 10})
            box.commit()
        end
        local before = {
            memtx = sampler.visibility_scan(spaces.memtx.id),
            vinyl = sampler.visibility_scan(spaces.vinyl.id),
        }
        sampler.visibility_close()
        local reopened = sampler.visibility_open(spaces.memtx.id,
                                                 spaces.vinyl.id)
        local after = {
            memtx = sampler.visibility_scan(spaces.memtx.id),
            vinyl = sampler.visibility_scan(spaces.vinyl.id),
        }
        sampler.visibility_close()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            output[engine].visibility = {
                open_rc = opened.rc,
                reopen_rc = reopened.rc,
                view_id = opened.id,
                before = before[engine],
                after = after[engine],
            }
            package.loaded.sql_stats_snapshot_test.clear()
            spaces[engine]:drop()
        end
        return output
    end)

    if res.test_wrapper_unavailable then
        t.skip('SQL stats live wrapper requires a TEST_BUILD server')
    end

    for _, engine in ipairs({'memtx', 'vinyl'}) do
        t.assert_equals(res[engine].candidate.begin_rc, 0)
        t.assert_equals(res[engine].candidate.candidate_built, 1,
                        'candidate assembly failed for '..engine..': '..
                        res[engine].candidate.extract_0_errors..'/'..
                        res[engine].candidate.extract_1_errors)
        t.assert_equals(res[engine].candidate.finish_rc, 0)
        t.assert_equals(res[engine].candidate.relation_rows, 8)
        t.assert_equals(res[engine].candidate.width_rows, 4)
        t.assert_equals(res[engine].candidate.primary_rows, 8)
        t.assert_equals(res[engine].candidate.secondary_rows, 8)
        t.assert_equals(res[engine].candidate.extract_0_calls, 4)
        t.assert_equals(res[engine].candidate.extract_1_calls, 4)
        t.assert_equals(res[engine].candidate.extract_0_errors, 0)
        t.assert_equals(res[engine].candidate.extract_1_errors, 0)
        local view_candidate = res[engine].view_candidate
        t.assert_equals(view_candidate.candidate_built, 1)
        t.assert_equals(view_candidate.publish_rc, 0)
        t.assert_equals(view_candidate.relation_rows, 8)
        t.assert_equals(view_candidate.width_rows, 4)
        t.assert_equals(view_candidate.primary_rows, 8)
        t.assert_equals(view_candidate.secondary_rows, 8)
        t.assert_equals(res[engine].budget_rejection.candidate_built, 0)
        t.assert_equals(res[engine].budget_rejection.has_installed_snapshot,
                        true)
        t.assert_equals(res[engine].stale_view_candidate.candidate_built, 1)
        t.assert_equals(res[engine].stale_view_candidate.publish_rc, 1)
        t.assert_equals(res[engine].stale_view_candidate.publish_after_write, -1)
        t.assert_equals(res[engine].stale_view_candidate.preserved_old_snapshot,
                        true)
        local visibility = res[engine].visibility
        t.assert_equals(visibility.open_rc, 0)
        t.assert_equals(visibility.reopen_rc, 0)
        t.assert_gt(visibility.view_id, 0)
        local before_primary = visibility.before.primary
        local before_secondary = visibility.before.secondary
        local after_primary = visibility.after.primary
        local after_secondary = visibility.after.secondary
        table.sort(before_primary)
        table.sort(before_secondary)
        table.sort(after_primary)
        table.sort(after_secondary)
        t.assert_equals(before_primary, {1, 2, 3, 4, 5, 6, 7, 8})
        t.assert_equals(before_secondary, before_primary)
        t.assert_equals(after_primary, {3, 4, 5, 6, 7, 8, 9, 10})
        t.assert_equals(after_secondary, after_primary)
        for _, index in ipairs({'primary', 'secondary'}) do
            local sample = res[engine][index]
            t.assert_equals(sample.begin_rc, 0)
            t.assert_equals(sample.sample_rc, 0)
            t.assert_equals(sample.finish_rc, 0)
            t.assert_equals(sample.rows, 4)
            t.assert_equals(sample.delivered, 4)
            t.assert_equals(sample.population, 8)
            t.assert_equals(sample.population_known, true)
            t.assert_equals(#sample.ids, 4)
        end
    end
end

g_budget.test_path_solver_width_configuration = function()
    local res = g_budget.server:exec(function()
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20);]])
        local rows = box.execute([[SELECT a FROM t WHERE id = 2;]]).rows
        local stats = box.stat.sql()
        box.execute([[DROP TABLE t;]])
        return {
            rows = rows,
            one = stats.sql_planner_path_solver_width_one,
            two = stats.sql_planner_path_solver_width_two,
            many = stats.sql_planner_path_solver_width_many,
        }
    end)
    t.assert_equals(res.rows, {{20}})
    t.assert_equals(res.one, 2)
    t.assert_equals(res.two, 7)
    t.assert_equals(res.many, 11)
end

g_jit.test_sql_jit_exec_count_growth = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local result = box.execute([[SELECT 1 + 2 + 3 + 4 + 5;]])
        local after = box.stat.sql()
        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_compile_success = after.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            before_fallback = before.sql_jit_fallback_count,
            after_fallback = after.sql_jit_fallback_count,
        }
    end)

    t.assert_equals(res.rows, {{15}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_compile_success, res.before_compile_success)
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    t.assert_ge(res.after_fallback, res.before_fallback)
end

g_jit.test_sql_jit_session_setting = function()
    local res = g_jit.server:exec(function()
        local settings = box.space._session_settings
        local before_value = settings:get('sql_jit').value
        box.execute([[SET SESSION "sql_jit" = false;]])
        local disabled_value = settings:get('sql_jit').value
        local before_disabled = box.stat.sql()
        local disabled = box.execute([[SELECT 1 + 2 + 3 + 4 + 5;]])
        local after_disabled = box.stat.sql()
        box.execute([[SET SESSION "sql_jit" = true;]])
        local restored_value = settings:get('sql_jit').value
        return {
            before_value = before_value,
            disabled_value = disabled_value,
            restored_value = restored_value,
            rows = disabled.rows,
            before_compile = before_disabled.sql_jit_compile_count,
            after_disabled_compile = after_disabled.sql_jit_compile_count,
            before_exec = before_disabled.sql_jit_exec_count,
            after_disabled_exec = after_disabled.sql_jit_exec_count,
        }
    end)

    t.assert_equals(res.before_value, true)
    t.assert_equals(res.disabled_value, false)
    t.assert_equals(res.restored_value, true)
    t.assert_equals(res.rows, {{15}})
    t.assert_equals(res.after_disabled_compile, res.before_compile)
    t.assert_equals(res.after_disabled_exec, res.before_exec)
end

-- Verify that:
--  1. Re-executing the same SQL text reuses the cached JIT code
--     (auto-stmt cache hit → jit_compiled already set → no new compile).
--  2. A different SQL string with the SAME opcode structure also reuses compiled
--     code via the positive shape cache (literal values differ but the native
--     function is identical — it reads p->aOp[i].p1 at runtime).
--  3. A SQL string with a DIFFERENT opcode structure (different opcodes/jumps)
--     triggers an independent new compilation.
g_jit.test_sql_jit_row_shape_negative_cache = function()
    local res = g_jit.server:exec(function()
        -- sql1 and sql2 have the same opcode structure (same Add chain, same shape)
        -- but different literal values.
        local sql1 = [[SELECT 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9;]]
        local sql2 = [[SELECT 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1;]]
        -- sql3 uses Concat instead of Add — genuinely different opcode shape.
        local sql3 = [[SELECT 'a' || 'b' || 'c' || 'd' || 'e' || 'f' || 'g' || 'h' || 'i';]]
        local before = box.stat.sql()
        local first = box.execute(sql1)
        local after_first = box.stat.sql()
        -- Re-run the SAME SQL: auto-stmt cache hit → JIT already compiled → no new compile
        local first_again = box.execute(sql1)
        local after_first_again = box.stat.sql()
        -- Different SQL, SAME opcode shape: positive shape cache hit → no new compile
        local second = box.execute(sql2)
        local after_second = box.stat.sql()
        -- Different SQL, DIFFERENT opcode shape: cache miss → compiled independently
        local third = box.execute(sql3)
        local after_third = box.stat.sql()
        return {
            first = first.rows,
            first_again = first_again.rows,
            second = second.rows,
            third = third.rows,
            before_compile = before.sql_jit_compile_count,
            after_first_compile = after_first.sql_jit_compile_count,
            after_first_again_compile = after_first_again.sql_jit_compile_count,
            after_second_compile = after_second.sql_jit_compile_count,
            after_third_compile = after_third.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_first_compile_success = after_first.sql_jit_compile_success_count,
            after_first_again_compile_success = after_first_again.sql_jit_compile_success_count,
            after_second_compile_success = after_second.sql_jit_compile_success_count,
            after_third_compile_success = after_third.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_first_exec = after_first.sql_jit_exec_count,
            after_first_again_exec = after_first_again.sql_jit_exec_count,
            after_second_exec = after_second.sql_jit_exec_count,
            after_third_exec = after_third.sql_jit_exec_count,
        }
    end)

    t.assert_equals(res.first, {{45}})
    t.assert_equals(res.first_again, {{45}})
    t.assert_equals(res.second, {{45}})
    if res.after_first_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    -- First execution: compiles and runs via JIT
    t.assert_gt(res.after_first_compile_success, res.before_compile_success)
    t.assert_gt(res.after_first_exec, res.before_exec)
    -- Second execution of SAME SQL: auto-stmt cache hit → no recompile
    t.assert_equals(res.after_first_again_compile, res.after_first_compile)
    t.assert_equals(res.after_first_again_compile_success,
                    res.after_first_compile_success)
    t.assert_gt(res.after_first_again_exec, res.after_first_exec)
    -- Different SQL, same opcode shape: positive shape cache hit → no recompile
    t.assert_equals(res.after_second_compile, res.after_first_again_compile,
                    'same-shape SQL should be served from the positive shape cache')
    t.assert_equals(res.after_second_compile_success,
                    res.after_first_again_compile_success)
    t.assert_gt(res.after_second_exec, res.after_first_again_exec)
    -- Different SQL with a different opcode shape: triggers new compilation
    t.assert_gt(res.after_third_compile, res.after_second_compile,
                'different-shape SQL should trigger independent JIT compilation')
    t.assert_gt(res.after_third_compile_success,
                res.after_second_compile_success)
    t.assert_gt(res.after_third_exec, res.after_second_exec)
end

g_jit.test_sql_jit_prepare_forces_small_statement = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local stmt = box.prepare([[SELECT 1 + 2;]])
        local after_prepare = box.stat.sql()
        local result = box.execute(stmt.stmt_id)
        local after_execute = box.stat.sql()
        box.unprepare(stmt.stmt_id)
        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_prepare_compile = after_prepare.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_prepare_compile_success = after_prepare.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_prepare_exec = after_prepare.sql_jit_exec_count,
            after_execute_exec = after_execute.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_execute_steps = after_execute.sql_jit_step_count,
        }
    end)

    t.assert_equals(res.rows, {{3}})
    if res.after_prepare_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_prepare_compile_success, res.before_compile_success)
    t.assert_equals(res.after_prepare_exec, res.before_exec)
    t.assert_gt(res.after_execute_exec, res.after_prepare_exec)
    t.assert_gt(res.after_execute_steps, res.before_steps)
end

g_jit.test_sql_jit_control_flow_opcodes = function()
    local res = g_jit.server:exec(function()
        local before = box.stat.sql()
        local if_stmt = box.prepare([[
            SELECT CASE WHEN NOT ? THEN 10 ELSE 20 END + 1 + 2 + 3;
        ]])
        local ifnot_stmt = box.prepare([[
            SELECT CASE WHEN ? THEN 10 ELSE 20 END + 1 + 2 + 3;
        ]])
        local is_null_stmt = box.prepare([[
            SELECT 1 WHERE ? IS NOT NULL AND 1 + 2 + 3 + 4 = 10;
        ]])
        local not_null_stmt = box.prepare([[
            SELECT 1 WHERE ? IS NULL AND 1 + 2 + 3 + 4 = 10;
        ]])
        local results = {
            if_op = box.execute(if_stmt.stmt_id, {false}).rows,
            ifnot_op = box.execute(ifnot_stmt.stmt_id, {true}).rows,
            is_null_op = box.execute(is_null_stmt.stmt_id, {1}).rows,
            not_null_op = box.execute(not_null_stmt.stmt_id, {box.NULL}).rows,
        }
        local after = box.stat.sql()
        box.unprepare(if_stmt.stmt_id)
        box.unprepare(ifnot_stmt.stmt_id)
        box.unprepare(is_null_stmt.stmt_id)
        box.unprepare(not_null_stmt.stmt_id)
        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end
        return {
            results = results,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.results.if_op, {{16}})
    t.assert_equals(res.results.ifnot_op, {{16}})
    t.assert_equals(res.results.is_null_op, {{1}})
    t.assert_equals(res.results.not_null_op, {{1}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    if res.profile ~= nil then
        local before = res.profile.before
        local after = res.profile.after
        local before_total =
            opcode_count(before, 'If') +
            opcode_count(before, 'IfNot') +
            opcode_count(before, 'IsNull', 'NotUsed_174') +
            opcode_count(before, 'NotNull', 'NotUsed_175')
        local after_total =
            opcode_count(after, 'If') +
            opcode_count(after, 'IfNot') +
            opcode_count(after, 'IsNull', 'NotUsed_174') +
            opcode_count(after, 'NotNull', 'NotUsed_175')
        t.assert_gt(after_total, before_total)
    end
end

g_jit.test_sql_jit_column_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT);]])
        box.execute([[INSERT INTO t VALUES (2, 20, 200);]])

        local before = box.stat.sql()
        local result = box.execute([[SELECT a + b + id + id FROM t;]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{224}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.Column or 0, res.profile.before.Column or 0)
    end
end

g_jit.test_sql_jit_materialization_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE dst (id INT PRIMARY KEY, a INT, b INT);]])
        box.execute([[INSERT INTO dst VALUES (1, 10, 100);]])

        local before = box.stat.sql()
        box.execute([[UPDATE dst SET a = a + 1;]])
        local after = box.stat.sql()
        local updated = box.execute([[SELECT id, a, b FROM dst;]]).rows

        box.execute([[DROP TABLE dst;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            updated = updated,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.updated, {{1, 11, 100}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.ApplyType or 0,
                    res.profile.before.ApplyType or 0)
        t.assert_gt(res.profile.after.MakeRecord or 0,
                    res.profile.before.MakeRecord or 0)
        t.assert_gt(res.profile.after.RowData or 0,
                    res.profile.before.RowData or 0)
    end
end

g_jit.test_sql_jit_aggregate_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE src (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO src VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        local result = box.execute([[SELECT sum(a + id + 1) FROM src;]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE src;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{69}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.AggStep or 0, res.profile.before.AggStep or 0)
        t.assert_gt(res.profile.after.AggFinal or 0, res.profile.before.AggFinal or 0)
    end
end

g_jit.test_sql_jit_control_flow_round2_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40);]])

        local before = box.stat.sql()
        local offset_stmt = box.prepare([[
            SELECT 100 + 1
            WHERE EXISTS(SELECT id + a + 1 FROM t LIMIT 2 OFFSET 1);
        ]])
        local once_stmt = box.prepare([[
            SELECT 100 + 1
            WHERE EXISTS(SELECT (SELECT 40 + 2) + id FROM t LIMIT 1);
        ]])
        local offset_result = box.execute(offset_stmt.stmt_id).rows
        local once_result = box.execute(once_stmt.stmt_id).rows
        local after = box.stat.sql()
        box.unprepare(offset_stmt.stmt_id)
        box.unprepare(once_stmt.stmt_id)

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            offset_result = offset_result,
            once_result = once_result,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.offset_result, {{101}})
    t.assert_equals(res.once_result, {{101}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.MustBeInt or 0,
                    res.profile.before.MustBeInt or 0)
        t.assert_gt(res.profile.after.OffsetLimit or 0,
                    res.profile.before.OffsetLimit or 0)
        t.assert_gt(res.profile.after.IfPos or 0,
                    res.profile.before.IfPos or 0)
        t.assert_gt(res.profile.after.Once or 0,
                    res.profile.before.Once or 0)
        t.assert_gt(res.profile.after.DecrJumpZero or 0,
                    res.profile.before.DecrJumpZero or 0)
    end
end

g_jit.test_sql_jit_cast_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a TEXT, b INT);]])
        box.execute([[INSERT INTO t VALUES
            (1, '10', 100),
            (2, '20', 200),
            (3, '30', 300);
        ]])

        local before = box.stat.sql()
        local result = box.execute([[
            SELECT CAST(a AS INTEGER) + b + id + id FROM t;
        ]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{112}, {224}, {336}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.Cast or 0, res.profile.before.Cast or 0)
    end
end

g_jit.test_sql_jit_coroutine_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        local stmt = box.prepare([[
            SELECT sum(x + 1)
            FROM (SELECT id + a + 1 AS x FROM t LIMIT 2);
        ]])
        local result = box.execute(stmt.stmt_id)
        local after = box.stat.sql()
        box.unprepare(stmt.stmt_id)

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{37}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.InitCoroutine or 0,
                    res.profile.before.InitCoroutine or 0)
        t.assert_gt(res.profile.after.Yield or 0,
                    res.profile.before.Yield or 0)
        t.assert_gt(res.profile.after.EndCoroutine or 0,
                    res.profile.before.EndCoroutine or 0)
    end
end

g_jit.test_sql_jit_sorter_gosub_return_opcodes = function()
    local res = g_jit.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])

        local before = box.stat.sql()
        local result = box.execute([[
            SELECT sum(x)
            FROM (
                SELECT id + a + 1 AS x FROM t
                UNION ALL
                SELECT id + a + 2 FROM t
                ORDER BY 1
            );
        ]])
        local after = box.stat.sql()

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            rows = result.rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.rows, {{141}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.Permutation or 0,
                    res.profile.before.Permutation or 0)
        t.assert_gt(res.profile.after.Compare or 0,
                    res.profile.before.Compare or 0)
        t.assert_gt(res.profile.after.SorterNext or 0,
                    res.profile.before.SorterNext or 0)
        t.assert_gt(res.profile.after.Gosub or 0,
                    res.profile.before.Gosub or 0)
        t.assert_gt(res.profile.after.Return or 0,
                    res.profile.before.Return or 0)
    end
end

g_jit.test_sql_jit_ttransaction_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])

        local before = box.stat.sql()
        local insert_result = box.execute([[INSERT INTO t VALUES (1, 10);]])
        local after = box.stat.sql()
        local rows = box.execute([[SELECT id, a FROM t;]]).rows

        box.execute([[DROP TABLE t;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            row_count = insert_result.row_count,
            rows = rows,
            before_compile = before.sql_jit_compile_count,
            after_compile = after.sql_jit_compile_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            profile = profile,
        }
    end)

    t.assert_equals(res.row_count, 1)
    t.assert_equals(res.rows, {{1, 10}})
    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_gt(res.after_exec, res.before_exec)
    t.assert_gt(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_gt(res.profile.after.TTransaction or 0,
                    res.profile.before.TTransaction or 0)
    end
end

g_jit.test_sql_jit_program_opcode = function()
    local res = g_jit.server:exec(function()
        box.execute([[CREATE TABLE t1(x INTEGER PRIMARY KEY);]])
        box.execute([[CREATE TABLE t2(y INTEGER PRIMARY KEY);]])
        box.execute([[
            CREATE TRIGGER tr AFTER INSERT ON t1 FOR EACH ROW
            BEGIN
                INSERT INTO t2 VALUES(new.x + 1);
            END;
        ]])

        local before = box.stat.sql()
        local stmt = box.prepare([[INSERT INTO t1 VALUES(10);]])
        local after_prepare = box.stat.sql()
        box.execute(stmt.stmt_id)
        local after = box.stat.sql()
        local rows = box.execute([[SELECT y FROM t2 WHERE y = 11;]]).rows
        box.unprepare(stmt.stmt_id)

        box.execute([[DROP TABLE t1;]])
        box.execute([[DROP TABLE t2;]])

        local profile = nil
        if before.sql_opcode_profile_enabled ~= 0 then
            profile = {
                before = before.jit_opcode_profile.count,
                after = after.jit_opcode_profile.count,
            }
        end

        return {
            before_compile = before.sql_jit_compile_count,
            after_compile = after_prepare.sql_jit_compile_count,
            before_compile_success = before.sql_jit_compile_success_count,
            after_compile_success = after_prepare.sql_jit_compile_success_count,
            before_exec = before.sql_jit_exec_count,
            after_exec = after.sql_jit_exec_count,
            before_steps = before.sql_jit_step_count,
            after_steps = after.sql_jit_step_count,
            rows = rows,
            profile = profile,
        }
    end)

    if res.after_compile == res.before_compile then
        t.skip('SQL JIT is not available in this build')
    end
    t.assert_equals(res.rows, {{11}})
    t.assert_equals(res.after_compile_success, res.before_compile_success)
    t.assert_equals(res.after_exec, res.before_exec)
    t.assert_equals(res.after_steps, res.before_steps)
    if res.profile ~= nil then
        t.assert_equals(res.profile.after.Program or 0,
                        res.profile.before.Program or 0)
        t.assert_equals(res.profile.after.Param or 0,
                        res.profile.before.Param or 0)
    end
end

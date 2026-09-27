local server = require('luatest.server')
local t = require('luatest')

local g = t.group('planner_final_paths')

g.before_all(function()
    g.server = server:new({alias = 'planner_final_paths'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_snapshot_contains_complete_final_single_relation_paths = function()
    local result = g.server:exec(function()
        local msgpack = require('msgpack')
        local space = box.schema.space.create('planner_final_paths_t')
        space:format({
            {name = 'id', type = 'unsigned'},
            {name = 'value', type = 'unsigned'},
        })
        space:create_index('primary', {
            parts = {{field = 'id', type = 'unsigned'}},
        })
        space:create_index('by_value', {
            unique = false,
            parts = {{field = 'value', type = 'unsigned'}},
        })
        for i = 1, 8 do
            space:insert({i, i % 3})
        end
        local direct_scan = box.execute([[SELECT id FROM planner_final_paths_t
            WHERE id = 1]])
        assert(direct_scan ~= nil and #direct_scan.rows == 1)
        box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
        local new_planner_scan = box.execute(
            [[SELECT id FROM planner_final_paths_t WHERE id = 1]])
        assert(new_planner_scan ~= nil and #new_planner_scan.rows == 1)
        box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
        local direct_values = box.execute([[VALUES (1), (2)]])
        assert(direct_values ~= nil and #direct_values.rows == 2)
        local direct_strings = box.execute([[VALUES ('alpha'), ('beta')]])
        assert(direct_strings ~= nil and
               direct_strings.rows[1][1] == 'alpha' and
               direct_strings.rows[2][1] == 'beta')
        local direct_count = box.execute(
            [[SELECT count(*) FROM planner_final_paths_t]])
        assert(direct_count ~= nil and direct_count.rows[1][1] == 8)
        local direct_compound = box.execute([[SELECT id FROM
            planner_final_paths_t WHERE id < 3 UNION ALL SELECT id FROM
            planner_final_paths_t WHERE id > 6]])
        assert(direct_compound ~= nil and #direct_compound.rows == 4)

        local point_explain = box.execute(
            [[EXPLAIN (planner = 'snapshot')
              SELECT id FROM planner_final_paths_t WHERE id = 1]])
        local point = msgpack.decode(tostring(point_explain.rows[1][1]))
        local explain, err = box.execute([[EXPLAIN (planner = 'snapshot')
            SELECT id FROM planner_final_paths_t
            WHERE value >= 1 ORDER BY value]])
        assert(err == nil, err and err.message)
        local snapshot = msgpack.decode(tostring(explain.rows[1][1]))
        local snapshot_bytes = tostring(explain.rows[1][1])
        local paths = snapshot.planner.final_paths
        local join_explain = box.execute(
            [[EXPLAIN (planner = 'snapshot')
              SELECT a.id FROM planner_final_paths_t AS a
              JOIN planner_final_paths_t AS b ON a.id = b.id]])
        local join = msgpack.decode(tostring(join_explain.rows[1][1]))
        local values_explain = box.execute(
            [[EXPLAIN (planner = 'snapshot') VALUES (1), (2)]])
        local values = msgpack.decode(tostring(values_explain.rows[1][1]))
        local count_explain = box.execute([[EXPLAIN (planner = 'snapshot')
            SELECT count(*) FROM planner_final_paths_t]])
        local count = msgpack.decode(tostring(count_explain.rows[1][1]))
        local compound_explain = box.execute([[EXPLAIN (planner = 'snapshot')
            SELECT id FROM planner_final_paths_t WHERE id < 3
            UNION ALL
            SELECT id FROM planner_final_paths_t WHERE id > 6]])
        local compound = msgpack.decode(
            tostring(compound_explain.rows[1][1]))
        local cte_explain = box.execute([[EXPLAIN (planner = 'snapshot')
            WITH RECURSIVE r(x) AS (
                VALUES (1) UNION ALL SELECT x + 1 FROM r WHERE x < 3
            ) SELECT x FROM r]])
        local cte = msgpack.decode(tostring(cte_explain.rows[1][1]))
        local from_subquery_explain = box.execute(
            [[EXPLAIN (planner = 'snapshot') SELECT id FROM
              (SELECT id FROM planner_final_paths_t) AS q]])
        local from_subquery = msgpack.decode(
            tostring(from_subquery_explain.rows[1][1]))
        local scalar_explain = box.execute([[EXPLAIN (planner = 'snapshot')
            SELECT (SELECT max(id) FROM planner_final_paths_t)]])
        local scalar = msgpack.decode(tostring(scalar_explain.rows[1][1]))
        local result = {
            version = snapshot.version,
            component_status = snapshot.planner.component_status,
            component_count = #snapshot.planner.component_routes,
            root_component_route = snapshot.planner.component_routes[1].route,
            replayable = snapshot.replayable,
            replay_inputs = snapshot.replay_inputs,
            status = snapshot.planner.final_path_status,
            count = #paths,
            point_status = point.planner.final_path_status,
            point_count = #point.planner.final_paths,
            join_status = join.planner.final_path_status,
            join_count = #join.planner.final_paths,
            join_component_status = join.planner.component_status,
            join_component_count = #join.planner.component_routes,
            join_component_route = join.planner.component_routes[1].route,
            values_component_status = values.planner.component_status,
            values_component_count = #values.planner.component_routes,
            values_component_route = values.planner.component_routes[1].route,
            count_component_status = count.planner.component_status,
            count_component_route = count.planner.component_routes[1].route,
            compound_component_status = compound.planner.component_status,
            compound_component_count =
                #compound.planner.component_routes,
            compound_component_root_route =
                compound.planner.component_routes[1].route,
            compound_path_class = compound.path_class,
            cte_component_status = cte.planner.component_status,
            cte_component_count = #cte.planner.component_routes,
            cte_root_route = cte.planner.component_routes[1].route,
            from_subquery_component_status =
                from_subquery.planner.component_status,
            from_subquery_component_count =
                #from_subquery.planner.component_routes,
            scalar_component_status = scalar.planner.component_status,
            scalar_component_count = #scalar.planner.component_routes,
            selected_fingerprint =
                snapshot.planner.selected_final_path_fingerprint,
            snapshot_bytes = snapshot_bytes,
        }
        local best = paths[1]
        for i = 2, #paths do
            if paths[i].path_cost_logest < best.path_cost_logest then
                best = paths[i]
            end
        end
        result.selected_matches_min = best ~= nil and
            best.fingerprint == result.selected_fingerprint
        if snapshot.replay_inputs ~= nil then
            local input = msgpack.decode(tostring(snapshot.replay_inputs))
            result.input_version = input.version
            result.input_selector_version =
                input.planner.final_path_selector_version
            result.input_final_path_count = #input.final_path_candidates
            result.input_paths_match_capture = true
            for i, path in ipairs(paths) do
                if input.final_path_candidates[i].plan_fingerprint ~=
                   path.fingerprint then
                    result.input_paths_match_capture = false
                    break
                end
            end
        end
        for _, path in ipairs(paths) do
            if type(path.fingerprint) ~= 'string' or
               #path.fingerprint ~= 16 or
               type(path.path_cost_logest) ~= 'number' or
               type(path.unsorted_cost_logest) ~= 'number' or
               type(path.output_rows_logest) ~= 'number' or
               type(path.is_ordered) ~= 'number' or
               type(path.reverse_mask) ~= 'number' then
                result.paths_valid = false
                break
            end
            result.paths_valid = true
        end
        space:drop()
        local replay = require('sql_replay')
        local replayed = replay.replay_snapshot(snapshot_bytes)
        result.replayed_fingerprint = replayed.fingerprint
        result.replay_matches_captured = replayed.matches_captured

        -- Mutating only captured costs must change selection; no live SQL
        -- relation or statistics are consulted by the replay API.
        local changed = msgpack.decode(snapshot_bytes)
        local changed_input =
            msgpack.decode(tostring(changed.replay_inputs))
        local paths = changed_input.final_path_candidates
        for _, path in ipairs(paths) do
            path.path_cost_logest = 32767
        end
        local altered = paths[1]
        paths[#paths + 1] = {
            is_ordered = altered.is_ordered,
            output_rows_logest = altered.output_rows_logest,
            path_cost_logest = -32768,
            plan_fingerprint = '0000000000000000',
            reverse_mask = altered.reverse_mask,
            unsorted_cost_logest = altered.unsorted_cost_logest,
        }
        assert(paths[#paths].plan_fingerprint ~= replayed.fingerprint)
        local counterfactual = replay.replay_input(
            msgpack.encode(changed_input))
        result.counterfactual_fingerprint = counterfactual.fingerprint
        changed.replay_inputs = msgpack.encode(changed_input)
        local changed_ok = pcall(replay.replay_snapshot,
                                 msgpack.encode(changed))
        result.changed_artifact_rejected = not changed_ok
        return result
    end)

    t.assert_equals(result.version, 5)
    t.assert_equals(result.component_status, 'complete')
    t.assert_equals(result.component_count, 1)
    t.assert_equals(result.root_component_route, 'current_where_c')
    t.assert_equals(result.replayable, true)
    t.assert_equals(result.input_version, 5)
    t.assert_equals(result.input_selector_version, 1)
    t.assert_equals(result.input_final_path_count, result.count)
    t.assert_equals(result.selected_matches_min, true)
    t.assert_equals(result.replayed_fingerprint,
                    result.selected_fingerprint)
    t.assert_equals(result.replay_matches_captured, true)
    t.assert_equals(result.counterfactual_fingerprint, '0000000000000000')
    t.assert_equals(result.changed_artifact_rejected, true)
    t.assert_equals(result.input_paths_match_capture, true)
    t.assert_equals(result.status, 'complete')
    t.assert_gt(result.count, 0)
    t.assert_equals(result.paths_valid, true)
    t.assert_equals(result.point_status, 'complete')
    t.assert_gt(result.point_count, 0)
    t.assert_equals(result.join_status, 'unavailable')
    t.assert_equals(result.join_count, 0)
    t.assert_equals(result.join_component_status, 'complete')
    t.assert_equals(result.join_component_count, 1)
    t.assert_equals(result.join_component_route, 'fallback')
    t.assert_equals(result.values_component_status, 'complete')
    t.assert_equals(result.values_component_count, 3)
    t.assert_equals(result.values_component_route, 'direct_values')
    t.assert_equals(result.count_component_status, 'complete')
    t.assert_equals(result.count_component_route, 'direct_op_count')
    t.assert_equals(result.compound_component_status, 'complete')
    t.assert_gt(result.compound_component_count, 1)
    t.assert_equals(result.compound_component_root_route,
                    'compound_dispatch')
    t.assert_equals(result.compound_path_class, 'mixed')
    t.assert_equals(result.cte_component_status, 'complete')
    t.assert_gt(result.cte_component_count, 1)
    t.assert_equals(result.cte_root_route, 'fallback')
    t.assert_equals(result.from_subquery_component_status, 'complete')
    t.assert_gt(result.from_subquery_component_count, 0)
    t.assert_equals(result.scalar_component_status, 'complete')
    t.assert_gt(result.scalar_component_count, 0)
end

g.test_snapshot_component_ledger_covers_producer_matrix = function()
    local snapshots = g.server:exec(function()
        local msgpack = require('msgpack')
        box.execute([[CREATE TABLE planner_component_matrix (
            id INTEGER PRIMARY KEY, v INTEGER)]])
        box.execute([[INSERT INTO planner_component_matrix VALUES
            (1, 10), (2, 10), (3, 30), (4, NULL)]])
        local queries = {
            constant = [[SELECT 1]],
            distinct = [[SELECT DISTINCT v FROM planner_component_matrix]],
            grouped = [[SELECT v, count(*) FROM planner_component_matrix
                        GROUP BY v]],
            aggregate = [[SELECT min(v), max(v)
                          FROM planner_component_matrix]],
            scalar_exists = [[SELECT EXISTS(
                SELECT 1 FROM planner_component_matrix WHERE id = 1)]],
            nested_function = [[SELECT (SELECT abs(id)
                FROM planner_component_matrix WHERE id = 1)
                FROM planner_component_matrix]],
            nested_destination = [[SELECT (SELECT id
                FROM planner_component_matrix)
                FROM planner_component_matrix]],
            scalar_direct_count = [[SELECT (SELECT count(*)
                FROM planner_component_matrix)
                FROM planner_component_matrix]],
            union = [[SELECT id FROM planner_component_matrix WHERE id = 1
                      UNION ALL
                      SELECT id FROM planner_component_matrix WHERE id = 3]],
            intersect = [[SELECT id FROM planner_component_matrix
                          INTERSECT
                          SELECT id FROM planner_component_matrix WHERE id > 1]],
            recursive_cte = [[WITH RECURSIVE r(x) AS (
                VALUES (1) UNION ALL SELECT x + 1 FROM r WHERE x < 3
            ) SELECT x FROM r]],
            single_values = [[EXPLAIN (planner = 'snapshot') SELECT 1]],
            direct_null_filter = [[SELECT id FROM planner_component_matrix
                                   WHERE v IS NULL]],
            direct_null_range_filter = [[SELECT id FROM
                planner_component_matrix WHERE v IS NOT NULL AND id > 1
                ORDER BY id ASC]],
            unsupported_null_filter = [[SELECT id FROM
                planner_component_matrix WHERE v IS NULL AND
                v IS NOT NULL]],
        }
        local result = {}
        for name, sql in pairs(queries) do
            if name == 'nested_destination' or
               name == 'direct_null_filter' or
               name == 'direct_null_range_filter' or
               name == 'unsupported_null_filter' then
                box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
            end
            local explain_sql = sql
            if name ~= 'single_values' then
                explain_sql = [[EXPLAIN (planner = 'snapshot') ]] .. sql
            end
            local explain, err = box.execute(explain_sql)
            assert(err == nil, err and err.message)
            local snapshot = msgpack.decode(tostring(explain.rows[1][1]))
            local components = snapshot.planner.component_routes
            local item = {
                status = snapshot.planner.component_status,
                count = #components,
                fallback_count = snapshot.planner.fallback_count,
                path_class = snapshot.path_class,
                fallback_reason = snapshot.fallback_reason,
                routes = {},
                component_routes = {},
                roles = {},
            }
            local ids = {}
            for _, component in ipairs(components) do
                ids[component.id] = true
                table.insert(item.routes, component.route)
                item.roles[component.role] = true
                table.insert(item.component_routes, {
                    id = component.id,
                    parent_id = component.parent_id,
                    role = component.role,
                    route = component.route,
                    fallback_reason = component.fallback_reason,
                })
                assert(component.route ~= 'pending', name .. ' has pending route')
            end
            for _, component in ipairs(components) do
                assert(component.parent_id == 0 or ids[component.parent_id],
                       name .. ' has missing component parent')
            end
            result[name] = item
            if name == 'nested_destination' or
               name == 'direct_null_filter' or
               name == 'direct_null_range_filter' or
               name == 'unsupported_null_filter' then
                box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
            end
        end
        box.execute([[DROP TABLE planner_component_matrix]])
        return result
    end)
    for name, snapshot in pairs(snapshots) do
        t.assert_equals(snapshot.status, 'complete', name)
        t.assert_gt(snapshot.count, 0, name)
    end
    t.assert_equals(snapshots.constant.routes[1], 'fallback')
    t.assert_equals(snapshots.direct_null_filter.path_class, 'new_planner')
    t.assert_equals(snapshots.direct_null_filter.routes[1], 'new_planner')
    t.assert_equals(snapshots.direct_null_range_filter.path_class,
                    'new_planner')
    t.assert_equals(snapshots.direct_null_range_filter.routes[1],
                    'new_planner')
    t.assert_equals(snapshots.unsupported_null_filter.path_class, 'fallback')
    t.assert_equals(snapshots.unsupported_null_filter.fallback_reason,
                    'UNSUPPORTED_FILTER')
    t.assert_equals(snapshots.unsupported_null_filter.routes[1], 'fallback')
    t.assert_gt(snapshots.union.count, 1)
    t.assert_gt(snapshots.intersect.count, 1)
    t.assert_gt(snapshots.scalar_exists.count, 1)
    local nested_routes = snapshots.nested_function.component_routes
    t.assert_gt(#nested_routes, 1)
    t.assert_equals(nested_routes[1].role, 'root')
    t.assert_equals(nested_routes[1].route, 'fallback')
    t.assert_equals(nested_routes[1].fallback_reason, 'UNSUPPORTED_SUBQUERY')
    local has_nested_function_reject = false
    for _, component in ipairs(nested_routes) do
        if component.parent_id == nested_routes[1].id then
            t.assert_equals(component.route, 'fallback')
            t.assert_equals(component.fallback_reason, 'UNSUPPORTED_FUNCTION')
            has_nested_function_reject = true
        end
    end
    t.assert(has_nested_function_reject)
    local destination_routes = snapshots.nested_destination.component_routes
    t.assert_gt(#destination_routes, 1)
    t.assert_equals(destination_routes[1].role, 'root')
    t.assert_equals(destination_routes[1].fallback_reason,
                    'UNSUPPORTED_SUBQUERY')
    t.assert_equals(destination_routes[2].role, 'subquery')
    t.assert_equals(destination_routes[2].route, 'fallback')
    t.assert_equals(destination_routes[2].fallback_reason,
                    'UNSUPPORTED_DESTINATION')
    t.assert_equals(snapshots.nested_destination.path_class, 'fallback')
    t.assert_equals(snapshots.nested_destination.fallback_reason,
                    'UNSUPPORTED_SUBQUERY')
    t.assert_equals(snapshots.nested_destination.fallback_count, 2)
    local scalar_count = snapshots.scalar_direct_count.component_routes
    t.assert_equals(snapshots.scalar_direct_count.path_class, 'mixed')
    t.assert_equals(snapshots.scalar_direct_count.fallback_reason, nil)
    t.assert_equals(scalar_count[1].role, 'root')
    t.assert_equals(scalar_count[1].route, 'fallback')
    t.assert_equals(scalar_count[2].parent_id, scalar_count[1].id)
    t.assert_equals(scalar_count[2].role, 'subquery')
    t.assert_equals(scalar_count[2].route, 'direct_op_count')
    t.assert_gt(snapshots.recursive_cte.count, 2)
    t.assert(snapshots.recursive_cte.roles.recursive_term)
    t.assert(snapshots.recursive_cte.roles.values)
    local recursive_routes = snapshots.recursive_cte.component_routes
    local has_values_anchor = false
    local has_recursive_term_fallback = false
    for _, component in ipairs(recursive_routes) do
        if component.role == 'values' then
            has_values_anchor = true
            t.assert_equals(component.route, 'direct_values')
        elseif component.role == 'recursive_term' then
            has_recursive_term_fallback = true
            t.assert_equals(component.route, 'fallback')
            t.assert_equals(component.fallback_reason,
                            'UNSUPPORTED_COMPOUND')
        end
    end
    t.assert(has_values_anchor)
    t.assert(has_recursive_term_fallback)
end

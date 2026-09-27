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

        local point_explain = box.execute(
            [[EXPLAIN (planner = 'snapshot')
              SELECT id FROM planner_final_paths_t WHERE id = 1]])
        local point = msgpack.decode(tostring(point_explain.rows[1][1]))
        local explain, err = box.execute([[EXPLAIN (planner = 'snapshot')
            SELECT id FROM planner_final_paths_t
            WHERE value >= 1 ORDER BY value]])
        assert(err == nil, err and err.message)
        local snapshot = msgpack.decode(tostring(explain.rows[1][1]))
        local paths = snapshot.planner.final_paths
        local join_explain = box.execute(
            [[EXPLAIN (planner = 'snapshot')
              SELECT a.id FROM planner_final_paths_t AS a
              JOIN planner_final_paths_t AS b ON a.id = b.id]])
        local join = msgpack.decode(tostring(join_explain.rows[1][1]))
        local result = {
            version = snapshot.version,
            replayable = snapshot.replayable,
            replay_inputs = snapshot.replay_inputs,
            status = snapshot.planner.final_path_status,
            count = #paths,
            point_status = point.planner.final_path_status,
            point_count = #point.planner.final_paths,
            join_status = join.planner.final_path_status,
            join_count = #join.planner.final_paths,
            selected_fingerprint =
                snapshot.planner.selected_final_path_fingerprint,
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
        return result
    end)

    t.assert_equals(result.version, 4)
    t.assert_equals(result.replayable, true)
    t.assert_equals(result.input_version, 5)
    t.assert_equals(result.input_selector_version, 1)
    t.assert_equals(result.input_final_path_count, result.count)
    t.assert_equals(result.selected_matches_min, true)
    t.assert_equals(result.input_paths_match_capture, true)
    t.assert_equals(result.status, 'complete')
    t.assert_gt(result.count, 0)
    t.assert_equals(result.paths_valid, true)
    t.assert_equals(result.point_status, 'complete')
    t.assert_gt(result.point_count, 0)
    t.assert_equals(result.join_status, 'unavailable')
    t.assert_equals(result.join_count, 0)
end

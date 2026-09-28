local server = require('luatest.server')
local t = require('luatest')

local g = t.group('planner_insert_select_snapshot')

g.before_all(function()
    g.server = server:new({alias = 'planner_insert_select_snapshot'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_insert_select_has_embedded_root_route = function()
    local result = g.server:exec(function()
        local msgpack = require('msgpack')
        box.execute([[CREATE TABLE planner_insert_source (
            id INTEGER PRIMARY KEY, value INTEGER)]])
        box.execute([[CREATE TABLE planner_insert_target (
            id INTEGER PRIMARY KEY, value INTEGER)]])
        box.execute([[INSERT INTO planner_insert_source VALUES (1, 10),
            (2, 20)]])

        local explain, err = box.execute([[EXPLAIN (planner = 'snapshot')
            INSERT INTO planner_insert_target
            SELECT id, value FROM planner_insert_source WHERE id > 0]])
        assert(err == nil, err and err.message)
        local snapshot = msgpack.decode(tostring(explain.rows[1][1]))
        local routes = snapshot.planner.component_routes
        local root = routes[1]
        assert(snapshot.planner.component_status == 'complete')
        assert(#routes == 1)
        assert(root.id == 1 and root.parent_id == 0)
        assert(root.role == 'insert_select_root')
        assert(root.route ~= 'fallback', root.fallback_reason)
        assert(snapshot.path_class == root.route)

        local inserted, insert_err = box.execute([[INSERT INTO
            planner_insert_target
            SELECT id, value FROM planner_insert_source WHERE id > 0]])
        assert(insert_err == nil, insert_err and insert_err.message)
        local rows = box.execute([[SELECT * FROM planner_insert_target
            ORDER BY id]])
        assert(#rows.rows == 2)
        box.execute([[DROP TABLE planner_insert_target]])
        box.execute([[DROP TABLE planner_insert_source]])
        return {role = root.role, route = root.route,
                component_status = snapshot.planner.component_status}
    end)
    t.assert_equals(result.component_status, 'complete')
    t.assert_equals(result.role, 'insert_select_root')
    t.assert_not_equals(result.route, 'fallback')
end

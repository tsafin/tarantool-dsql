local server = require('luatest.server')
local t = require('luatest')

local g = t.group('planner_insert_select_snapshot')

g.before_all(function()
    g.server = server:new({alias = 'm35_ins_sel'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_insert_select_has_embedded_root_route = function()
    local result = g.server:exec(function()
        local msgpack = require('msgpack')
        box.execute([[SET SESSION "sql_seq_scan" = true]])
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
        local rows, rows_err = box.execute([[SELECT * FROM
            planner_insert_target ORDER BY id]])
        assert(rows_err == nil and rows ~= nil,
               rows_err and rows_err.message or 'SELECT returned no result')
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

g.test_delete_view_materialization_has_embedded_root_route = function()
    local result = g.server:exec(function()
        local msgpack = require('msgpack')
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        box.execute([[CREATE TABLE planner_view_source (
            id INTEGER PRIMARY KEY, value INTEGER)]])
        box.execute([[INSERT INTO planner_view_source VALUES (1, 10),
            (2, 20)]])
        box.execute([[CREATE VIEW planner_view AS
            SELECT id, value FROM planner_view_source]])
        box.execute([[CREATE TRIGGER planner_view_delete
            INSTEAD OF DELETE ON planner_view FOR EACH ROW BEGIN
                DELETE FROM planner_view_source WHERE id = OLD.id;
            END]])

        local explain, err = box.execute([[EXPLAIN (planner = 'snapshot')
            DELETE FROM planner_view WHERE id = 2]])
        assert(err == nil, err and err.message)
        local snapshot = msgpack.decode(tostring(explain.rows[1][1]))
        local matching = nil
        for _, route in ipairs(snapshot.planner.component_routes) do
            if route.role == 'dml_view_materialization_root' then
                matching = route
            end
        end
        assert(snapshot.planner.component_status == 'complete')
        assert(matching ~= nil, 'missing embedded view materialization route')
        assert(matching.parent_id == 0)
        assert(matching.route == 'fallback')
        assert(matching.fallback_reason == 'UNSUPPORTED_SUBQUERY')

        local deleted, delete_err = box.execute(
            [[DELETE FROM planner_view WHERE id = 2]])
        assert(delete_err == nil and deleted ~= nil,
               delete_err and delete_err.message or 'DELETE returned no result')
        local rows, rows_err = box.execute(
            [[SELECT id FROM planner_view_source ORDER BY id]])
        assert(rows_err == nil and rows ~= nil,
               rows_err and rows_err.message or 'SELECT returned no result')
        assert(#rows.rows == 1 and rows.rows[1][1] == 1)
        box.execute([[DROP VIEW planner_view]])
        box.execute([[DROP TABLE planner_view_source]])
        return matching.role
    end)
    t.assert_equals(result, 'dml_view_materialization_root')
end

g.test_update_view_materialization_has_embedded_root_route = function()
    local result = g.server:exec(function()
        local msgpack = require('msgpack')
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        box.execute([[CREATE TABLE planner_update_view_source (
            id INTEGER PRIMARY KEY, value INTEGER)]])
        box.execute([[INSERT INTO planner_update_view_source VALUES (1, 10),
            (2, 20)]])
        box.execute([[CREATE VIEW planner_update_view AS
            SELECT id, value FROM planner_update_view_source]])
        box.execute([[CREATE TRIGGER planner_update_view_update
            INSTEAD OF UPDATE ON planner_update_view FOR EACH ROW BEGIN
                UPDATE planner_update_view_source SET value = NEW.value
                WHERE id = OLD.id;
            END]])

        local explain, err = box.execute([[EXPLAIN (planner = 'snapshot')
            UPDATE planner_update_view SET value = 25 WHERE id = 2]])
        assert(err == nil, err and err.message)
        local snapshot = msgpack.decode(tostring(explain.rows[1][1]))
        local matching = nil
        for _, route in ipairs(snapshot.planner.component_routes) do
            if route.role == 'dml_view_materialization_root' then
                matching = route
            end
        end
        assert(snapshot.planner.component_status == 'complete')
        assert(matching ~= nil, 'missing embedded view materialization route')
        assert(matching.parent_id == 0)
        assert(matching.route == 'fallback')
        assert(matching.fallback_reason == 'UNSUPPORTED_SUBQUERY')

        local updated, update_err = box.execute(
            [[UPDATE planner_update_view SET value = 25 WHERE id = 2]])
        assert(update_err == nil and updated ~= nil,
               update_err and update_err.message or 'UPDATE returned no result')
        local rows, rows_err = box.execute(
            [[SELECT id, value FROM planner_update_view_source ORDER BY id]])
        assert(rows_err == nil and rows ~= nil,
               rows_err and rows_err.message or 'SELECT returned no result')
        assert(#rows.rows == 2)
        assert(rows.rows[1][2] == 10 and rows.rows[2][2] == 25)
        box.execute([[DROP VIEW planner_update_view]])
        box.execute([[DROP TABLE planner_update_view_source]])
        return matching.role
    end)
    t.assert_equals(result, 'dml_view_materialization_root')
end

g.test_trigger_select_routes_join_statement_ledger = function()
    local result = g.server:exec(function()
        local msgpack = require('msgpack')
        box.execute([[SET SESSION "sql_seq_scan" = true]])
        box.execute([[CREATE TABLE planner_trigger_target (
            id INTEGER PRIMARY KEY, value INTEGER)]])
        box.execute([[CREATE TABLE planner_trigger_source (
            id INTEGER PRIMARY KEY, value INTEGER)]])
        box.execute([[INSERT INTO planner_trigger_source VALUES (1, 10),
            (2, 20)]])
        box.execute([[CREATE TRIGGER planner_trigger_select
            AFTER INSERT ON planner_trigger_target FOR EACH ROW BEGIN
                SELECT value FROM planner_trigger_source WHERE id = 1;
            END]])

        local function explain(sql)
            local result, err = box.execute(sql)
            assert(err == nil, err and err.message)
            return msgpack.decode(tostring(result.rows[1][1]))
        end
        local standalone = explain([[EXPLAIN (planner = 'snapshot')
            INSERT INTO planner_trigger_target VALUES (1, 1)]])
        assert(standalone.planner.component_status == 'complete',
               tostring(standalone.planner.component_status) .. ':' ..
               tostring(#standalone.planner.component_routes) .. ':' ..
               tostring(standalone.planner.component_routes[1] and
                        standalone.planner.component_routes[1].route))
        assert(#standalone.planner.component_routes == 1)
        local trigger = standalone.planner.component_routes[1]
        assert(trigger.id == 1 and trigger.parent_id == 0)
        assert(trigger.role == 'trigger_select_root')
        assert(trigger.route == 'current_where_c')
        local inserted, insert_err = box.execute(
            [[INSERT INTO planner_trigger_target VALUES (1, 1)]])
        assert(insert_err == nil and inserted ~= nil,
               insert_err and insert_err.message or 'INSERT failed')

        local nested = explain([[EXPLAIN (planner = 'snapshot')
            INSERT INTO planner_trigger_target
            SELECT id, value FROM planner_trigger_source WHERE id = 2]])
        assert(nested.planner.component_status == 'complete')
        assert(#nested.planner.component_routes == 2)
        local insert_select, trigger_select =
            nested.planner.component_routes[1],
            nested.planner.component_routes[2]
        assert(insert_select.id == 1 and insert_select.parent_id == 0)
        assert(insert_select.role == 'insert_select_root')
        assert(trigger_select.id == 2 and trigger_select.parent_id == 1)
        assert(trigger_select.role == 'trigger_select')
        assert(trigger_select.route == 'current_where_c')
        local selected, select_err = box.execute([[INSERT INTO
            planner_trigger_target
            SELECT id, value FROM planner_trigger_source WHERE id = 2]])
        assert(select_err == nil and selected ~= nil,
               select_err and select_err.message or 'INSERT-SELECT failed')

        local rows, err = box.execute([[SELECT id FROM
            planner_trigger_target ORDER BY id]])
        assert(err == nil and #rows.rows == 2)
        box.execute([[DROP TABLE planner_trigger_target]])
        box.execute([[DROP TABLE planner_trigger_source]])
        return {
            standalone = trigger.role,
            nested = {insert_select.role, trigger_select.role},
            status = nested.planner.component_status,
        }
    end)
    t.assert_equals(result.status, 'complete')
    t.assert_equals(result.standalone, 'trigger_select_root')
    t.assert_equals(result.nested,
                     {'insert_select_root', 'trigger_select'})
end

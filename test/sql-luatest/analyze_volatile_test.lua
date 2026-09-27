local server = require('luatest.server')
local t = require('luatest')

local g = t.group('sql_volatile_analyze')

g.before_all(function()
    g.server = server:new({alias = 'sql_volatile_analyze'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_bare_and_named_analyze_publish_atomically = function()
    local result = g.server:exec(function()
        local adapter = package.loaded.sql_stats_snapshot_test
        if adapter == nil then
            return {test_wrapper_unavailable = true}
        end
        adapter.clear()

        local function create(name, engine)
            local space = box.schema.space.create(name, {engine = engine})
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
            return space
        end
        local function same_snapshot(left, right)
            if left.relation_count ~= right.relation_count then
                return false
            end
            for i, relation in ipairs(left.relations) do
                local other = right.relations[i]
                if relation.space_id ~= other.space_id or
                   relation.row_count ~= other.row_count then
                    return false
                end
            end
            return true
        end

        local memtx = create('analyze_volatile_memtx', 'memtx')
        local vinyl = create('analyze_volatile_vinyl', 'vinyl')
        local analyze = box.execute([[ANALYZE]])
        local bare = adapter.state()
        memtx:insert({9, 1})
        box.execute('ANALYZE ' .. memtx.name)
        local named = adapter.state()

        local before_system = adapter.state()
        box.execute([[ANALYZE _space]])
        local after_system = adapter.state()
        local missing_ok, missing_result, missing_error = pcall(box.execute,
            [[ANALYZE analyze_volatile_missing]])
        box.execute([[CREATE VIEW analyze_volatile_view AS
                      SELECT id FROM analyze_volatile_memtx]])
        local view_ok, _, view_error = pcall(box.execute,
            [[ANALYZE analyze_volatile_view]])

        local unsupported = box.schema.space.create(
            'analyze_volatile_unsupported', {engine = 'memtx'})
        unsupported:format({
            {name = 'id', type = 'unsigned'},
            {name = 'coordinates', type = 'array'},
        })
        unsupported:create_index('primary', {
            parts = {{field = 'id', type = 'unsigned'}},
        })
        unsupported:create_index('spatial', {
            type = 'rtree', unique = false,
            parts = {{field = 'coordinates', type = 'array'}},
        })
        unsupported:insert({1, {1, 2}})
        local before_failure = adapter.state()
        local unsupported_ok, _, unsupported_error = pcall(box.execute,
            [[ANALYZE analyze_volatile_unsupported]])
        local after_failure = adapter.state()

        local result = {
            bare_success = analyze ~= nil,
            bare_count = bare and bare.relation_count,
            bare_rows = bare and bare.relations,
            named_count = named and named.relation_count,
            named_rows = named and named.relations,
            system_noop = same_snapshot(before_system, after_system),
            missing_rejected = missing_ok and missing_result == nil and
                missing_error ~= nil,
            view_rejected = view_ok and view_error ~= nil,
            unsupported_rejected = unsupported_ok and
                unsupported_error ~= nil,
            failure_preserved = same_snapshot(before_failure, after_failure),
            memtx_id = memtx.id,
            vinyl_id = vinyl.id,
        }
        unsupported:drop()
        box.space.analyze_volatile_view:drop()
        memtx:drop()
        vinyl:drop()
        adapter.clear()
        return result
    end)

    if result.test_wrapper_unavailable then
        t.skip('volatile ANALYZE runtime test requires a TEST_BUILD server')
    end
    t.assert_equals(result.bare_success, true)
    t.assert_equals(result.bare_count, 2)
    t.assert_equals(result.named_count, 2)
    t.assert_equals(result.system_noop, true)
    t.assert_equals(result.missing_rejected, true)
    t.assert_equals(result.view_rejected, true)
    t.assert_equals(result.unsupported_rejected, true)
    t.assert_equals(result.failure_preserved, true)

    local row_counts = {}
    for _, relation in ipairs(result.named_rows) do
        row_counts[relation.space_id] = relation.row_count
    end
    t.assert_equals(row_counts[result.memtx_id], 9)
    t.assert_equals(row_counts[result.vinyl_id], 8)
end

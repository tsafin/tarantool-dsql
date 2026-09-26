local tap = require('tap')
local test = tap.test('SQL planner fallback capture')
test:plan(1)

local format = {{name = 'id', type = 'unsigned'}}
local a = box.schema.space.create('sql_fallback_a', {
    temporary = true, format = format,
})
a:create_index('primary')
a:insert({1})
local b = box.schema.space.create('sql_fallback_b', {
    temporary = true, format = format,
})
b:create_index('primary')
b:insert({1})

local result, err = box.execute([[SELECT a.id FROM sql_fallback_a AS a
    JOIN sql_fallback_b AS b ON a.id = b.id]])
test:ok(result ~= nil, tostring(err))

b:drop()
a:drop()
os.exit(test:check() and 0 or 1)

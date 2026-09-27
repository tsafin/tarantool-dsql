local tap = require('tap')
local test = tap.test('SQL planner fallback capture')
test:plan(11)

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
result, err = box.execute([[SELECT count(*) FROM sql_fallback_a WHERE id > 0]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[SELECT id FROM sql_fallback_a
    UNION SELECT id FROM sql_fallback_b]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[SELECT DISTINCT id % 2 FROM sql_fallback_a]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[SELECT id FROM
    (SELECT id FROM sql_fallback_a LIMIT 1)]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[WITH cte AS (SELECT id FROM sql_fallback_a)
    SELECT id FROM cte]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[SELECT id, count(*) FROM sql_fallback_a
    GROUP BY id HAVING count(*) > 0]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[SELECT id FROM sql_fallback_a WHERE id = 1]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[VALUES (1), (2)]])
test:ok(result ~= nil, tostring(err))
result, err = box.execute([[SELECT count(*) FROM sql_fallback_a]])
test:ok(result ~= nil, tostring(err))
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
result, err = box.execute([[SELECT id FROM sql_fallback_a WHERE id = 1]])
test:ok(result ~= nil, tostring(err))
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])

b:drop()
a:drop()
os.exit(test:check() and 0 or 1)

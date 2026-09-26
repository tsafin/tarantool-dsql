local tap = require('tap')
local test = tap.test('typed SQL capture')
test:plan(3)
local result, err = box.execute([[SELECT CAST('1.20' AS DECIMAL),
    CAST('2020-01-01T00:00:00Z' AS DATETIME), [11, 22], {1: 'two'}]])
test:ok(result ~= nil, tostring(err))
local call_ok, bad_result, bad_err = pcall(box.execute,
    'SELECT * FROM sql_capture_missing_relation')
test:ok((not call_ok) or (bad_result == nil and bad_err ~= nil),
    'expected SELECT compilation error is captured')
local space = box.schema.space.create('typed_sql_no_planner', {
    temporary = true,
    format = {{name = 'id', type = 'unsigned'}},
})
space:create_index('primary')
result, err = box.execute('INSERT INTO typed_sql_no_planner VALUES (1)')
test:ok(result ~= nil, tostring(err))
space:drop()
os.exit(test:check() and 0 or 1)

local tap = require('tap')
local test = tap.test('typed SQL capture')
test:plan(1)
local result, err = box.execute([[SELECT CAST('1.20' AS DECIMAL),
    CAST('2020-01-01T00:00:00Z' AS DATETIME), [11, 22], {1: 'two'}]])
test:ok(result ~= nil, tostring(err))
os.exit(test:check() and 0 or 1)

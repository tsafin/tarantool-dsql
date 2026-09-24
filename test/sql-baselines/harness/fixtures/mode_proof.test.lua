local tap = require('tap')
local test = tap.test('per-query mode proof')
test:plan(5)
test:is(box.execute('CREATE TABLE mode_proof_table (id INTEGER PRIMARY KEY)') ~= nil,
        true, 'create table')
test:is(box.execute('INSERT INTO mode_proof_table VALUES (1)') ~= nil,
        true, 'insert row')
test:is(box.execute('SELECT id FROM mode_proof_table').rows[1][1], 1,
        'executed query')
test:is(box.execute('EXPLAIN SELECT 1').rows ~= nil, true, 'compile only')
local _, err = box.execute('SELECT * FROM no_such_mode_proof_table')
test:is(err ~= nil, true, 'compile error')
os.exit(test:check() and 0 or 1)

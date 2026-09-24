local source = debug.getinfo(1, 'S').source
local dir = source:sub(2):match('^(.+)/[^/]+$')
local repo = dir:match('^(.+)/test/sql%-baselines/harness$')
local adapter = dofile(dir .. '/sql_file.lua')

local function capture(path, expected)
    local seen = {}
    local count = adapter.run(repo .. '/test/sql/' .. path, function(sql)
        seen[#seen + 1] = sql
        return {rows = {}}
    end)
    assert(count == expected and #seen == expected)
    return seen
end

local first = capture('gh-4256-do-not-change-order-during-insertion.test.sql', 5)
assert(first[1] == 'CREATE TABLE t (i INT PRIMARY KEY AUTOINCREMENT);')
local second = capture('gh-4697-scalar-bool-sort-cmp.test.sql', 7)
assert(second[7] == 'SELECT s3, TYPEOF(s3) FROM SEQSCAN test ORDER BY s3;')

local ok, err = pcall(adapter.run, repo .. '/test/sql/boolean.test.sql',
                      function() return {rows = {}} end)
assert(not ok and err:match('unsupported SQL console directive'))

ok, err = pcall(adapter.run, repo .. '/test/sql/' ..
                'gh-4256-do-not-change-order-during-insertion.test.sql',
                function() return nil, 'injected failure' end)
assert(not ok and err:match('unexpected SQL error'))

print('sql_file_test: ok')

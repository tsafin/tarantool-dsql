#!/usr/bin/env tarantool
-- Join-legality fixtures for a left-deep exact-search comparator.
-- Check SQL results as well as loop order: an EQP-only test could miss a
-- null-extension regression, while result-only checks miss illegal search.
local test = require('sqltester')
test:plan(11)

test:do_execsql_test('dp-join-setup', [[
    CREATE TABLE dpa(id INTEGER PRIMARY KEY, k INTEGER);
    CREATE TABLE dpb(id INTEGER PRIMARY KEY, k INTEGER);
    CREATE TABLE dpc(id INTEGER PRIMARY KEY, k INTEGER);
    CREATE TABLE dpx(id INTEGER PRIMARY KEY, k INTEGER);
    CREATE TABLE dpy(id INTEGER PRIMARY KEY, k INTEGER);
    INSERT INTO dpa VALUES(1, 10), (2, 20), (3, 30);
    INSERT INTO dpb VALUES(1, 10), (2, 20);
    INSERT INTO dpc VALUES(1, 10), (3, 30);
    INSERT INTO dpx VALUES(1, 10), (2, 20), (3, 30);
    INSERT INTO dpy VALUES(1, 20), (2, 30), (3, 10);
    CREATE INDEX dpak ON dpa(k);
    CREATE INDEX dpbk ON dpb(k);
    CREATE INDEX dpck ON dpc(k);
    CREATE INDEX dpxk ON dpx(k);
    CREATE INDEX dpyk ON dpy(k);
]], {})

local function loop_order(sql)
    local eqp = test:execsql('EXPLAIN QUERY PLAN ' .. sql)
    local order = {}
    for i = 4, #eqp, 4 do
        local name = eqp[i]:match('TABLE%s+(dp[abc])')
        if name ~= nil then table.insert(order, name) end
    end
    return order
end

-- INNER JOIN permits alternative orders; its semantics must not depend on
-- the chosen one. Keep a cyclic and a disconnected join graph in the corpus.
test:do_execsql_test('dp-inner-cycle', [[
    SELECT dpa.id FROM dpa JOIN dpb ON dpa.k = dpb.k
        JOIN dpc ON dpb.k = dpc.k AND dpc.id = dpa.id
    ORDER BY dpa.id;
]], {1})
test:do_execsql_test('dp-inner-disconnected', [[
    SELECT dpa.id, dpc.id FROM dpa JOIN dpb ON dpa.k = dpb.k,
        dpc WHERE dpc.id = 3 ORDER BY dpa.id;
]], {1, 3, 2, 3})

-- CROSS is an order barrier in this planner, even though it has inner-join
-- result semantics. The right table, and a following JOIN, cannot move left.
test:do_test('dp-cross-order', function()
    return loop_order([[SELECT dpa.id FROM dpa CROSS JOIN dpb
        JOIN dpc ON dpc.id = dpb.id WHERE dpa.id = 1]])
end, {'dpa', 'dpb', 'dpc'})
test:do_execsql_test('dp-cross-result', [[
    SELECT dpa.id, dpb.id FROM dpa CROSS JOIN dpb
        JOIN dpc ON dpc.id = dpb.id WHERE dpa.id = 1;
]], {1, 1})

-- LEFT JOIN must preserve its left input before opening the nullable side.
-- The third relation tests the inherited barrier at the next loop depth.
test:do_test('dp-left-order', function()
    return loop_order([[SELECT dpa.id FROM dpa LEFT JOIN dpb
        ON dpa.k = dpb.k JOIN dpc ON dpc.id = dpa.id]])
end, {'dpa', 'dpb', 'dpc'})
test:do_execsql_test('dp-left-null-extension', [[
    SELECT dpa.id, dpb.id FROM dpa LEFT JOIN dpb ON dpa.k = dpb.k
    ORDER BY dpa.id;
]], {1, 1, 2, 2, 3, ''})

-- USING is predicate/column projection rewriting, not a new physical join.
test:do_execsql_test('dp-using', [[
    SELECT k FROM dpa JOIN dpb USING(k) ORDER BY k;
]], {10, 20})
test:do_execsql_test('dp-natural', [[
    SELECT k FROM dpa NATURAL JOIN dpb ORDER BY k;
]], {10, 20})

-- dpx and dpy have identical single-column id/k marginals, but different
-- cross-table id/k correlations. A marginal-only estimator cannot know that
-- one two-predicate join is full and the other empty.
test:do_execsql_test('dp-correlated-actual', [[
    SELECT (SELECT COUNT(*) FROM dpa JOIN dpx
            ON dpa.id = dpx.id AND dpa.k = dpx.k),
           (SELECT COUNT(*) FROM dpa JOIN dpy
            ON dpa.id = dpy.id AND dpa.k = dpy.k);
]], {3, 0})
test:do_test('dp-correlated-eqp-estimates', function()
    local function estimates(rhs)
        local eqp = test:execsql('EXPLAIN QUERY PLAN SELECT dpa.id FROM ' ..
            'dpa JOIN ' .. rhs .. ' ON dpa.id = ' .. rhs ..
            '.id AND dpa.k = ' .. rhs .. '.k')
        local result = {}
        for i = 4, #eqp, 4 do
            local estimate = eqp[i]:match('%(%~%d+ rows?%)')
            if estimate ~= nil then table.insert(result, estimate) end
        end
        return result
    end
    local x, y = estimates('dpx'), estimates('dpy')
    return {#x > 0, table.concat(x, ','), table.concat(y, ',')}
end, {true, '(~1048576 rows),(~1 row)', '(~1048576 rows),(~1 row)'})

test:finish_test()

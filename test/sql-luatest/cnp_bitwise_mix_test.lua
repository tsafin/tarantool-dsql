local server = require('luatest.server')
local t = require('luatest')

local g = t.group('sql_cnp_bitwise_mix')

local BITWISE_SQL = [[
    SELECT (((a & b) | c) & ((a << 1) | (b >> 1))),
           ((a | b) & (c << 2)),
           ((a >> 1) | (b << 1)),
           ((~a) & 1023),
           ((a & (~b)) | (c >> 1)),
           ((a << 2) & (b | 255)),
           ((a & 1023) | ((~b) & 255)),
           (((a << 1) & 1023) | ((b >> 1) & 255)),
           (((c << 2) | (a & 255)) & ((~b) & 1023))
    FROM bench_arith
    WHERE id = ?;
]]

g.before_all(function()
    g.server = server:new({
        alias = 'sql_cnp_bitwise_mix',
        env = {VDBE_DISPATCHER = 'cnp'},
    })
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_bitwise_mix_extended_shape = function()
    local rows = g.server:exec(function(sql)
        local yaml = require('yaml')
        local t = require('luatest')
        pcall(box.execute, 'DROP TABLE bench_arith')
        box.execute([[
            CREATE TABLE bench_arith(
                id INTEGER PRIMARY KEY,
                a INTEGER,
                b INTEGER,
                c INTEGER
            );
        ]])
        box.execute([[INSERT INTO bench_arith VALUES (1, 10, 6, 3);]])
        local explain = box.execute('EXPLAIN ' .. sql, {1}).rows
        local counts = {}
        for _, row in ipairs(explain) do
            counts[row[2]] = (counts[row[2]] or 0) + 1
        end
        t.assert_equals(counts.Column, 3)
        t.assert_equals(counts.BitAnd, 13)
        t.assert_equals(counts.BitOr, 9)
        t.assert_equals(counts.BitNot, 4)
        t.assert_equals(counts.ShiftLeft, 6)
        t.assert_equals(counts.ShiftRight, 4)
        box.execute([[DROP TABLE bench_arith;]])
        return yaml.encode(counts)
    end, {BITWISE_SQL})
    t.assert_type(rows, 'string')
end

g.test_bitwise_mix_extended_result_and_cnp_exec = function()
    local res = g.server:exec(function(sql)
        local bit = require('bit')
        local t = require('luatest')
        pcall(box.execute, 'DROP TABLE bench_arith')
        box.execute([[
            CREATE TABLE bench_arith(
                id INTEGER PRIMARY KEY,
                a INTEGER,
                b INTEGER,
                c INTEGER
            );
        ]])
        for i = 1, 4 do
            box.execute([[INSERT INTO bench_arith VALUES (?, ?, ?, ?)]],
                        {i, i * 10, i * 5 + 1, i * 2 + 1})
        end
        local id = 3
        local a = id * 10
        local b = id * 5 + 1
        local c = id * 2 + 1
        local expected = {
            bit.band(bit.bor(bit.band(a, b), c),
                     bit.bor(bit.lshift(a, 1), bit.rshift(b, 1))),
            bit.band(bit.bor(a, b), bit.lshift(c, 2)),
            bit.bor(bit.rshift(a, 1), bit.lshift(b, 1)),
            bit.band(bit.bnot(a), 1023),
            bit.bor(bit.band(a, bit.bnot(b)), bit.rshift(c, 1)),
            bit.band(bit.lshift(a, 2), bit.bor(b, 255)),
            bit.bor(bit.band(a, 1023), bit.band(bit.bnot(b), 255)),
            bit.bor(bit.band(bit.lshift(a, 1), 1023),
                    bit.band(bit.rshift(b, 1), 255)),
            bit.band(bit.bor(bit.lshift(c, 2), bit.band(a, 255)),
                     bit.band(bit.bnot(b), 1023)),
        }
        local before = box.stat.sql()
        local stmt = box.prepare(sql)
        local result = box.execute(stmt.stmt_id, {id})
        local after = box.stat.sql()
        box.unprepare(stmt.stmt_id)
        box.execute([[DROP TABLE bench_arith;]])
        t.assert_equals(result.rows[1], expected)
        t.assert_gt(after.sql_cnp_exec_count, before.sql_cnp_exec_count)
        t.assert_equals(after.sql_cnp_fallback_count,
                        before.sql_cnp_fallback_count)
        return {
            rows = result.rows[1],
            before_exec = before.sql_cnp_exec_count,
            after_exec = after.sql_cnp_exec_count,
        }
    end, {BITWISE_SQL})
    t.assert_equals(#res.rows, 9)
    t.assert_gt(res.after_exec, res.before_exec)
end

g.test_bitwise_mix_negative_integer_keeps_error_semantics = function()
    local err = g.server:exec(function(sql)
        pcall(box.execute, 'DROP TABLE bench_arith')
        box.execute([[
            CREATE TABLE bench_arith(
                id INTEGER PRIMARY KEY,
                a INTEGER,
                b INTEGER,
                c INTEGER
            );
        ]])
        box.execute([[INSERT INTO bench_arith VALUES (1, -10, 6, 3);]])
        local _, err = box.execute(sql, {1})
        box.execute([[DROP TABLE bench_arith;]])
        return err.message
    end, {BITWISE_SQL})
    t.assert_str_contains(err, 'to unsigned')
end

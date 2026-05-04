local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({
        alias = 'explain-modifiers',
        env = { VDBE_DISPATCHER = 'cnp' },
    })
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_explain_bytecode_and_disassembly = function()
    g.server:exec(function()
        local stat = box.stat.sql()
        if stat.sql_cnp_compile_count == nil then
            t.skip('CnP is not available in this build')
        end

        local bytecode = box.execute([[EXPLAIN (bytecode=yes) SELECT 1 + 2 + 3;]])
        t.assert_equals(bytecode.metadata, {
            {name = 'addr', type = 'integer'},
            {name = 'opcode', type = 'text'},
            {name = 'p1', type = 'integer'},
            {name = 'p2', type = 'integer'},
            {name = 'p3', type = 'integer'},
            {name = 'p4', type = 'text'},
            {name = 'p5', type = 'text'},
            {name = 'comment', type = 'text'},
        })
        t.assert_gt(#bytecode.rows, 0)
        t.assert_equals(bytecode.rows[1][1], 0)
        t.assert_equals(type(bytecode.rows[1][2]), 'string')
        local opcodes = {}
        local literal_count = 0
        for _, row in ipairs(bytecode.rows) do
            opcodes[row[2]] = (opcodes[row[2]] or 0) + 1
            if row[2] == 'Integer' or row[2] == 'Int64' then
                literal_count = literal_count + 1
            end
        end
        t.assert_equals(opcodes.Add, nil)
        t.assert_equals(opcodes.Subtract, nil)
        t.assert_equals(opcodes.Multiply, nil)
        t.assert_equals(opcodes.Divide, nil)
        t.assert_equals(opcodes.Remainder, nil)
        t.assert_ge(literal_count, 1)

        local disassemble = box.execute(
            [[EXPLAIN (disassemble=yes) SELECT 1 + 2 + 3;]]
        )
        t.assert_equals(disassemble.metadata, {
            {name = 'section', type = 'text'},
            {name = 'addr', type = 'integer'},
            {name = 'detail', type = 'text'},
        })
        t.assert_equals(disassemble.rows[1][1], 'disassembly')

        local disassembly = {}
        for _, row in ipairs(disassemble.rows) do
            if row[1] == 'disassembly' then
                table.insert(disassembly, row[3])
            end
        end
        t.assert_gt(#disassembly, 3)

        local dump = table.concat(disassembly, '\n')
        t.assert(
            dump:find('push   rbp', 1, true) ~= nil or
            dump:find('push   rax', 1, true) ~= nil
        )
        t.assert_str_contains(dump, 'ret')
        t.assert_not_str_contains(dump, 'file format elf64-x86-64')
        t.assert_not_str_contains(dump, 'Disassembly of section .text:')
        t.assert_str_contains(dump, 'vdbe_cnp_stmt_')

        local combined = box.execute(
            [[EXPLAIN (bytecode=yes, disassemble=yes) SELECT 1 + 2 + 3;]]
        )
        t.assert_equals(combined.metadata, {
            {name = 'section', type = 'text'},
            {name = 'addr', type = 'integer'},
            {name = 'detail', type = 'text'},
        })
        t.assert_equals(combined.rows[1][1], 'bytecode')
        t.assert_equals(combined.rows[#combined.rows][1], 'disassembly')

        local ok, err = pcall(function()
            box.execute([[EXPLAIN (unknown=yes) SELECT 1;]])
        end)
        t.assert_equals(ok, true)
        t.assert_equals(err, nil)
    end)
end

local t = require('luatest')
local treegen = require('test.treegen')
local justrun = require('test.justrun')

local g = t.group()

g.test_large_space_format_does_not_corrupt_gc_region = function()
    treegen.init(g)
    local dir = treegen.prepare_directory(g, {}, {})
    treegen.write_script(dir, 'main.lua', [[
        box.cfg{}

        local format = {}
        for i = 1, 2001 do
            format[i] = {name = 'A' .. i, type = 'unsigned'}
        end

        local s = box.schema.space.create('S0', {format = format})
        s:create_index('pk')
        assert(s.name == 'S0')
        assert(#s:format() == 2001)
        s:drop()
        os.exit(0)
    ]])

    local opts = {nojson = true, stderr = true}
    local res = justrun.tarantool(dir, {}, {'main.lua'}, opts)
    t.assert_equals(res.exit_code, 0)
    t.assert_not_str_contains(res.stderr, 'Assertion')
    t.assert_not_str_contains(res.stderr, 'Aborted')
end

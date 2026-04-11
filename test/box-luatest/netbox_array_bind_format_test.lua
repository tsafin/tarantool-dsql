local t = require('luatest')
local treegen = require('test.treegen')
local justrun = require('test.justrun')

local g = t.group()

g.test_netbox_array_bind_does_not_corrupt_tuple_format = function()
    treegen.init(g)
    local dir = treegen.prepare_directory(g, {}, {})
    treegen.write_script(dir, 'main.lua', [=[
        local socket_path = 'unix/:./iproto.sock'
        box.cfg{listen = socket_path}
        box.schema.user.grant('guest', 'execute', 'sql')

        local cn = require('net.box').connect(socket_path)
        local res = cn:execute([[SELECT #a;]], {{['#a'] = {1, 2, 3}}})
        assert(res.rows[1][1][1] == 1)
        cn:close()

        box.schema.user.revoke('guest', 'execute', 'sql')
        os.exit(0)
    ]=])

    local opts = {nojson = true, stderr = true}
    local res = justrun.tarantool(dir, {}, {'main.lua'}, opts)
    t.assert_equals(res.exit_code, 0)
    t.assert_not_str_contains(res.stderr, 'Assertion')
    t.assert_not_str_contains(res.stderr, 'Aborted')
end

local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'gh-6572'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_nan_null_lua = function()
    g.server:exec(function()
        local sql = [[SELECT LUA('return 0/0'), TYPEOF(LUA('return 0/0'));]]
        t.assert_equals(box.execute(sql).rows, {{nil, 'NULL'}})
    end)
end

g.test_nan_null_c = function()
    g.server:exec(function()
        local build_path = os.getenv("BUILDDIR")
        local fio = require('fio')
        local source_dir = fio.dirname(debug.getinfo(1, 'S').source:sub(2))
        local module_cpath = source_dir..'/?.so;'..source_dir..'/?.dylib;'
        if build_path ~= nil then
            module_cpath = module_cpath..
                           build_path..'/test/sql-luatest/?.so;'..
                           build_path..'/test/sql-luatest/?.dylib;'
        end
        package.cpath = module_cpath..package.cpath
        local func = {language = 'C', returns = 'double', exports = {'SQL'}}
        box.schema.func.create('gh_6572.get_nan', func);

        local sql = [[SELECT "gh_6572.get_nan"(), TYPEOF("gh_6572.get_nan"());]]
        t.assert_equals(box.execute(sql).rows, {{nil, 'NULL'}})
    end)
end

g.test_nan_comp_dec = function()
    g.server:exec(function()
        local sql = [[SELECT 1.0 > LUA('return 0/0');]]
        t.assert_equals(box.execute(sql).rows, {{}})
    end)
end

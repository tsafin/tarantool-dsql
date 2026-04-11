#!/usr/bin/env tarantool
local build_path = os.getenv("BUILDDIR")
local fio = require('fio')
local source_dir = fio.dirname(debug.getinfo(1, 'S').source:sub(2))
local module_cpath = source_dir..'/?.so;'..source_dir..'/?.dylib;'
if build_path ~= nil then
    module_cpath = module_cpath..
                   build_path..'/test/sql-tap/?.so;'..
                   build_path..'/test/sql-tap/?.dylib;'
end
package.cpath = module_cpath..package.cpath

local test = require("sqltester")
test:plan(2)

box.schema.func.create("gh-5938-wrong-string-length.ret_str", {
    language = "C",
    param_list = { "string" },
    returns = "string",
    exports = { "LUA", "SQL" },
    is_deterministic = true
})

test:execsql([[CREATE TABLE t (i INT PRIMARY KEY, s STRING);]])
box.space.t:insert({1, 'This is a complete string'})
box.space.t:insert({2, 'This is a cropped\0 string'})

test:do_execsql_test(
    "gh-5938-1",
    [[
        SELECT "gh-5938-wrong-string-length.ret_str"(s) from t;
    ]], {
        "This is a complete string","This is a cropped\0 string"
    })

box.schema.func.create("ret_str", {
    language = "Lua",
    body = [[function(str) return str end]],
    param_list = { "string" },
    returns = "string",
    exports = { "LUA", "SQL" },
    is_deterministic = true
})

test:do_execsql_test(
    "gh-5938-2",
    [[
        SELECT "ret_str"(s) from t;
    ]], {
        "This is a complete string","This is a cropped\0 string"
    })

test:finish_test()

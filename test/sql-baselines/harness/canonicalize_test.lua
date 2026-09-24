#!/usr/bin/env tarantool
-- Standalone regression for SQL result-column capture.
local dir = debug.getinfo(1, 'S').source:sub(2):match('^(.+)/[^/]+$')
local canonicalize = dofile(dir .. '/canonicalize.lua')

local result = canonicalize.canon_L1({{42, 'answer'}}, false, {
    {name = 'N', type = 'integer'},
    {name = 'LABEL', type = 'string'},
})
assert(result.column_names[1] == 'N' and result.column_names[2] == 'LABEL')
assert(result.column_types[1] == 'integer' and result.column_types[2] == 'string')
assert(result.rows[1][1] == 42 and result.rows[1][2] == 'answer')

local dml = canonicalize.canon_L1(nil, false, nil)
assert(dml.column_names == nil and dml.column_types == nil)

local ok = pcall(canonicalize.canon_L1, {}, false, {{name = 'N'}})
assert(not ok, 'malformed result metadata must fail capture')

assert(canonicalize.has_order_by('SELECT * FROM t ORDER /* hi */ BY a'))
assert(canonicalize.has_order_by('SELECT * FROM t order\nby a'))
assert(not canonicalize.has_order_by("SELECT 'ORDER BY' FROM t"))
assert(not canonicalize.has_order_by('SELECT "ORDER BY" FROM t'))
assert(not canonicalize.has_order_by('SELECT * FROM t -- ORDER BY a'))
assert(not canonicalize.has_order_by('SELECT * FROM (SELECT * FROM t ORDER BY a) AS x'))
assert(not canonicalize.has_order_by('SELECT * FROM t /* ORDER BY a */'))
assert(not canonicalize.has_order_by('SELECT * FROM t ORDER + BY a'))

local nested = setmetatable({[1] = {11, 22}, ['1'] = 'text'},
                        {__serialize = 'map'})
local containers = canonicalize.canon_L1({{nested}}, false, nil)
assert(type(containers.rows[1][1]) == 'table')
assert(containers.rows[1][1][1][2] == 22)
assert(containers.rows[1][1]['1'] == 'text')
assert(getmetatable(containers.rows[1][1]).__serialize == 'map')
local empty_map = setmetatable({}, {__serialize = 'map'})
local empty_seq = setmetatable({}, {__serialize = 'seq'})
local encoded = dofile(dir .. '/../lib/canonical_yaml.lua').emit_nodoc(
    canonicalize.canon_L1({{empty_map, empty_seq}}, false, nil).rows)
assert(encoded:find('{}', 1, true) and encoded:find('[]', 1, true))
os.exit(0)

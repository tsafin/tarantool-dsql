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

local aliases = canonicalize.canon_L1({}, false, {
    {name = 'sql_sq_7ED763C32038.COLUMN_1', type = 'integer'},
    {name = 'sql_sq_ABCDEF', type = 'integer'},
    {name = 'sql_sq_abcd.COLUMN_1', type = 'integer'},
    {name = 'user_sql_sq_ABC.COLUMN_1', type = 'integer'},
})
assert(aliases.column_names[1] == 'sql_sq_<generated>.COLUMN_1')
assert(aliases.column_names[2] == 'sql_sq_<generated>')
assert(aliases.column_names[3] == 'sql_sq_abcd.COLUMN_1')
assert(aliases.column_names[4] == 'user_sql_sq_ABC.COLUMN_1')

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
assert(containers.rows[1][1].sql_type == 'map')
local entry_values = {}
for _, entry in ipairs(containers.rows[1][1].entries) do
    entry_values[type(entry.key) .. tostring(entry.key)] = entry.value
end
assert(entry_values.number1.sql_type == 'array')
assert(entry_values.number1.items[2] == 22)
assert(entry_values.string1 == 'text')
local empty_map = setmetatable({}, {__serialize = 'map'})
local empty_seq = setmetatable({}, {__serialize = 'seq'})
local empties = canonicalize.canon_L1({{empty_map, empty_seq}}, false, nil).rows[1]
assert(empties[1].sql_type == 'map' and #empties[1].entries == 0)
assert(empties[2].sql_type == 'array' and #empties[2].items == 0)

local decimal = require('decimal')
local datetime = require('datetime')
local ffi = require('ffi')
local extended = canonicalize.canon_L1({{decimal.new('1.20'),
    datetime.new{year = 2020}, ffi.new('int64_t', 42),
    ffi.new('uint64_t', 43)}}, false, nil).rows[1]
assert(extended[1].sql_type == 'decimal' and extended[1].value == '1.20')
assert(extended[2].sql_type == 'datetime')
assert(extended[3].sql_type == 'int64' and extended[3].value == '42')
assert(extended[4].sql_type == 'uint64' and extended[4].value == '43')
local yaml_lib = dofile(dir .. '/../lib/canonical_yaml.lua')
local decoded = require('yaml').decode(yaml_lib.emit(extended))
assert(decoded[1].sql_type == 'decimal' and decoded[1].value == '1.20')
assert(decoded[2].sql_type == 'datetime' and type(decoded[2].value) == 'string')

local explain_rows = {
    {1, 'OpenTEphemeral', 2, 0, 0, string.char(0x80, 0x31), '00'},
    {2, 'OpenTEphemeral', 3, 0, 0, string.char(0xff, 0x42), '00'},
    {3, 'OpenTEphemeral', 4, 0, 0, '', '00'},
    {4, 'String', 5, 0, 0, 'meaningful P4', '00'},
}
local explained = canonicalize.canon_L1(explain_rows, true, nil,
                                        'EXPLAIN SELECT 1').rows
assert(explained[1][6] == '<sql_space_info>')
assert(explained[2][6] == '<sql_space_info>')
assert(explained[3][6] == '')
assert(explained[4][6] == 'meaningful P4')
local ordinary = canonicalize.canon_L1(explain_rows, true, nil,
                                       'SELECT 1').rows
assert(ordinary[1][6] == string.char(0x80, 0x31))
os.exit(0)

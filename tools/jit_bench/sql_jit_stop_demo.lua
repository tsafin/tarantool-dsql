#!/usr/bin/env tarantool

local ffi = require('ffi')

ffi.cdef[[
int raise(int sig);
]]

local SIGSTOP = 19
local SQL = 'SELECT 1 + 2 + 3 + 4 + 5;'

box.cfg{log_level = 4}

local function assert_res(res)
    if res.rows[1][1] ~= 15 then
        error(('unexpected result: %s'):format(tostring(res.rows[1][1])))
    end
end

local stmt = box.prepare(SQL)

-- MCJIT compiles on prepare. CnP compiles on first execution, so warm it once.
if os.getenv('VDBE_DISPATCHER') == 'cnp' then
    assert_res(box.execute(stmt.stmt_id))
end

ffi.C.raise(SIGSTOP)

for _ = 1, 10 do
    assert_res(box.execute(stmt.stmt_id))
end

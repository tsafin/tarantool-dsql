--[[
 * M2 gate test for Copy-and-Patch VDBE dispatcher.
 *
 * Gate: VDBE_DISPATCHER=cnp box.execute("SELECT 42") returns 42
 * and the CnP execution counter is non-zero.
 *
 * Run from the build directory:
 *   cd /path/to/build && rm -f *.snap *.xlog
 *   VDBE_DISPATCHER=cnp ./src/tarantool /path/to/test_cnp_m2.lua
--]]

box.cfg{}

local res = box.execute("SELECT 42")
assert(res ~= nil, "box.execute returned nil")
assert(res.rows ~= nil, "result has no rows")
assert(#res.rows == 1, "expected 1 row, got " .. tostring(#res.rows))
local val = res.rows[1][1]
assert(val == 42, "expected 42, got " .. tostring(val))

local stats = box.stat.sql()
local cnp_count = stats["sql_cnp_exec_count"]
assert(cnp_count ~= nil, "sql_cnp_exec_count missing from box.stat.sql()")
assert(cnp_count > 0,
       "CnP exec counter should be > 0, got " .. tostring(cnp_count))

print(string.format("M2 gate: PASS  SELECT 42 = %d  sql_cnp_exec_count = %d",
                    val, cnp_count))
os.exit(0)

-- forensic.lua
-- M0.3 L6 forensic VDBE program-listing capture.
--
-- ## Capture contract
--
-- We investigated src/box/sql/vdbe.c for per-statement VDBE trace hooks:
--
-- 1. SQL_VdbeTrace / vdbe_trace() — exists but is SQL_DEBUG-only (#ifdef SQL_DEBUG)
--    and writes to stdout via printf(). It fires per-opcode inside check_vdbe_operands()
--    which is also #ifdef SQL_DEBUG. There is no Lua-callable API to subscribe to it
--    or redirect its output.
--
-- 2. db->xTrace / SQL_TRACE_STMT — db->xTrace is a C function pointer in the sql
--    struct (sqlInt.h:995). OP_Trace (vdbe.c:3816) calls db->xTrace(SQL_TRACE_STMT, ...)
--    with the SQL text of the statement, NOT per-opcode. This fires once per prepared
--    statement execution at the OP_Trace opcode, and only if db->mTrace has
--    SQL_TRACE_STMT set. The callback receives a Vdbe pointer and the SQL string;
--    it does NOT expose the opcode stream. There is no Lua binding to set db->xTrace.
--
-- 3. SQL_SqlTrace / sqlDebugPrintf — prints "SQL-trace: <sql>" to stdout when the
--    SQL_SqlTrace flag is set. Again per-statement (not per-opcode) and stdout-only.
--
-- 4. box.stat.sql() per-opcode profiling (SQL_VDBE_OP_PROFILE) — accumulates opcode
--    counts and timing across ALL statements since process start. Taking a before/after
--    delta gives approximate per-statement data, but:
--      (a) it conflates concurrent fibers that may execute SQL simultaneously;
--      (b) it gives only aggregate opcode counts (no PC sequence, no P1..P5 values);
--      (c) the schema calls for "one VDBE opcode per line" with P1..P5 — impossible
--          from aggregate counters alone.
--    This approach would produce MISLEADING data. We do NOT fall back to it.
--
-- ## What would be needed in C (for a future implementer)
--
-- To capture a real per-statement VDBE opcode trace from Lua, one of these C changes
-- would be needed (none of which we implement here — src/ is read-only for this task):
--
--   Option A: Add a Lua callback hook to sqlVdbeExec() that fires before each opcode
--     dispatch, receiving (pc, opcode, p1, p2, p3, p4_rendered, p5). The hook pointer
--     would live on the Vdbe struct or the sql struct. Lua registers the callback via
--     a new box.sql.set_vdbe_trace_hook(fn) builtin. This is the cleanest option.
--
--   Option B: Teach SQL_VdbeTrace to write to a Tarantool say_logger or an in-memory
--     ring buffer accessible from Lua, instead of printf(). Still requires a C change.
--
--   Option C: Use the existing db->xTrace + SQL_TRACE_STMT mechanism but add a new
--     SQL_TRACE_OPCODE mask and extend xTrace to fire per-opcode. Architecturally
--     consistent with the existing API but requires more C changes.
--
-- ## Current behavior of this module
--
-- The harness runs `EXPLAIN <statement>` before the statement and captures its
-- stable VDBE program listing. This records the compiled opcode sequence and
-- operands, but not the dynamic sequence taken at execution time. The file
-- header makes this limitation explicit. Aggregate profile deltas are not used.
--
-- Output path:
--   test/sql-baselines/forensics/<suite>/<test>/<seq_str>.<engine>.<dispatcher>.trace-query
--
-- Format when an EXPLAIN program is available:
--   <pc>,<opcode_name>,<P1>,<P2>,<P3>,<P4_kind>:<P4_value>,<P5>
-- `EXPLAIN_TEXT` is used for P4 because SQL EXPLAIN does not expose the original
-- internal P4 union tag. See SCHEMA.md L6 for the precise format.

local fio = require('fio')

local M = {}

-- Compute query index string per SCHEMA.md:
--   %02d for 1-99 → "q01" .. "q99"
--   unpadded for 100+ → "q100", "q101", ...
local function seq_str(n)
    if n < 100 then
        return string.format('q%02d', n)
    else
        return 'q' .. tostring(n)
    end
end

-- Detect current dispatcher from environment variable VDBE_DISPATCHER.
local function current_dispatcher()
    local d = os.getenv('VDBE_DISPATCHER') or 'generated'
    return d
end

-- snapshot_before / snapshot_after remain stubs for compatibility with run.lua;
-- L6 now comes from a separate EXPLAIN program listing, not aggregate counters.

function M.snapshot_before(dispatcher)  -- luacheck: ignore
    -- No per-statement trace API available in the current build.
    -- See module header for investigation details.
    return nil
end

function M.snapshot_after(before, dispatcher)  -- luacheck: ignore
    return nil
end

-- Write a forensic trace file for one query.
-- params:
--   baselines_root  string
--   suite           string
--   test_basename   string
--   seq             int
--   engine          string
--   dispatcher      string (optional, auto-detected from VDBE_DISPATCHER env)
--   profile_delta   table | nil  (ignored — aggregate deltas are misleading; see header)
--   sql             string (optional, the SQL text for the comment)
function M.write(params)
    local p = params
    local dispatcher = p.dispatcher or current_dispatcher()

    local out_dir = p.baselines_root .. '/forensics/' .. p.suite .. '/' .. p.test_basename
    fio.mktree(out_dir)
    local fname = seq_str(p.seq) .. '.' .. p.engine .. '.' .. dispatcher .. '.trace-query'
    local out_path = out_dir .. '/' .. fname

    local f = io.open(out_path, 'w')
    if not f then
        error('Cannot open for writing: ' .. out_path)
    end

    f:write('# L6 VDBE static program listing from SQL EXPLAIN\n')
    f:write('# This is not a dynamic execution trace; untaken branches are included.\n')
    f:write('# suite=' .. (p.suite or '') .. '\n')
    f:write('# test=' .. (p.test_basename or '') .. '\n')
    f:write('# query_index=' .. tostring(p.seq) .. '\n')
    f:write('# engine=' .. (p.engine or '') .. '\n')
    f:write('# dispatcher=' .. dispatcher .. '\n')
    if p.sql then
        f:write('# sql=' .. p.sql:gsub('\n', ' ') .. '\n')
    end
    f:write('# format: <pc>,<opcode_name>,<P1>,<P2>,<P3>,<P4_kind>:<P4_value>,<P5>\n')
    if type(p.program) == 'table' then
        for _, row in ipairs(p.program) do
            local p4_kind, p4_value = 'NONE', ''
            if row[6] ~= nil and row[6] ~= '' then
                p4_kind = 'EXPLAIN_TEXT'
                p4_value = string.format('%q', tostring(row[6]))
            end
            f:write(string.format('%s,%s,%s,%s,%s,%s:%s,%s\n',
                tostring(row[1] or ''), tostring(row[2] or ''),
                tostring(row[3] or ''), tostring(row[4] or ''),
                tostring(row[5] or ''), p4_kind, p4_value,
                tostring(row[7] or '')))
        end
    elseif p.capture_error ~= nil then
        f:write('# capture_unavailable=' .. tostring(p.capture_error):gsub('[\r\n]', ' ') .. '\n')
    else
        f:write('# capture_unavailable=no EXPLAIN program returned\n')
    end
    f:close()

    return out_path
end

return M

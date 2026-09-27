-- canonicalize.lua
-- Applies the 8 canonicalization rules from SCHEMA.md to L1 rows and L2 diagnostics.

local M = {}
local source = debug.getinfo(1, 'S').source
local dir = source:sub(1, 1) == '@' and source:sub(2):match('^(.+)/[^/]+$') or '.'
local yaml_emitter = dofile(dir .. '/../lib/canonical_yaml.lua')
local ffi = require('ffi')
local decimal = require('decimal')
local datetime = require('datetime')
local uuid = require('uuid')
local varbinary = require('varbinary')
local msgpack = require('msgpack')

-- Rule 1 helper: produce a sort key for a row
local function row_sort_key(row)
    -- SQL MAP/ARRAY values have pointer-like tostring() representations.
    -- Sort by the same typed, canonical encoding used in snapshots instead.
    return yaml_emitter.emit_nodoc(row)
end

-- Rule 3-7: canonicalize a single cell value
local function canon_value(v, seen)
    if v == nil or v == box.NULL then
        return box.NULL  -- preserve array positions; emitter writes null
    end
    local t = type(v)
    if t == 'boolean' then
        return v  -- Rule 4
    end
    if t == 'number' then
        -- Rule 2: float normalization (leave as number; emitter formats)
        return v
    end
    if t == 'cdata' then
        local kind
        if ffi.istype('int64_t', v) then kind = 'int64'
        elseif ffi.istype('uint64_t', v) then kind = 'uint64'
        elseif decimal.is_decimal(v) then kind = 'decimal'
        elseif datetime.is_datetime(v) then kind = 'datetime'
        elseif ffi.istype('struct interval', v) then kind = 'interval'
        elseif uuid.is_uuid(v) then kind = 'uuid'
        elseif varbinary.is(v) then kind = 'varbinary'
        else error('unsupported SQL cdata value: ' .. tostring(ffi.typeof(v))) end
        local value = tostring(v)
        if kind == 'int64' or kind == 'uint64' then
            value = value:gsub('ULL$', ''):gsub('LL$', '')
        end
        return {sql_type = kind, value = value}
    end
    if t == 'string' then
        -- Rule 5: blobs — pass through; emitter detects and base64-encodes
        return v
    end
    if t == 'table' then
        if seen[v] then error('cyclic SQL container cannot be snapshotted') end
        seen[v] = true
        local meta = getmetatable(v)
        local kind = meta and meta.__serialize
        if kind == 'seq' or (kind ~= 'map' and #v > 0) then
            local items = {}
            for i = 1, #v do items[i] = canon_value(v[i], seen) end
            seen[v] = nil
            return {sql_type = 'array', items = items}
        end
        local entries = {}
        for k, child in pairs(v) do
            entries[#entries + 1] = {
                key = canon_value(k, seen),
                value = canon_value(child, seen),
            }
        end
        table.sort(entries, function(a, b)
            return yaml_emitter.emit_nodoc(a.key) <
                   yaml_emitter.emit_nodoc(b.key)
        end)
        seen[v] = nil
        return {sql_type = 'map', entries = entries}
    end
    -- Fallback
    return tostring(v)
end

-- Canonicalize a single row (list of values)
local function canon_row(row)
    local result = {}
    for i, v in ipairs(row) do
        result[i] = canon_value(v, {})
    end
    return result
end

-- Rule 1: Sort rows unless ordered = true
-- rows: list of rows (each row is a list of values)
-- ordered: boolean
local function sort_rows(rows, ordered)
    if ordered then return rows end
    local copy = {}
    for i, r in ipairs(rows) do
        copy[i] = r
    end
    table.sort(copy, function(a, b)
        return row_sort_key(a) < row_sort_key(b)
    end)
    return copy
end

-- Detect a top-level ORDER BY. A nested sort does not order the outer result,
-- and text in comments or quoted strings/identifiers is not SQL syntax.
function M.has_order_by(sql)
    if type(sql) ~= 'string' then return false end
    local i, n, depth, order_seen = 1, #sql, 0, false
    while i <= n do
        local ch, next_ch = sql:sub(i, i), sql:sub(i + 1, i + 1)
        if ch == '-' and next_ch == '-' then
            i = i + 2
            while i <= n and sql:sub(i, i) ~= '\n' do i = i + 1 end
        elseif ch == '/' and next_ch == '*' then
            i = i + 2
            while i <= n and sql:sub(i, i + 1) ~= '*/' do i = i + 1 end
            i = i + 2
        elseif ch == "'" or ch == '"' or ch == '`' then
            local quote = ch
            order_seen = false
            i = i + 1
            while i <= n do
                if sql:sub(i, i) == quote then
                    if sql:sub(i + 1, i + 1) == quote then
                        i = i + 2
                    else
                        i = i + 1
                        break
                    end
                else
                    i = i + 1
                end
            end
        elseif ch == '[' then
            order_seen = false
            i = i + 1
            while i <= n and sql:sub(i, i) ~= ']' do i = i + 1 end
            i = i + 1
        elseif ch == '(' then
            depth = depth + 1
            order_seen = false
            i = i + 1
        elseif ch == ')' then
            depth = math.max(depth - 1, 0)
            order_seen = false
            i = i + 1
        elseif ch:match('[%a_]') then
            local start = i
            repeat i = i + 1 until i > n or not sql:sub(i, i):match('[%w_]')
            if depth == 0 then
                local word = sql:sub(start, i - 1):upper()
                if order_seen and word == 'BY' then return true end
                order_seen = word == 'ORDER'
            end
        else
            if not ch:match('%s') then order_seen = false end
            i = i + 1
        end
    end
    return false
end

-- Canonicalize L1: apply all 8 rules to result rows
-- rows: list of rows from box.execute (may be box tuples)
-- ordered: whether results are already ordered (true = don't sort)
-- metadata: box.execute column descriptors (when the statement has columns)
-- Returns: { rows = [...], rows_sorted = bool, column_names = [...],
--            column_types = [...] }
local function normalize_generated_column_name(name)
    -- select.c:5015-5023 names anonymous FROM-subquery spaces by formatting
    -- their address as sql_sq_%llX. A full column name can expose that
    -- process-local address. Limit normalization to the observed generated
    -- .COLUMN_N suffix so arbitrary user-authored aliases are not masked.
    local column = name:match('^sql_sq_[0-9A-F]+%.COLUMN_(%d+)$')
    if column then return 'sql_sq_<generated>.COLUMN_' .. column end
    return name
end

function M.canon_L1(rows, ordered, metadata, sql)
    local column_names, column_types = nil, nil
    if metadata ~= nil then
        column_names, column_types = {}, {}
        for i, column in ipairs(metadata) do
            assert(type(column.name) == 'string', 'invalid SQL column name')
            assert(type(column.type) == 'string', 'invalid SQL column type')
            column_names[i] = normalize_generated_column_name(column.name)
            column_types[i] = column.type
        end
    end
    if rows == nil then
        return { rows = {}, rows_sorted = not ordered,
                 column_names = column_names, column_types = column_types }
    end

    -- Flatten box tuples to plain Lua tables
    local plain = {}
    for _, row in ipairs(rows) do
        local r = {}
        if type(row) == 'table' or (type(row) == 'cdata' and box.tuple.is(row)) then
            for i = 1, #row do
                r[i] = row[i]
            end
        else
            r[1] = row
        end
        table.insert(plain, r)
    end

    -- Planner snapshot EXPLAIN is itself a MsgPack diagnostic envelope. Its
    -- elapsed_us field is intentionally nondeterministic across dispatchers
    -- and repeated runs; planner_metrics in the manifest carries the same
    -- structural evidence separately. Canonicalize only this volatile field
    -- while preserving the result's VARBINARY type and all other fields.
    local explain_options = type(sql) == 'string' and
        sql:lower():match('^%s*explain%s*%((.-)%)')
    local is_planner_snapshot = type(explain_options) == 'string' and
        (explain_options:match("planner%s*=%s*'snapshot'") ~= nil or
         explain_options:match('planner%s*=%s*"snapshot"') ~= nil)
    if is_planner_snapshot then
        for _, row in ipairs(plain) do
            if varbinary.is(row[1]) then
                local ok, envelope = pcall(msgpack.decode, tostring(row[1]))
                if ok and type(envelope) == 'table' and
                   envelope.format == 'tarantool.sql.planner.snapshot' and
                   type(envelope.planner) == 'table' then
                    envelope.planner.elapsed_us = 0
                    row[1] = varbinary.new(msgpack.encode(envelope))
                end
            end
        end
    end

    -- EXPLAIN's OpenTEphemeral P4 is a raw sql_space_info struct pointer,
    -- not a printable semantic field: vdbeaux.c displayP4() returns p4.z
    -- for this P4_DYNAMIC opcode. Its bytes vary per process. Keep every
    -- other opcode/P4 intact, and only rewrite nonempty P4 in EXPLAIN output.
    local is_explain = type(sql) == 'string' and
                       sql:match('^%s*[Ee][Xx][Pp][Ll][Aa][Ii][Nn]%s') ~= nil
    if is_explain then
        for _, row in ipairs(plain) do
            if row[2] == 'OpenTEphemeral' and row[6] ~= nil and
               row[6] ~= '' then
                row[6] = '<sql_space_info>'
            end
        end
    end

    -- Apply per-cell canonicalization
    local canon = {}
    for _, row in ipairs(plain) do
        table.insert(canon, canon_row(row))
    end

    -- Rule 1: sort if not ordered
    local sorted = sort_rows(canon, ordered or false)

    return { rows = sorted, rows_sorted = not ordered,
             column_names = column_names, column_types = column_types }
end

-- Canonicalize L2: normalize error messages
-- status: 'ok' or 'error'
-- err: the error object or string (nil if ok)
-- Returns: { status=, error_code=, error_msg= }
function M.canon_L2(status, err)
    if status == 'ok' or err == nil then
        return { status = 'ok', error_code = nil, error_msg = nil }
    end

    local code = nil
    local msg = nil

    if type(err) == 'table' and err.code then
        code = err.code
        msg = tostring(err.message or err)
    elseif type(err) == 'cdata' then
        -- box.error cdata object
        local ok, e = pcall(function() return err.code end)
        if ok then code = e end
        msg = tostring(err)
    else
        msg = tostring(err)
    end

    -- Canonicalize error message:
    -- Strip absolute path prefixes like /home/user/path/file.lua:NN:
    if msg then
        msg = msg:gsub('/[%w/._-]+%.lua:%d+: ', '<src>: ')
        -- Strip "at line N" patterns
        msg = msg:gsub(' at line %d+', '')
    end

    if code ~= nil then code = tostring(code) end
    return {
        status = 'error',
        error_code = code,
        error_message_canonical = msg,
    }
end

return M

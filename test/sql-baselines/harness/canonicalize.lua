-- canonicalize.lua
-- Applies the 8 canonicalization rules from SCHEMA.md to L1 rows and L2 diagnostics.

local M = {}
local source = debug.getinfo(1, 'S').source
local dir = source:sub(1, 1) == '@' and source:sub(2):match('^(.+)/[^/]+$') or '.'
local yaml_emitter = dofile(dir .. '/../lib/canonical_yaml.lua')

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
        -- int64/uint64 cdata — pass through; emitter handles Rule 7
        return v
    end
    if t == 'string' then
        -- Rule 5: blobs — pass through; emitter detects and base64-encodes
        return v
    end
    if t == 'table' then
        if seen[v] then error('cyclic SQL container cannot be snapshotted') end
        seen[v] = true
        local result = {}
        for k, child in pairs(v) do
            result[k] = canon_value(child, seen)
        end
        seen[v] = nil
        local meta = getmetatable(v)
        local kind = meta and meta.__serialize
        if kind == 'map' or kind == 'seq' then
            setmetatable(result, {__serialize = kind})
        end
        return result
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
function M.canon_L1(rows, ordered, metadata)
    local column_names, column_types = nil, nil
    if metadata ~= nil then
        column_names, column_types = {}, {}
        for i, column in ipairs(metadata) do
            assert(type(column.name) == 'string', 'invalid SQL column name')
            assert(type(column.type) == 'string', 'invalid SQL column type')
            column_names[i] = column.name
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

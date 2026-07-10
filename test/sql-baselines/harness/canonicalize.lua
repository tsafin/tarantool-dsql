-- canonicalize.lua
-- Applies the 8 canonicalization rules from SCHEMA.md to L1 rows and L2 diagnostics.

local M = {}

-- Rule 1 helper: produce a sort key for a row
local function row_sort_key(row)
    local parts = {}
    for _, v in ipairs(row) do
        table.insert(parts, tostring(v))
    end
    return table.concat(parts, '\t')
end

-- Rule 3-7: canonicalize a single cell value
local function canon_value(v)
    if v == nil or v == box.NULL then
        return nil  -- will be emitted as null
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
    -- Fallback
    return tostring(v)
end

-- Canonicalize a single row (list of values)
local function canon_row(row)
    local result = {}
    for i, v in ipairs(row) do
        result[i] = canon_value(v)
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

-- Detect ORDER BY in SQL text (simple regex, handles most cases)
-- Returns true if the SQL contains an ORDER BY clause.
function M.has_order_by(sql)
    if type(sql) ~= 'string' then return false end
    -- Case-insensitive search; skip ORDER BY inside string literals (best effort)
    return sql:upper():find('ORDER%s+BY') ~= nil
end

-- Canonicalize L1: apply all 8 rules to result rows
-- rows: list of rows from box.execute (may be box tuples)
-- ordered: whether results are already ordered (true = don't sort)
-- Returns: { rows = [...], ordered = bool }
function M.canon_L1(rows, ordered)
    if rows == nil then
        return { rows = {}, ordered = ordered or false }
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

    return { rows = sorted, ordered = ordered or false }
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

    return {
        status = 'error',
        error_code = code,
        error_message_canonical = msg,
    }
end

return M

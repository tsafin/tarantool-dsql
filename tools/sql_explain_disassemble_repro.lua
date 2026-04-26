box.cfg{}

local json = require('json')

local function is_array(t)
    local n = #t
    for k, _ in pairs(t) do
        if type(k) ~= 'number' or k < 1 or k > n or k % 1 ~= 0 then
            return false
        end
    end
    return true
end

local function pretty_json(v, indent)
    indent = indent or 0
    local pad = string.rep(' ', indent)
    local next_pad = string.rep(' ', indent + 2)

    if type(v) ~= 'table' then
        return json.encode(v)
    end

    local parts = {}
    if is_array(v) then
        if #v == 0 then
            return '[]'
        end
        table.insert(parts, '[\n')
        for i, item in ipairs(v) do
            table.insert(parts, next_pad)
            table.insert(parts, pretty_json(item, indent + 2))
            if i < #v then
                table.insert(parts, ',')
            end
            table.insert(parts, '\n')
        end
        table.insert(parts, pad)
        table.insert(parts, ']')
        return table.concat(parts)
    end

    local keys = {}
    for k, _ in pairs(v) do
        table.insert(keys, k)
    end
    table.sort(keys, function(a, b)
        return tostring(a) < tostring(b)
    end)

    if #keys == 0 then
        return '{}'
    end

    table.insert(parts, '{\n')
    for i, k in ipairs(keys) do
        table.insert(parts, next_pad)
        table.insert(parts, json.encode(k))
        table.insert(parts, ': ')
        table.insert(parts, pretty_json(v[k], indent + 2))
        if i < #keys then
            table.insert(parts, ',')
        end
        table.insert(parts, '\n')
    end
    table.insert(parts, pad)
    table.insert(parts, '}')
    return table.concat(parts)
end

local function measure(sql)
    local stat = box.stat.sql()
    local before = stat.sql_cnp_compiled_bytes or 0
    local result = box.execute(sql)
    local after = box.stat.sql().sql_cnp_compiled_bytes or 0

    local text_bytes = 0
    for _, row in ipairs(result.rows) do
        if row[1] == 'disassembly' then
            text_bytes = text_bytes + #row[3] + 1
        end
    end

    return {
        sql = sql,
        cnp_bytes_delta = after - before,
        row_count = #result.rows,
        disasm_text_bytes = text_bytes,
        metadata = result.metadata,
        rows = result.rows,
    }
end

local res = measure([[EXPLAIN (bytecode=yes,disassemble=yes) SELECT 1 + 2 + 3;]])
print(pretty_json(res))

os.exit(0)

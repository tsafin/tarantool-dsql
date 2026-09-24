-- canonical_yaml.lua
-- Canonical YAML emitter for the M0 parity corpus.
--
-- Contract (from test/sql-baselines/SCHEMA.md):
--   - sorted map keys at every nesting depth
--   - LF line endings, single trailing newline at EOF
--   - deterministic scalar formatting:
--       booleans as "true" / "false"
--       nil and box.NULL as "null"
--       integers as tostring(v); int64/uint64 cdata as "!int64 <n>"
--       finite floats as string.format("%.17g", v), forced to include "." or "e"
--       strings quoted (single-quote style) when needed, base64-encoded
--         with !!binary tag when they contain non-printable bytes
--   - round-trip stable through Tarantool's yaml.decode
--
-- Public API:
--   M.emit(value)            -- takes any Lua table/scalar, emits "---\n<doc>\n"
--   M.emit_nodoc(value)      -- same as M.emit without leading "---" marker
--   M.emit_to_file(v, path)  -- writes M.emit(v) to path
--
-- classify.lua uses this for classification.yaml.
-- The M0.2 harness uses this for per-query snapshot YAML — the SCHEMA.md v1
-- doc structure emits cleanly under the generic sorted-key contract.

local M = {}

-- ---------------------------------------------------------------------------
-- Base64 encoder (used by !!binary blob emission)
-- ---------------------------------------------------------------------------

local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function base64_encode(data)
    return ((data:gsub(".", function(x)
        local r, b = "", x:byte()
        for i = 8, 1, -1 do
            r = r .. (b % 2 ^ i - b % 2 ^ (i - 1) > 0 and "1" or "0")
        end
        return r
    end) .. "0000"):gsub("%d%d%d?%d?%d?%d?", function(x)
        if #x < 6 then return "" end
        local c = 0
        for i = 1, 6 do
            c = c + (x:sub(i, i) == "1" and 2 ^ (6 - i) or 0)
        end
        return B64:sub(c + 1, c + 1)
    end) .. ({ "", "==", "=" })[#data % 3 + 1])
end

-- ---------------------------------------------------------------------------
-- Scalar formatting helpers
-- ---------------------------------------------------------------------------

-- Reserved YAML keywords that must be quoted to avoid misinterpretation.
local RESERVED = {
    ["true"] = true, ["false"] = true, ["null"] = true,
    ["yes"] = true, ["no"] = true, ["on"] = true, ["off"] = true,
    ["~"] = true,
}

-- Characters that, if present anywhere in a scalar, force quoting.
local YAML_SPECIAL = "{}[],:#&*!|>'\"%@`"

-- Valid UTF-8 is text, not a blob. A byte >= 0x80 alone does not make a
-- string binary: SQL TEXT commonly contains non-ASCII characters.
local function valid_utf8(s)
    local i = 1
    while i <= #s do
        local c = s:byte(i)
        if c < 0x80 then
            i = i + 1
        else
            local n, low, high
            if c >= 0xc2 and c <= 0xdf then
                n, low, high = 2, 0x80, 0xbf
            elseif c == 0xe0 then
                n, low, high = 3, 0xa0, 0xbf
            elseif c >= 0xe1 and c <= 0xec or c >= 0xee and c <= 0xef then
                n, low, high = 3, 0x80, 0xbf
            elseif c == 0xed then
                n, low, high = 3, 0x80, 0x9f
            elseif c == 0xf0 then
                n, low, high = 4, 0x90, 0xbf
            elseif c >= 0xf1 and c <= 0xf3 then
                n, low, high = 4, 0x80, 0xbf
            elseif c == 0xf4 then
                n, low, high = 4, 0x80, 0x8f
            else
                return false
            end
            local second = s:byte(i + 1)
            if not second or second < low or second > high then return false end
            for j = i + 2, i + n - 1 do
                local continuation = s:byte(j)
                if not continuation or continuation < 0x80 or
                   continuation > 0xbf then return false end
            end
            i = i + n
        end
    end
    return true
end

-- Emit control-byte or invalid-UTF-8 strings as binary. TAB, LF, CR are kept
-- as text, and valid Unicode remains UTF-8 text in YAML.
local function is_blob(s)
    for i = 1, #s do
        local b = s:byte(i)
        if (b < 0x20 and b ~= 0x09 and b ~= 0x0a and b ~= 0x0d) or
           b == 0x7f then
            return true
        end
    end
    return not valid_utf8(s)
end

-- Returns true when a plain-scalar string must be quoted.
local function needs_quoting(s)
    if s == "" then return true end
    if RESERVED[s:lower()] then return true end
    if tonumber(s) ~= nil then return true end
    if s:match("^%s") or s:match("%s$") then return true end
    if s:find("\n", 1, true) or s:find("\r", 1, true) then return true end
    for i = 1, #YAML_SPECIAL do
        if s:find(YAML_SPECIAL:sub(i, i), 1, true) then return true end
    end
    return false
end

-- Single-quote a string, doubling any embedded single quotes per YAML 1.1.
local function single_quote(s)
    return "'" .. s:gsub("'", "''") .. "'"
end

-- Format a Lua number as either a safe integer or a round-trippable float.
-- NaN/Inf are rejected until v1 defines a typed non-finite representation;
-- silently emitting strings would conflate them with SQL TEXT.
local function format_number(v)
    if v ~= v or v == math.huge or v == -math.huge then
        error("canonical_yaml: non-finite SQL number is unsupported")
    end
    local safe_integer = v == math.floor(v) and math.abs(v) <= 2 ^ 53
    if not safe_integer then
        local s = string.format("%.17g", v)
        if not s:find("[%.e]") then
            s = s .. ".0"
        end
        return s
    end
    return tostring(math.floor(v))
end

-- Emit a Lua value as a YAML scalar string. Handles nil, box.NULL, boolean,
-- number, cdata (Tarantool int64/uint64), string (plain, quoted, blob).
local function emit_scalar(v)
    if v == nil then return "null" end
    if box and v == box.NULL then return "null" end
    local t = type(v)
    if t == "boolean" then
        return v and "true" or "false"
    end
    if t == "number" then
        return format_number(v)
    end
    if t == "cdata" then
        local s = tostring(v)
        s = s:gsub("ULL$", ""):gsub("LL$", "")
        return "!int64 " .. s
    end
    if t == "string" then
        if is_blob(v) then
            return "!!binary " .. base64_encode(v)
        end
        if needs_quoting(v) then
            return single_quote(v)
        end
        return v
    end
    -- Fallback: whatever it is, stringify and quote if needed.
    local s = tostring(v)
    if needs_quoting(s) then return single_quote(s) end
    return s
end

-- ---------------------------------------------------------------------------
-- Recursive emitter — generic path (M.emit, M.emit_nodoc)
-- ---------------------------------------------------------------------------

-- emit_value returns a list of lines. Every line already carries indent_str
-- as its prefix — callers do NOT re-indent sub-lines.
local function emit_value(v, indent_str)
    local lines = {}

    if type(v) == "table" then
        -- Array vs map: array only if keys are 1..N consecutive.
        local meta = getmetatable(v)
        local kind = meta and meta.__serialize
        local is_array = kind ~= "map"
        local max_int = 0
        for k in pairs(v) do
            if type(k) == "number" and k == math.floor(k) and k >= 1 then
                if k > max_int then max_int = k end
            else
                is_array = false
                break
            end
        end
        if is_array and max_int ~= #v then is_array = false end
        if kind == "seq" and not is_array then
            error("canonical_yaml: invalid SQL sequence")
        end

        if is_array and #v == 0 then
            lines[#lines + 1] = indent_str .. "[]"
        elseif is_array then
            local child_indent = indent_str .. "  "
            for i = 1, #v do
                local elem = v[i]
                if type(elem) == "table" then
                    local sub = emit_value(elem, child_indent)
                    if #sub == 0 then
                        lines[#lines + 1] = indent_str .. "- {}"
                    else
                        -- Sub[1] carries child_indent; strip it and replace
                        -- with "indent_str- ". Continuation lines stay as-is.
                        local first = sub[1]:sub(#child_indent + 1)
                        lines[#lines + 1] = indent_str .. "- " .. first
                        for j = 2, #sub do
                            lines[#lines + 1] = sub[j]
                        end
                    end
                else
                    lines[#lines + 1] = indent_str .. "- " .. emit_scalar(elem)
                end
            end
        else
            -- Map: alphabetically sorted keys.
            local keys = {}
            for k in pairs(v) do keys[#keys + 1] = k end
            table.sort(keys, function(a, b)
                if tostring(a) == tostring(b) then
                    return type(a) < type(b)
                end
                return tostring(a) < tostring(b)
            end)

            if #keys == 0 then
                lines[#lines + 1] = indent_str .. "{}"
            else
                local child_indent = indent_str .. "  "
                for _, k in ipairs(keys) do
                    local val = v[k]
                    local key_str = emit_scalar(k)

                    if type(val) == "table" then
                        local has_content = false
                        for _ in pairs(val) do has_content = true; break end
                        if not has_content then
                            local val_meta = getmetatable(val)
                            local empty_kind = val_meta and val_meta.__serialize
                            lines[#lines + 1] = indent_str .. key_str ..
                                                 (empty_kind == 'map' and ': {}' or ': []')
                        else
                            -- Try inline [a, b, c] for scalar-only arrays.
                            local short_inline = true
                            local val_meta = getmetatable(val)
                            local val_kind = val_meta and val_meta.__serialize
                            if #val > 0 and val_kind ~= 'map' then
                                for _, elem in ipairs(val) do
                                    if type(elem) == "table" then
                                        short_inline = false; break
                                    end
                                end
                            else
                                short_inline = false
                            end

                            if short_inline then
                                local parts = {}
                                for _, elem in ipairs(val) do
                                    parts[#parts + 1] = emit_scalar(elem)
                                end
                                local inline = "[" .. table.concat(parts, ", ") .. "]"
                                local line = indent_str .. key_str .. ": " .. inline
                                if #line <= 100 then
                                    lines[#lines + 1] = line
                                else
                                    short_inline = false
                                end
                            end

                            if not short_inline then
                                lines[#lines + 1] = indent_str .. key_str .. ":"
                                local sub = emit_value(val, child_indent)
                                for _, l in ipairs(sub) do
                                    lines[#lines + 1] = l
                                end
                            end
                        end
                    else
                        lines[#lines + 1] = indent_str .. key_str .. ": " .. emit_scalar(val)
                    end
                end
            end
        end
    else
        lines[#lines + 1] = indent_str .. emit_scalar(v)
    end

    return lines
end

-- ---------------------------------------------------------------------------
-- Public API — generic
-- ---------------------------------------------------------------------------

function M.emit(value)
    local lines = emit_value(value, "")
    return "---\n" .. table.concat(lines, "\n") .. "\n"
end

function M.emit_nodoc(value)
    local lines = emit_value(value, "")
    return table.concat(lines, "\n") .. "\n"
end

function M.emit_to_file(value, path)
    local f, err = io.open(path, "w")
    if not f then
        error("canonical_yaml: cannot open '" .. path .. "': " .. tostring(err))
    end
    f:write(M.emit(value))
    f:close()
end

return M

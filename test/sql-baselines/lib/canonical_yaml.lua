-- canonical_yaml.lua
-- Minimal canonical-YAML emitter for the M0 parity corpus.
--
-- Contract (from SCHEMA.md):
--   - sorted map keys at every nesting depth
--   - normalized strings (quoted when needed)
--   - LF line endings
--   - single trailing newline at EOF
--   - round-trip stable: parse(emit(parse(x))) == parse(x)
--
-- This is a *write-only* emitter, not a full YAML library.
-- It handles the value types that appear in classification.yaml and
-- snapshot YAML: strings, numbers, booleans, nil/null, arrays, maps.
-- It does NOT handle anchors, aliases, multi-document streams, or
-- binary/float special tags — those are snapshot harness concerns.

local M = {}

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

-- Characters that require quoting in a plain YAML scalar.
local SAFE_PLAIN = "^[%w%-%._/: ]+$"

-- Keywords that must be quoted to avoid YAML misinterpretation.
local RESERVED = {
    ["true"] = true, ["false"] = true, ["null"] = true,
    ["yes"] = true, ["no"] = true, ["on"] = true, ["off"] = true,
    ["~"] = true,
}

-- Returns true when a string value can be emitted as an unquoted plain scalar.
local function is_safe_plain(s)
    if type(s) ~= "string" then return false end
    if s == "" then return false end
    if RESERVED[s:lower()] then return false end
    -- Must not start with special YAML characters
    local first = s:sub(1, 1)
    if first == "-" or first == ":" or first == "#" or first == "&"
        or first == "*" or first == "!" or first == "|" or first == ">"
        or first == "'" or first == '"' or first == "{"  or first == "["
        or first == "}" or first == "]" or first == "," or first == "@"
        or first == "`" or first == "%" then
        return false
    end
    -- Must not look like a number (could be misread)
    if tonumber(s) ~= nil then return false end
    -- Must contain only safe characters
    return s:match(SAFE_PLAIN) ~= nil
end

-- Escape a string for double-quoted YAML scalar.
local function dquote(s)
    -- Replace backslash first, then special chars
    s = s:gsub("\\", "\\\\")
    s = s:gsub('"', '\\"')
    s = s:gsub("\n", "\\n")
    s = s:gsub("\r", "\\r")
    s = s:gsub("\t", "\\t")
    -- Control chars
    s = s:gsub("[\0-\31\127]", function(c)
        return string.format("\\u%04x", string.byte(c))
    end)
    return '"' .. s .. '"'
end

-- Emit a scalar value (string, number, boolean, nil).
local function emit_scalar(v)
    local t = type(v)
    if v == nil then
        return "null"
    elseif t == "boolean" then
        return tostring(v)   -- "true" or "false"
    elseif t == "number" then
        return tostring(v)
    elseif t == "string" then
        if is_safe_plain(v) then
            return v
        else
            return dquote(v)
        end
    else
        -- Fallback: stringify whatever it is
        return dquote(tostring(v))
    end
end

-- ---------------------------------------------------------------------------
-- Core recursive emitter
-- ---------------------------------------------------------------------------

-- indent_str: the indentation string for the *current* level
-- Returns a table of lines (without trailing newline).
local function emit_value(v, indent_str)
    local t = type(v)
    local lines = {}

    if t == "table" then
        -- Decide: array or map?
        -- We treat a table as an array if it has only consecutive integer keys
        -- starting from 1 and no string keys.
        local is_array = true
        local max_int = 0
        for k, _ in pairs(v) do
            if type(k) == "number" and k == math.floor(k) and k >= 1 then
                if k > max_int then max_int = k end
            else
                is_array = false
                break
            end
        end
        if is_array and max_int ~= #v then
            is_array = false
        end

        if is_array and #v == 0 then
            -- Empty array
            lines[#lines + 1] = indent_str .. "[]"
        elseif is_array then
            -- Array: each element on its own line with "- " prefix.
            -- Recursive emit_value bakes child_indent into every returned line;
            -- we strip that prefix from the first line to replace it with
            -- "indent_str- ", and keep subsequent lines as-is.
            local child_indent = indent_str .. "  "
            for i = 1, #v do
                local elem = v[i]
                local et = type(elem)
                if et == "table" then
                    local sub = emit_value(elem, child_indent)
                    if #sub == 0 then
                        lines[#lines + 1] = indent_str .. "- {}"
                    else
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
            -- Map: sort keys alphabetically
            local keys = {}
            for k in pairs(v) do
                keys[#keys + 1] = k
            end
            table.sort(keys, function(a, b)
                return tostring(a) < tostring(b)
            end)

            if #keys == 0 then
                lines[#lines + 1] = indent_str .. "{}"
            else
                local child_indent = indent_str .. "  "
                for _, k in ipairs(keys) do
                    local val = v[k]
                    local key_str = emit_scalar(tostring(k))
                    local vt = type(val)
                    if vt == "table" then
                        -- Check if value is empty
                        local has_content = false
                        for _ in pairs(val) do has_content = true; break end
                        if not has_content then
                            lines[#lines + 1] = indent_str .. key_str .. ": {}"
                        else
                            -- Is it a simple inline array of scalars?
                            local short_inline = true
                            if #val > 0 then
                                for _, elem in ipairs(val) do
                                    if type(elem) == "table" then
                                        short_inline = false; break
                                    end
                                end
                            else
                                short_inline = false
                            end

                            if short_inline and #val > 0 then
                                -- Inline array: [a, b, c]
                                local parts = {}
                                for _, elem in ipairs(val) do
                                    parts[#parts + 1] = emit_scalar(elem)
                                end
                                local inline = "[" .. table.concat(parts, ", ") .. "]"
                                -- Keep inline if short enough (<= 100 chars)
                                local line = indent_str .. key_str .. ": " .. inline
                                if #line <= 100 then
                                    lines[#lines + 1] = line
                                else
                                    -- Fall through to block style
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
-- Public API
-- ---------------------------------------------------------------------------

-- M.emit(value) -> string
-- Serializes `value` as a canonical YAML document (with leading "---\n").
-- The returned string ends with exactly one LF.
function M.emit(value)
    local lines = emit_value(value, "")
    local result = "---\n" .. table.concat(lines, "\n") .. "\n"
    return result
end

-- M.emit_nodoc(value) -> string
-- Like emit() but without the leading "---\n". Useful for embedded blocks.
function M.emit_nodoc(value)
    local lines = emit_value(value, "")
    return table.concat(lines, "\n") .. "\n"
end

-- M.emit_to_file(value, path)
-- Writes canonical YAML to `path`. Raises on I/O error.
function M.emit_to_file(value, path)
    local f, err = io.open(path, "w")
    if not f then
        error("canonical_yaml: cannot open '" .. path .. "': " .. tostring(err))
    end
    f:write(M.emit(value))
    f:close()
end

return M

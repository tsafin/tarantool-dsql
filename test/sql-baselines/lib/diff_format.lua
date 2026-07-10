-- test/sql-baselines/lib/diff_format.lua
--
-- Output-formatting helpers for the snapshot diff tool (M0.4).
-- Used by test/sql-baselines/diff.lua; may be reused by future tooling.
--
-- No external dependencies beyond Lua standard library + LuaJIT built-ins.

local M = {}

-- Drift categories (returned per snapshot comparison).
M.MATCH              = "MATCH"
M.RESULT_REGRESSION  = "RESULT-REGRESSION"       -- L1 rows diverged (hard gate)
M.DIAGNOSTIC_CHANGE  = "DIAGNOSTIC-CHANGE"       -- L2 status/error_code diverged (hard gate)
M.PATH_CLASS_SHIFT   = "PATH-CLASS-SHIFT"        -- L3 taken/reason diverged (hard gate)
M.SOFT_DRIFT         = "SOFT-DRIFT"              -- column_types / error_message / version (soft)

-- Severity buckets
local HARD_CATEGORIES = {
    [M.RESULT_REGRESSION] = true,
    [M.DIAGNOSTIC_CHANGE] = true,
    [M.PATH_CLASS_SHIFT]  = true,
}

--- Return true if the given category is a hard-gate failure.
function M.is_hard(category)
    return HARD_CATEGORIES[category] == true
end

-- ---------------------------------------------------------------------------
-- Internal helpers
-- ---------------------------------------------------------------------------

local function indent(n)
    return string.rep("  ", n)
end

local function val_to_str(v)
    if v == nil then
        return "<nil>"
    elseif type(v) == "table" then
        -- Shallow pretty-print for small tables
        local parts = {}
        for k, vv in pairs(v) do
            parts[#parts + 1] = tostring(k) .. "=" .. tostring(vv)
        end
        return "{" .. table.concat(parts, ", ") .. "}"
    else
        return tostring(v)
    end
end

-- ---------------------------------------------------------------------------
-- Text formatter
-- ---------------------------------------------------------------------------

--- Format a list of DiffRecord entries as human-readable text.
-- @param records  list of DiffRecord (see diff.lua for schema)
-- @param opts     {show_matches=bool}
-- @return string
function M.format_text(records, opts)
    opts = opts or {}
    local lines = {}
    local hard_count  = 0
    local soft_count  = 0
    local match_count = 0

    for _, rec in ipairs(records) do
        local cat = rec.category
        if cat == M.MATCH then
            match_count = match_count + 1
            if opts.show_matches then
                lines[#lines + 1] = string.format("  MATCH  %s", rec.query_id)
            end
        elseif M.is_hard(cat) then
            hard_count = hard_count + 1
            lines[#lines + 1] = string.format("[HARD] %-22s  %s", cat, rec.query_id)
            for _, field in ipairs(rec.fields or {}) do
                lines[#lines + 1] = indent(2) .. string.format(
                    "%-30s  baseline: %s  candidate: %s",
                    field.name,
                    val_to_str(field.baseline),
                    val_to_str(field.candidate)
                )
            end
        else
            soft_count = soft_count + 1
            lines[#lines + 1] = string.format("[soft] %-22s  %s", cat, rec.query_id)
            for _, field in ipairs(rec.fields or {}) do
                lines[#lines + 1] = indent(2) .. string.format(
                    "%-30s  baseline: %s  candidate: %s",
                    field.name,
                    val_to_str(field.baseline),
                    val_to_str(field.candidate)
                )
            end
        end
    end

    lines[#lines + 1] = ""
    lines[#lines + 1] = string.format(
        "Summary: %d hard regressions, %d soft drifts, %d matches (total %d snapshots)",
        hard_count, soft_count, match_count, #records
    )

    return table.concat(lines, "\n")
end

-- ---------------------------------------------------------------------------
-- YAML formatter (minimal, hand-rolled — no external lib required)
-- ---------------------------------------------------------------------------

-- Escape a string value for inline YAML.
local function yaml_str(s)
    if s == nil then return "~" end
    s = tostring(s)
    -- Quote if the value contains special characters.
    if s:match("[:#{}%[%],&*?|<>=!%%@`'\"%c]") or s == "" then
        return '"' .. s:gsub('"', '\\"'):gsub("\n", "\\n") .. '"'
    end
    return s
end

local function yaml_val(v)
    if v == nil     then return "~" end
    if type(v) == "boolean" then return tostring(v) end
    if type(v) == "number"  then return tostring(v) end
    return yaml_str(tostring(v))
end

--- Format a list of DiffRecord entries as YAML.
-- @param records  list of DiffRecord
-- @param opts     {show_matches=bool}
-- @return string
function M.format_yaml(records, opts)
    opts = opts or {}
    local lines = {}
    local hard_count  = 0
    local soft_count  = 0
    local match_count = 0

    lines[#lines + 1] = "---"
    lines[#lines + 1] = "diffs:"

    for _, rec in ipairs(records) do
        local cat = rec.category
        if cat == M.MATCH then
            match_count = match_count + 1
            if opts.show_matches then
                lines[#lines + 1] = "  - query_id: " .. yaml_str(rec.query_id)
                lines[#lines + 1] = "    category: MATCH"
            end
        else
            if M.is_hard(cat) then hard_count = hard_count + 1
            else soft_count = soft_count + 1 end

            lines[#lines + 1] = "  - query_id: " .. yaml_str(rec.query_id)
            lines[#lines + 1] = "    category: " .. yaml_str(cat)
            if rec.fields and #rec.fields > 0 then
                lines[#lines + 1] = "    fields:"
                for _, field in ipairs(rec.fields) do
                    lines[#lines + 1] = "      - name:      " .. yaml_str(field.name)
                    lines[#lines + 1] = "        baseline:  " .. yaml_val(field.baseline)
                    lines[#lines + 1] = "        candidate: " .. yaml_val(field.candidate)
                end
            end
        end
    end

    lines[#lines + 1] = "summary:"
    lines[#lines + 1] = "  total:       " .. #records
    lines[#lines + 1] = "  hard_count:  " .. hard_count
    lines[#lines + 1] = "  soft_count:  " .. soft_count
    lines[#lines + 1] = "  match_count: " .. match_count
    lines[#lines + 1] = "  passed:      " .. (hard_count == 0 and "true" or "false")

    return table.concat(lines, "\n")
end

-- ---------------------------------------------------------------------------
-- JSON formatter (minimal, hand-rolled)
-- ---------------------------------------------------------------------------

local function json_str(s)
    if s == nil then return "null" end
    s = tostring(s)
    s = s:gsub('\\', '\\\\')
    s = s:gsub('"',  '\\"')
    s = s:gsub('\n', '\\n')
    s = s:gsub('\r', '\\r')
    s = s:gsub('\t', '\\t')
    return '"' .. s .. '"'
end

local function json_val(v)
    if v == nil            then return "null" end
    if type(v) == "boolean"then return tostring(v) end
    if type(v) == "number" then return tostring(v) end
    return json_str(tostring(v))
end

--- Format a list of DiffRecord entries as JSON (for machine consumption).
-- @param records  list of DiffRecord
-- @return string
function M.format_json(records)
    local hard_count  = 0
    local soft_count  = 0
    local match_count = 0
    local diff_items  = {}

    for _, rec in ipairs(records) do
        local cat = rec.category
        if cat == M.MATCH then
            match_count = match_count + 1
        else
            if M.is_hard(cat) then hard_count = hard_count + 1
            else soft_count = soft_count + 1 end

            local field_parts = {}
            for _, field in ipairs(rec.fields or {}) do
                field_parts[#field_parts + 1] = string.format(
                    '{"name":%s,"baseline":%s,"candidate":%s}',
                    json_str(field.name),
                    json_val(field.baseline),
                    json_val(field.candidate)
                )
            end

            diff_items[#diff_items + 1] = string.format(
                '{"query_id":%s,"category":%s,"fields":[%s]}',
                json_str(rec.query_id),
                json_str(cat),
                table.concat(field_parts, ",")
            )
        end
    end

    return string.format(
        '{"diffs":[%s],"summary":{"total":%d,"hard_count":%d,"soft_count":%d,"match_count":%d,"passed":%s}}',
        table.concat(diff_items, ","),
        #records,
        hard_count,
        soft_count,
        match_count,
        hard_count == 0 and "true" or "false"
    )
end

return M

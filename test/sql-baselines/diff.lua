#!/usr/bin/env tarantool
-- test/sql-baselines/diff.lua
--
-- M0.4 snapshot diff tool.
-- Compares two SQL baseline snapshot trees (or single snapshot YAML files),
-- classifies drift per layer, and exits non-zero on any hard-gate mismatch.
--
-- Usage:
--   tarantool test/sql-baselines/diff.lua <baseline_path> <candidate_path> \
--       [--format=text|yaml|json] [--show-matches] [--strict]
--
-- Paths may be:
--   - A directory produced by the M0.2 harness  (scan all *.yaml inside)
--   - A single snapshot YAML file
--
-- Exit codes:
--   0  — no hard-gate failures (there may be soft-gate drifts)
--   1  — one or more hard-gate failures
--   2  — usage / IO error
--
-- Dependencies:
--   lib/diff_format.lua  (this repo, test/sql-baselines/lib/)
--   M0.1 YAML emitter    (stub-imported below — produced by the m0.1 worktree)
--   M0.2 harness         (not imported here; caller runs it before invoking diff.lua)
--
-- Layer contract (from SCHEMA.md, locked):
--   L1 hard  : result.rows
--   L1 soft  : result.column_types
--   L2 hard  : diagnostic.status, diagnostic.error_code
--   L2 soft  : diagnostic.error_message_canonical
--   L3 hard  : path_class.taken, path_class.reason
--   Soft only: captured.tarantool_version
--
-- DiffRecord schema:
--   { query_id  = string,
--     category  = string,        -- one of diff_format.* constants
--     fields    = list of        -- empty for MATCH
--       { name=string, baseline=any, candidate=any } }

-- ---------------------------------------------------------------------------
-- Resolve script location so relative requires work regardless of cwd.
-- ---------------------------------------------------------------------------

local script_dir
do
    local src = debug.getinfo(1, "S").source
    if src:sub(1, 1) == "@" then
        src = src:sub(2)  -- strip leading '@'
    end
    script_dir = src:match("^(.*)/[^/]*$") or "."
end

package.path = script_dir .. "/lib/?.lua;" .. package.path

local fmt = require("diff_format")

-- Snapshot YAML is loaded via Tarantool's built-in yaml module.
local ok_yaml, yaml = pcall(require, "yaml")
if not ok_yaml then
    error("YAML module not available. Run this script via the tarantool binary.", 2)
end

-- ---------------------------------------------------------------------------
-- Argument parsing
-- ---------------------------------------------------------------------------

local function usage()
    io.stderr:write(table.concat({
        "Usage: tarantool test/sql-baselines/diff.lua <baseline> <candidate>",
        "             [--format=text|yaml|json] [--show-matches] [--strict]",
        "",
        "  baseline   / candidate : path to snapshot directory or single YAML file",
        "  --format   : output format (default: text)",
        "  --show-matches : also list MATCH entries in the output",
        "  --strict   : treat soft-gate drifts as failures too",
        "",
    }, "\n"))
    os.exit(2)
end

local baseline_path, candidate_path
local output_format  = "text"
local show_matches   = false
local strict         = false

local argv = arg or {}
local positional = {}
for _, a in ipairs(argv) do
    local k, v = a:match("^%-%-([^=]+)=?(.*)")
    if k then
        if k == "format"       then output_format = v
        elseif k == "show-matches" then show_matches = true
        elseif k == "strict"   then strict = true
        else
            io.stderr:write("Unknown option: " .. a .. "\n")
            usage()
        end
    else
        positional[#positional + 1] = a
    end
end

baseline_path  = positional[1]
candidate_path = positional[2]

if not baseline_path or not candidate_path then
    usage()
end

if output_format ~= "text" and output_format ~= "yaml" and output_format ~= "json" then
    io.stderr:write("Invalid --format value: " .. output_format .. "\n")
    usage()
end

-- ---------------------------------------------------------------------------
-- File system helpers
-- ---------------------------------------------------------------------------

local function path_exists(p)
    local f = io.open(p, "r")
    if f then f:close(); return true end
    return false
end

local function is_dir(p)
    local fio = require("fio")
    local st = fio.stat(p)
    return st ~= nil and st:is_dir()
end

--- Collect all *.yaml files under a path (recursively if it's a directory).
local function collect_yaml_files(p)
    if not path_exists(p) then
        return nil, "path not found: " .. p
    end

    local ok_fio, fio = pcall(require, "fio")
    local files = {}

    if is_dir(p) then
        if ok_fio then
            -- Use fio.glob for directory scan.
            local patterns = {
                p .. "/*.yaml",
                p .. "/**/*.yaml",
            }
            for _, pat in ipairs(patterns) do
                for _, f in ipairs(fio.glob(pat) or {}) do
                    files[#files + 1] = f
                end
            end
            -- fio.glob may not support **, fall back to find.
            if #files == 0 then
                local h = io.popen("find " .. string.format("%q", p) .. " -name '*.yaml' 2>/dev/null")
                if h then
                    for line in h:lines() do
                        files[#files + 1] = line
                    end
                    h:close()
                end
            end
        else
            local h = io.popen("find " .. string.format("%q", p) .. " -name '*.yaml' 2>/dev/null")
            if h then
                for line in h:lines() do
                    files[#files + 1] = line
                end
                h:close()
            end
        end
    else
        -- Single file.
        files[1] = p
    end

    if #files == 0 then
        return nil, "no .yaml files found under: " .. p
    end

    table.sort(files)
    return files
end

-- ---------------------------------------------------------------------------
-- YAML load helpers
-- ---------------------------------------------------------------------------

local function load_yaml_file(path)
    local f, err = io.open(path, "r")
    if not f then
        return nil, "cannot open file: " .. path .. ": " .. tostring(err)
    end
    local content = f:read("*a")
    f:close()

    local ok, data = pcall(yaml.decode, content)
    if not ok then
        return nil, "YAML parse error in " .. path .. ": " .. tostring(data)
    end
    return data
end

--- Derive a stable query_id from a file path by stripping the base prefix and extension.
local function query_id_from_path(base_prefix, file_path)
    local rel = file_path
    if base_prefix and base_prefix ~= "" then
        -- Strip the base prefix (directory path).
        local escaped = base_prefix:gsub("([%.%+%-%*%?%[%]%^%$%(%)%%])", "%%%1")
        rel = file_path:gsub("^" .. escaped .. "/?", "")
    end
    -- Remove .yaml extension.
    rel = rel:gsub("%.yaml$", "")
    return rel
end

-- ---------------------------------------------------------------------------
-- Core comparison logic
-- ---------------------------------------------------------------------------

-- Compare two scalar values; returns true if equal.
local function eq(a, b)
    if type(a) ~= type(b) then return false end
    if type(a) == "table" then
        -- Shallow equality check on plain value lists.
        local na, nb = 0, 0
        for k, v in pairs(a) do
            na = na + 1
            if b[k] ~= v then return false end
        end
        for _ in pairs(b) do nb = nb + 1 end
        return na == nb
    end
    return a == b
end

-- Deep equality for nested row tables.
local function rows_equal(ra, rb)
    if ra == nil and rb == nil then return true end
    if ra == nil or rb == nil  then return false end
    if type(ra) ~= "table" or type(rb) ~= "table" then
        return ra == rb
    end
    if #ra ~= #rb then return false end
    for i = 1, #ra do
        local row_a = ra[i]
        local row_b = rb[i]
        if type(row_a) ~= type(row_b) then return false end
        if type(row_a) == "table" then
            if #row_a ~= #row_b then return false end
            for j = 1, #row_a do
                if tostring(row_a[j]) ~= tostring(row_b[j]) then return false end
            end
        else
            if row_a ~= row_b then return false end
        end
    end
    return true
end

--- Compare one baseline snapshot table against one candidate snapshot table.
-- Returns a DiffRecord.
local function compare_snapshots(query_id, base_snap, cand_snap)
    local fields = {}
    local category = fmt.MATCH

    local function push_field(name, bv, cv)
        fields[#fields + 1] = { name = name, baseline = bv, candidate = cv }
    end

    -- Helper: get nested field safely.
    local function get(t, ...)
        if type(t) ~= "table" then return nil end
        local v = t
        for _, k in ipairs({...}) do
            if type(v) ~= "table" then return nil end
            v = v[k]
        end
        return v
    end

    -- L1: l1_result.rows (hard gate)
    local base_rows = get(base_snap, "l1_result", "rows")
    local cand_rows = get(cand_snap, "l1_result", "rows")
    if not rows_equal(base_rows, cand_rows) then
        push_field("l1_result.rows", base_rows, cand_rows)
        if category == fmt.MATCH then category = fmt.RESULT_REGRESSION end
    end

    -- L2: l2_diagnostic.status (hard gate)
    local base_status = get(base_snap, "l2_diagnostic", "status")
    local cand_status = get(cand_snap, "l2_diagnostic", "status")
    if not eq(base_status, cand_status) then
        push_field("l2_diagnostic.status", base_status, cand_status)
        if category == fmt.MATCH then category = fmt.DIAGNOSTIC_CHANGE end
    end

    -- L2: l2_diagnostic.error_code (hard gate)
    local base_ec = get(base_snap, "l2_diagnostic", "error_code")
    local cand_ec = get(cand_snap, "l2_diagnostic", "error_code")
    if not eq(base_ec, cand_ec) then
        push_field("l2_diagnostic.error_code", base_ec, cand_ec)
        if category == fmt.MATCH then category = fmt.DIAGNOSTIC_CHANGE end
    end

    -- L3: l3_path_class.taken (hard gate)
    local base_taken = get(base_snap, "l3_path_class", "taken")
    local cand_taken = get(cand_snap, "l3_path_class", "taken")
    if not eq(base_taken, cand_taken) then
        push_field("l3_path_class.taken", base_taken, cand_taken)
        if category == fmt.MATCH then category = fmt.PATH_CLASS_SHIFT end
    end

    -- L3: l3_path_class.reason (hard gate)
    local base_reason = get(base_snap, "l3_path_class", "reason")
    local cand_reason = get(cand_snap, "l3_path_class", "reason")
    if not eq(base_reason, cand_reason) then
        push_field("l3_path_class.reason", base_reason, cand_reason)
        if category == fmt.MATCH then category = fmt.PATH_CLASS_SHIFT end
    end

    -- Soft gates (report only)
    local base_ct = get(base_snap, "l1_result", "column_types")
    local cand_ct = get(cand_snap, "l1_result", "column_types")
    if not eq(base_ct, cand_ct) then
        push_field("l1_result.column_types", base_ct, cand_ct)
        if category == fmt.MATCH then category = fmt.SOFT_DRIFT end
    end

    local base_em = get(base_snap, "l2_diagnostic", "error_message_canonical")
    local cand_em = get(cand_snap, "l2_diagnostic", "error_message_canonical")
    if not eq(base_em, cand_em) then
        push_field("l2_diagnostic.error_message_canonical", base_em, cand_em)
        if category == fmt.MATCH then category = fmt.SOFT_DRIFT end
    end

    local base_ver = get(base_snap, "captured", "tarantool_version")
    local cand_ver = get(cand_snap, "captured", "tarantool_version")
    if not eq(base_ver, cand_ver) then
        push_field("captured.tarantool_version", base_ver, cand_ver)
        if category == fmt.MATCH then category = fmt.SOFT_DRIFT end
    end

    return { query_id = query_id, category = category, fields = fields }
end

-- ---------------------------------------------------------------------------
-- Main
-- ---------------------------------------------------------------------------

-- Collect files from both paths.
local base_files, err1 = collect_yaml_files(baseline_path)
local cand_files, err2 = collect_yaml_files(candidate_path)

if not base_files then
    io.stderr:write("Error reading baseline: " .. tostring(err1) .. "\n")
    os.exit(2)
end
if not cand_files then
    io.stderr:write("Error reading candidate: " .. tostring(err2) .. "\n")
    os.exit(2)
end

-- Build lookup map: query_id -> file path, for each side.
local function build_map(files, prefix)
    local map = {}
    for _, f in ipairs(files) do
        local qid = query_id_from_path(prefix, f)
        map[qid] = f
    end
    return map
end

local base_dir  = is_dir(baseline_path)  and baseline_path  or (baseline_path:match("^(.*)/[^/]*$") or ".")
local cand_dir  = is_dir(candidate_path) and candidate_path or (candidate_path:match("^(.*)/[^/]*$") or ".")

local base_map  = build_map(base_files,  base_dir)
local cand_map  = build_map(cand_files,  cand_dir)

-- Union of all query IDs.
local all_ids = {}
local seen = {}
for qid in pairs(base_map) do
    if not seen[qid] then seen[qid] = true; all_ids[#all_ids + 1] = qid end
end
for qid in pairs(cand_map) do
    if not seen[qid] then seen[qid] = true; all_ids[#all_ids + 1] = qid end
end
table.sort(all_ids)

-- Compare.
local records = {}
for _, qid in ipairs(all_ids) do
    local base_file = base_map[qid]
    local cand_file = cand_map[qid]

    if not base_file then
        -- Present in candidate but not baseline: treat as new (soft drift).
        records[#records + 1] = {
            query_id = qid,
            category = fmt.SOFT_DRIFT,
            fields   = {{ name = "_presence", baseline = "missing", candidate = "present" }},
        }
    elseif not cand_file then
        -- Present in baseline but not candidate: treated as regression (hard).
        records[#records + 1] = {
            query_id = qid,
            category = fmt.RESULT_REGRESSION,
            fields   = {{ name = "_presence", baseline = "present", candidate = "missing" }},
        }
    else
        local base_snap, e1 = load_yaml_file(base_file)
        local cand_snap, e2 = load_yaml_file(cand_file)

        if not base_snap then
            io.stderr:write("Warning: cannot load baseline snapshot " .. base_file .. ": " .. tostring(e1) .. "\n")
        elseif not cand_snap then
            io.stderr:write("Warning: cannot load candidate snapshot " .. cand_file .. ": " .. tostring(e2) .. "\n")
        else
            records[#records + 1] = compare_snapshots(qid, base_snap, cand_snap)
        end
    end
end

-- Produce output.
local output
if output_format == "yaml" then
    output = fmt.format_yaml(records, { show_matches = show_matches })
elseif output_format == "json" then
    output = fmt.format_json(records)
else
    output = fmt.format_text(records, { show_matches = show_matches })
end

print(output)

-- Determine exit code.
local has_hard = false
local has_soft = false
for _, rec in ipairs(records) do
    if fmt.is_hard(rec.category) then
        has_hard = true
    elseif rec.category ~= fmt.MATCH then
        has_soft = true
    end
end

if has_hard then
    os.exit(1)
elseif strict and has_soft then
    os.exit(1)
else
    os.exit(0)
end

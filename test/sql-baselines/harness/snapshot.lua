-- snapshot.lua
-- Builds and writes per-query snapshot YAML files.
-- See test/sql-baselines/SCHEMA.md for the locked v1 schema contract.
--
-- Top-level keys emitted (canonical sorted order via lib/canonical_yaml.lua):
--   captured, engine, l1_result, l2_diagnostic, l3_path_class,
--   metadata, schema_version, test

local fio = require('fio')

-- Resolve lib path relative to this file's location
local harness_dir = debug.getinfo(1, 'S').source:match('^@(.+)/[^/]+$') or '.'
local lib_dir = harness_dir .. '/../lib'
package.path = package.path .. ';' .. lib_dir .. '/?.lua'

local yaml_emitter = require('canonical_yaml')
local canonicalize = require('canonicalize')

local M = {}

-- Load classification.yaml if present.
-- Returns a table keyed by "<suite>/<test_basename>" → list of tags.
-- If missing, logs a warning and returns {}.
--
-- classification.yaml (from M0.1 classify.lua) has shape:
--     tests:
--       - suite: sql
--         file: check-clear-ephemeral.test.lua
--         feature_tags: [cte, ddl, ...]
-- Test basename is the file stem (extension stripped).
local yaml_decode = require('yaml').decode
local classification_cache = nil
local function load_classification(baselines_root)
    if classification_cache ~= nil then return classification_cache end
    local path = baselines_root .. '/classification.yaml'
    local f = io.open(path, 'r')
    if not f then
        io.stderr:write('[WARN] classification.yaml not found at ' .. path ..
            ' — feature_tags will be empty. Run M0.1 classifier first.\n')
        classification_cache = {}
        return classification_cache
    end
    local text = f:read('*a')
    f:close()
    local ok, doc = pcall(yaml_decode, text)
    if not ok or type(doc) ~= 'table' or type(doc.tests) ~= 'table' then
        io.stderr:write('[WARN] classification.yaml parse failed at ' .. path ..
            ' — feature_tags will be empty.\n')
        classification_cache = {}
        return classification_cache
    end
    local result = {}
    for _, t in ipairs(doc.tests) do
        if t.suite and t.file then
            local basename = t.file:match('^(.+)%.test%.lua$')
                          or t.file:match('^(.+)%.lua$')
                          or t.file
            result[t.suite .. '/' .. basename] = t.feature_tags or {}
        end
    end
    classification_cache = result
    return result
end

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

-- Build the output path for a snapshot.
-- baselines_root: e.g. "/path/to/test/sql-baselines"
-- suite:          e.g. "sql-tap"
-- test_basename:  e.g. "select1"
-- seq:            integer (1-based query index)
-- engine:         "memtx" or "vinyl"
function M.snapshot_path(baselines_root, suite, test_basename, seq, engine)
    return baselines_root .. '/snapshots/' .. suite .. '/' .. test_basename ..
        '/' .. seq_str(seq) .. '.' .. engine .. '.yaml'
end

-- Return current UTC timestamp in RFC3339 format (best-effort).
-- Tarantool's os.time() is Unix epoch; strftime is not available in all
-- environments so we use a fixed-format manual construction.
local function utc_timestamp()
    -- fiber.time64() / os.time() gives local epoch; format as Z (UTC approximation).
    local t = os.time()
    local d = os.date('!*t', t)
    return string.format('%04d-%02d-%02dT%02d:%02d:%02dZ',
        d.year, d.month, d.day, d.hour, d.min, d.sec)
end

-- Return short git commit hash (first 10 chars) or "unknown".
local function git_commit()
    local f = io.popen('git rev-parse --short=10 HEAD 2>/dev/null')
    if not f then return 'unknown' end
    local s = f:read('*l') or 'unknown'
    f:close()
    return s ~= '' and s or 'unknown'
end

-- Return tarantool version string from box.info.version.
local function tarantool_version()
    local ok, v = pcall(function() return box.info.version end)
    return (ok and v) or 'unknown'
end

-- Detect current dispatcher from VDBE_DISPATCHER env var.
local function current_dispatcher()
    return os.getenv('VDBE_DISPATCHER') or 'generated'
end

-- Write one snapshot file.
-- params:
--   baselines_root  string  absolute path to test/sql-baselines
--   suite           string  e.g. "sql-tap"
--   test_basename   string  e.g. "select1"
--   source_file     string  e.g. "sql-tap/select1.test.lua"
--   seq             int     1-based query index
--   engine          string  "memtx" or "vinyl"
--   sql             string  original SQL text (already trimmed)
--   rows            table   raw rows from box.execute (nil on error)
--   err             any     error value (nil on success)
function M.write(params)
    local p = params
    local ordered = canonicalize.has_order_by(p.sql)
    local L1 = canonicalize.canon_L1(p.rows, ordered)
    local ok_flag = (p.err == nil)
    local status = ok_flag and 'success' or 'error'
    local L2 = canonicalize.canon_L2(status, p.err)

    -- Load feature tags
    local cls = load_classification(p.baselines_root)
    local cls_key = p.suite .. '/' .. p.test_basename
    local tags = cls[cls_key] or {}

    -- Build doc strictly following SCHEMA.md v1 key names and structure.
    local doc = {
        schema_version = 1,

        test = {
            suite       = p.suite,
            file        = p.source_file,
            query_index = p.seq,
            query_sql   = p.sql .. '\n',   -- block-scalar in YAML emitter
            feature_tags = tags,
        },

        engine = p.engine,

        captured = {
            at                  = utc_timestamp(),
            against_commit      = git_commit(),
            tarantool_version   = tarantool_version(),
            primary_dispatcher  = current_dispatcher(),
        },

        l1_result = {
            ok           = ok_flag,
            column_names = L1.column_names,
            column_types = L1.column_types,
            rows_sorted  = L1.rows_sorted,
            rows         = L1.rows,
        },

        l2_diagnostic = {
            status                    = status,
            error_code                = L2.error_code,
            error_message_canonical   = L2.error_message_canonical,
        },

        l3_path_class = {
            taken       = 'current_where_c',
            reason      = nil,
            fallback_to = nil,
        },

        metadata = {
            planner_version        = 0,
            classification_version = 1,
        },
    }

    local yaml_str = yaml_emitter.emit(doc)

    -- Ensure output directory exists
    local out_path = M.snapshot_path(
        p.baselines_root, p.suite, p.test_basename, p.seq, p.engine)
    local out_dir = out_path:match('^(.+)/[^/]+$')
    fio.mktree(out_dir)

    local f = io.open(out_path, 'w')
    if not f then
        error('Cannot open for writing: ' .. out_path)
    end
    f:write(yaml_str)
    f:close()

    return out_path
end

return M

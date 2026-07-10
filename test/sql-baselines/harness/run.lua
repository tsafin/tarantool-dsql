#!/usr/bin/env tarantool
-- run.lua — M0.2 snapshot harness entry point.
--
-- Usage:
--   tarantool run.lua <test_file> [--engine=memtx|vinyl] [--out=<baselines_dir>]
--                                 [--forensic] [--suite=<name>]
--
-- Example:
--   cd /path/to/build && rm -f *.snap *.xlog
--   tarantool /path/to/test/sql-baselines/harness/run.lua \
--       /path/to/test/sql-tap/select1.test.lua \
--       --engine=memtx \
--       --out=/path/to/test/sql-baselines
--
-- The harness intercepts SQL execution by monkey-patching box.execute to
-- record every query and its results, then runs the test file in a sandboxed
-- environment. Queries classified as DDL/DML setup (execsql without a named
-- test) are still executed but stored with a special label.
--
-- CRITICAL per CLAUDE.md:
--   - box.cfg{} must be called before any box.execute
--   - rm -f *.snap *.xlog before running
--   - Always use absolute paths
--   - os.exit() at end

-- Resolve harness directory (absolute path)
local _src = debug.getinfo(1, 'S').source
local harness_dir
if _src:sub(1,1) == '@' then
    harness_dir = _src:sub(2):match('^(.+)/[^/]+$')
else
    harness_dir = '.'
end
local baselines_root_default = harness_dir .. '/..'
local lib_dir = harness_dir .. '/../lib'

-- Extend package.path so our modules are findable
package.path = harness_dir .. '/?.lua;' .. lib_dir .. '/?.lua;' .. package.path

local fio = require('fio')
local snapshot_mod = require('snapshot')
local forensic_mod = require('forensic')
local canonicalize = require('canonicalize')

-- ── Argument parsing ──────────────────────────────────────────────────────────

local function parse_args(args)
    local result = {
        test_file = nil,
        engine = 'memtx',
        baselines_root = nil,
        forensic = false,
        suite = nil,
    }
    for _, a in ipairs(args) do
        if a:sub(1,1) ~= '-' then
            result.test_file = a
        elseif a:match('^%-%-engine=(.+)') then
            result.engine = a:match('^%-%-engine=(.+)')
        elseif a:match('^%-%-out=(.+)') then
            result.baselines_root = a:match('^%-%-out=(.+)')
        elseif a:match('^%-%-suite=(.+)') then
            result.suite = a:match('^%-%-suite=(.+)')
        elseif a == '--forensic' then
            result.forensic = true
        end
    end
    return result
end

local cfg = parse_args(arg or {})

if cfg.test_file == nil then
    io.stderr:write('Usage: tarantool run.lua <test_file> [--engine=memtx|vinyl] ' ..
        '[--out=<baselines_dir>] [--forensic] [--suite=<name>]\n')
    os.exit(1)
end

-- Resolve absolute path for test file
if cfg.test_file:sub(1,1) ~= '/' then
    cfg.test_file = fio.cwd() .. '/' .. cfg.test_file
end
if cfg.baselines_root == nil then
    cfg.baselines_root = fio.abspath(baselines_root_default)
end

-- Derive suite and test_basename from the test file path
-- e.g. /path/test/sql-tap/select1.test.lua → suite=sql-tap, basename=select1
local function derive_suite_and_basename(test_file, suite_hint)
    local basename = test_file:match('/([^/]+)%.test%.lua$') or
                     test_file:match('/([^/]+)%.lua$') or
                     test_file:match('/([^/]+)$')
    local suite = suite_hint
    if suite == nil then
        suite = test_file:match('/test/([^/]+)/[^/]+$') or 'sql-tap'
    end
    return suite, basename
end

local suite, test_basename = derive_suite_and_basename(cfg.test_file, cfg.suite)
local source_file = suite .. '/' .. test_basename .. '.test.lua'

io.write(string.format('[harness] test_file=%s engine=%s suite=%s basename=%s\n',
    cfg.test_file, cfg.engine, suite, test_basename))
io.write(string.format('[harness] baselines_root=%s\n', cfg.baselines_root))

-- ── Tarantool initialization ──────────────────────────────────────────────────

-- box.cfg must come before any box.execute
box.cfg {
    log_level = 2,   -- errors only
    memtx_memory = 128 * 1024 * 1024,
}

-- Enable seq_scan so tests that don't have explicit indexes still work
box.execute("SET SESSION \"sql_seq_scan\" = true")

-- ── Query interception ────────────────────────────────────────────────────────
-- We intercept box.execute to capture every SQL statement the test file runs,
-- along with results and errors.

local captured_queries = {}  -- list of {sql, rows, err}
local _real_box_execute = box.execute

local function intercepted_execute(sql, bindings)
    -- Only intercept plain string SQL calls (not prepared statements)
    if type(sql) ~= 'string' then
        return _real_box_execute(sql, bindings)
    end

    -- Capture before-profile for forensic if enabled
    local profile_before = nil
    if cfg.forensic then
        profile_before = forensic_mod.snapshot_before()
    end

    -- box.execute distinguishes 1-arg from 2-arg-with-nil (it rejects nil
    -- bindings), so match the caller's argc rather than always passing 2.
    local ok, res
    if bindings ~= nil then
        ok, res = pcall(_real_box_execute, sql, bindings)
    else
        ok, res = pcall(_real_box_execute, sql)
    end

    local profile_delta = nil
    if cfg.forensic and profile_before then
        profile_delta = forensic_mod.snapshot_after(profile_before)
    end

    local rows = nil
    local err = nil

    if ok then
        if res ~= nil and res.rows ~= nil then
            rows = res.rows
        end
    else
        err = res  -- error object or string
    end

    -- Record every SQL statement (including DDL / DML setup)
    table.insert(captured_queries, {
        sql = sql,
        rows = rows,
        err = err,
        profile_delta = profile_delta,
    })

    -- Re-raise on error so the test file's pcall/catchsql sees it
    if not ok then
        error(res, 0)
    end
    return res
end

-- Patch box.execute globally so the test file's require of sqltester gets it
box.execute = intercepted_execute

-- ── Run the test file ─────────────────────────────────────────────────────────
-- We load and execute the test file. sqltester internally calls box.execute,
-- which is now patched. We suppress os.exit() from sqltester.

-- Suppress os.exit so the harness keeps running after the test completes
local _real_os_exit = os.exit
os.exit = function(code)
    -- store the exit code but don't actually exit yet
    os._harness_exit_code = code
end

-- Suppress tap output (sqltester uses tap module which writes to stdout)
-- We redirect by providing a no-op print — actually keep stdout as-is for now;
-- TAP noise is acceptable since we only care about captured_queries.

-- Add sql-tap lua helpers to path so sqltester and sql_tokenizer resolve
local test_dir = cfg.test_file:match('^(.+)/[^/]+$')
package.path = test_dir .. '/?.lua;' ..
               test_dir .. '/lua/?.lua;' ..
               package.path

-- Execute the test file
local ok_load, load_err = pcall(dofile, cfg.test_file)
if not ok_load then
    io.stderr:write('[harness] Test file execution error (non-fatal, continuing with captured queries): ' ..
        tostring(load_err) .. '\n')
end

-- Restore os.exit
os.exit = _real_os_exit

io.write(string.format('[harness] Captured %d SQL statements\n', #captured_queries))

-- ── Write snapshots ───────────────────────────────────────────────────────────

local written = 0
local skipped = 0
local errors_seen = 0

for seq, q in ipairs(captured_queries) do
    -- Skip empty or whitespace-only SQL
    local sql_trimmed = q.sql:match('^%s*(.-)%s*$')
    if sql_trimmed == '' then
        skipped = skipped + 1
    else
        local ok_w, err_w = pcall(snapshot_mod.write, {
            baselines_root = cfg.baselines_root,
            suite          = suite,
            test_basename  = test_basename,
            source_file    = source_file,
            seq            = seq,
            engine         = cfg.engine,
            sql            = sql_trimmed,
            rows           = q.rows,
            err            = q.err,
        })
        if ok_w then
            written = written + 1
        else
            -- seq_str format: q01..q99, q100+ unpadded (matches SCHEMA.md %02d rule)
            local seq_label = seq < 100 and string.format('q%02d', seq) or ('q'..seq)
            io.stderr:write(string.format('[harness] ERROR writing snapshot %s: %s\n',
                seq_label, tostring(err_w)))
            errors_seen = errors_seen + 1
        end

        -- Write forensic trace if requested
        if cfg.forensic and q.profile_delta ~= nil then
            local ok_f, err_f = pcall(forensic_mod.write, {
                baselines_root = cfg.baselines_root,
                suite          = suite,
                test_basename  = test_basename,
                seq            = seq,
                engine         = cfg.engine,
                profile_delta  = q.profile_delta,
            })
            if not ok_f then
                local seq_label = seq < 100 and string.format('q%02d', seq) or ('q'..seq)
                io.stderr:write(string.format('[harness] WARN forensic %s: %s\n',
                    seq_label, tostring(err_f)))
            end
        end
    end
end

io.write(string.format('[harness] Done. written=%d skipped=%d errors=%d\n',
    written, skipped, errors_seen))

local rc = (errors_seen > 0) and 1 or 0
os.exit(rc)

#!/usr/bin/env tarantool
-- run.lua — M0.2 snapshot harness entry point.
--
-- Usage:
--   tarantool run.lua <test_file> [--engine=memtx|vinyl] [--out=<baselines_dir>]
--                          [--work-dir=<empty_database_dir>]
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
local json = require('json')
local snapshot_mod = require('snapshot')
local forensic_mod = require('forensic')
local canonicalize = require('canonicalize')
local msgpack = require('msgpack')

-- ── Argument parsing ──────────────────────────────────────────────────────────

local function parse_args(args)
    local result = {
        test_file = nil,
        engine = 'memtx',
        baselines_root = nil,
        forensic = false,
        suite = nil,
        work_dir = nil,
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
        elseif a:match('^%-%-work%-dir=(.+)') then
            result.work_dir = a:match('^%-%-work%-dir=(.+)')
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
if cfg.engine ~= 'memtx' and cfg.engine ~= 'vinyl' then
    io.stderr:write('Invalid engine: ' .. tostring(cfg.engine) .. '\n')
    os.exit(2)
end
local dispatcher_requested = os.getenv('VDBE_DISPATCHER') or 'generated'
local llvm_requested = os.getenv('SQL_JIT_ENABLE') == '1'
if (dispatcher_requested ~= 'generated' and dispatcher_requested ~= 'cnp') or
   (dispatcher_requested == 'cnp' and llvm_requested) then
    io.stderr:write('Capture requires generated, cnp, or generated with SQL_JIT_ENABLE=1\n')
    os.exit(2)
end
if cfg.work_dir == nil or cfg.work_dir:sub(1, 1) ~= '/' then
    io.stderr:write('--work-dir must be an absolute, empty directory\n')
    os.exit(2)
end
if not fio.stat(cfg.work_dir) or not fio.stat(cfg.work_dir):is_dir() or
   #fio.listdir(cfg.work_dir) ~= 0 then
    io.stderr:write('--work-dir must exist and be empty: ' .. cfg.work_dir .. '\n')
    os.exit(2)
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
    local basename = test_file:match('/([^/]+)%.test%.sql$') or
                     test_file:match('/([^/]+)%.test%.lua$') or
                     test_file:match('/([^/]+)%.lua$') or
                     test_file:match('/([^/]+)$')
    local suite = suite_hint
    if suite == nil then
        suite = test_file:match('/test/([^/]+)/[^/]+$') or 'sql-tap'
    end
    return suite, basename
end

local suite, test_basename = derive_suite_and_basename(cfg.test_file, cfg.suite)
local source_file = suite .. '/' .. cfg.test_file:match('([^/]+)$')

io.write(string.format('[harness] test_file=%s engine=%s suite=%s basename=%s\n',
    cfg.test_file, cfg.engine, suite, test_basename))
io.write(string.format('[harness] baselines_root=%s\n', cfg.baselines_root))

-- ── Tarantool initialization ──────────────────────────────────────────────────

-- box.cfg must come before any box.execute
box.cfg {
    work_dir = cfg.work_dir,
    log_level = 2,   -- errors only
    memtx_memory = 128 * 1024 * 1024,
    memtx_max_tuple_size = 4996109,
    vinyl_max_tuple_size = 4996109,
    log = 'tarantool.log',
}

-- Enable seq_scan so tests that don't have explicit indexes still work
box.execute("SET SESSION \"sql_seq_scan\" = true")
box.space._session_settings:update('sql_default_engine', {{'=', 2, cfg.engine}})
local stat_before = box.stat.sql()
local execution_mode = llvm_requested and 'llvm' or dispatcher_requested

-- ── Query interception ────────────────────────────────────────────────────────
-- We intercept box.execute to capture every SQL statement the test file runs,
-- along with results and errors.

local captured_queries = {}  -- list of {sql, rows, metadata, err}
local _real_box_execute = box.execute
local engine_mismatch = false
local non_ddl_engine_mismatches = 0

-- A child fiber can have a default SQL engine setting different from the
-- parent while operating on an existing space. Only CREATE TABLE consumes
-- sql_default_engine to choose storage. Be conservative: a false positive
-- rejects a capture, while missing a creation would mislabel its engine.
local function may_create_table(sql)
    local upper = sql:upper():gsub('/%*.-%*/', ' '):gsub('%-%-[^\n]*', ' ')
    return upper:find('CREATE%s+TABLE') ~= nil or
           upper:find('CREATE%s+TEMP%s+TABLE') ~= nil or
           upper:find('CREATE%s+TEMPORARY%s+TABLE') ~= nil
end

local function intercepted_execute(sql, bindings)
    -- Only intercept plain string SQL calls (not prepared statements)
    if type(sql) ~= 'string' then
        return _real_box_execute(sql, bindings)
    end
    local setting = box.space._session_settings:get('sql_default_engine')
    if not setting or setting[2] ~= cfg.engine then
        if may_create_table(sql) then
            engine_mismatch = true
        else
            non_ddl_engine_mismatches = non_ddl_engine_mismatches + 1
        end
    end

    -- Capture the static VDBE program through SQL EXPLAIN when forensic mode
    -- is requested. This is a program listing, not a dynamic dispatch trace;
    -- the distinction is recorded in the L6 file header and schema.
    local explain_rows, explain_error = nil, nil
    local planner_path_class, planner_fallback_reason = nil, nil
    local normalized_sql = sql:gsub('/%*.-%*/', ' '):gsub('%-%-[^\n]*', ' ')
    local first_word = normalized_sql:match('^%s*(%a+)')
    local is_select = first_word ~= nil and
        (first_word:upper() == 'SELECT' or first_word:upper() == 'WITH')
    if is_select then
        local snapshot_ok, snapshot_res
        if bindings ~= nil then
            snapshot_ok, snapshot_res = pcall(_real_box_execute,
                "EXPLAIN (planner = 'snapshot') " .. sql, bindings)
        else
            snapshot_ok, snapshot_res = pcall(_real_box_execute,
                "EXPLAIN (planner = 'snapshot') " .. sql)
        end
        if snapshot_ok and snapshot_res ~= nil and
           snapshot_res.rows ~= nil and snapshot_res.rows[1] ~= nil then
            local decode_ok, planner_snapshot = pcall(msgpack.decode,
                tostring(snapshot_res.rows[1][1]))
            if decode_ok and type(planner_snapshot) == 'table' then
                planner_path_class = planner_snapshot.path_class
                planner_fallback_reason = planner_snapshot.fallback_reason
            end
        end
    end
    if cfg.forensic then
        if is_select then
            local explain_ok, explain_res, explain_err
            if bindings ~= nil then
                explain_ok, explain_res, explain_err =
                    pcall(_real_box_execute, 'EXPLAIN ' .. sql, bindings)
            else
                explain_ok, explain_res, explain_err =
                    pcall(_real_box_execute, 'EXPLAIN ' .. sql)
            end
            if explain_ok and explain_err == nil and explain_res ~= nil and
               explain_res.rows ~= nil then
                explain_rows = explain_res.rows
            else
                explain_error = explain_ok and tostring(explain_err) or
                                tostring(explain_res)
            end
        else
            explain_error = 'statement is already EXPLAIN or has no SQL keyword'
        end
    end

    -- Capture before-profile for forensic if enabled
    local profile_before = nil
    if cfg.forensic then
        profile_before = forensic_mod.snapshot_before()
    end

    -- box.execute distinguishes 1-arg from 2-arg-with-nil (it rejects nil
    -- bindings), so match the caller's argc rather than always passing 2.
    local query_stat_before = box.stat.sql()
    local ok, res, returned_err
    if bindings ~= nil then
        ok, res, returned_err = pcall(_real_box_execute, sql, bindings)
    else
        ok, res, returned_err = pcall(_real_box_execute, sql)
    end
    local query_stat_after = box.stat.sql()
    local interpreter_delta = tonumber(query_stat_after.sql_interpreter_step_count or 0) -
                              tonumber(query_stat_before.sql_interpreter_step_count or 0)
    local native_prefix = execution_mode == 'llvm' and 'sql_jit_' or 'sql_cnp_'
    local native_counter = native_prefix .. 'exec_count'
    local native_delta = tonumber(query_stat_after[native_counter] or 0) -
                         tonumber(query_stat_before[native_counter] or 0)
    local compile_counter = native_prefix .. 'compile_count'
    local compile_delta = tonumber(query_stat_after[compile_counter] or 0) -
                          tonumber(query_stat_before[compile_counter] or 0)
    local success_counter = native_prefix .. 'compile_success_count'
    local success_delta = tonumber(query_stat_after[success_counter] or 0) -
                          tonumber(query_stat_before[success_counter] or 0)

    local profile_delta = nil
    if cfg.forensic and profile_before then
        profile_delta = forensic_mod.snapshot_after(profile_before)
    end

    local rows = nil
    local metadata = nil
    local err = nil

    if ok and returned_err == nil then
        if res ~= nil and res.rows ~= nil then
            rows = res.rows
        end
        if res ~= nil then
            metadata = res.metadata
        end
    else
        err = ok and returned_err or res
    end

    -- Record every SQL statement (including DDL / DML setup)
    table.insert(captured_queries, {
        sql = sql,
        rows = rows,
        metadata = metadata,
        err = err,
        profile_delta = profile_delta,
        interpreter_delta = interpreter_delta,
        native_delta = native_delta,
        compile_delta = compile_delta,
        success_delta = success_delta,
        explain_rows = explain_rows,
        explain_error = explain_error,
        planner_path_class = planner_path_class,
        planner_fallback_reason = planner_fallback_reason,
    })

    -- Re-raise on error so the test file's pcall/catchsql sees it
    if not ok then
        error(res, 0)
    end
    return res, returned_err
end

-- Patch box.execute globally so the test file's require of sqltester gets it
box.execute = intercepted_execute

-- ── Run the test file ─────────────────────────────────────────────────────────
-- We load and execute the test file. sqltester internally calls box.execute,
-- which is now patched. Turn os.exit() into a private signal so that no code
-- after finish_test() can run, while retaining its exit status.

local _real_os_exit = os.exit
local test_exit_code = nil
local test_finished = false
local test_exit_signal = {}
local tap_check_count = 0
local tap_check_failed = false
local last_tap_check_query_count = nil
os.exit = function(code)
    code = code or 0
    test_exit_code = code
    local caller = debug.getinfo(2, 'S')
    local source = caller and caller.source or ''
    local sqltester_finish = type(source) == 'string' and
                             source:match('/sqltester%.lua$') ~= nil
    local checked_direct_finish = source == '@' .. cfg.test_file and
                                  tap_check_count > 0 and not tap_check_failed and
                                  last_tap_check_query_count == #captured_queries
    test_finished = sqltester_finish or checked_direct_finish
    error(test_exit_signal, 0)
end

-- A later box.cfg() failure must retain the normal test behavior. Count it
-- even when the test catches the exception, and let the exception propagate.
local _real_box_cfg = box.cfg
local cfg_errors = 0
box.cfg = setmetatable({}, {
    __call = function(_, opts)
        local ok, err = pcall(_real_box_cfg, opts)
        if not ok then
            cfg_errors = cfg_errors + 1
            error(err, 0)
        end
    end,
    __index = _real_box_cfg,
    __newindex = _real_box_cfg,
})

-- SQL TAP has two valid exit patterns: sqltester.finish_test() and a direct
-- os.exit(test:check() and 0 or 1). Observe the latter's actual TAP check so
-- a bare early os.exit(0) cannot masquerade as a completed test.
local tap = require('tap')
local _real_tap_test = tap.test
tap.test = function(...)
    local test = _real_tap_test(...)
    local _real_check = test.check
    test.check = function(self, ...)
        local checked = _real_check(self, ...)
        tap_check_count = tap_check_count + 1
        tap_check_failed = tap_check_failed or checked ~= true
        last_tap_check_query_count = #captured_queries
        return checked
    end
    return test
end

-- Suppress tap output (sqltester uses tap module which writes to stdout)
-- We redirect by providing a no-op print — actually keep stdout as-is for now;
-- TAP noise is acceptable since we only care about captured_queries.

-- Extend package.path so sqltester, sql_tokenizer, and test_run resolve.
-- test-run.py normally copies these into a temp dir; here we point at their
-- source locations. sql_tokenizer lives under test/sql/lua/, not test/sql-tap/lua/,
-- so both need to be on the path when running any sql-tap test. Derive the
-- repo root from the test file path (test/<suite>/<file>.test.lua → repo).
local test_dir = cfg.test_file:match('^(.+)/[^/]+$')
local repo_root = cfg.test_file:match('^(.+)/test/[^/]+/[^/]+%.lua$')
package.path = test_dir .. '/?.lua;' ..
               test_dir .. '/lua/?.lua;' ..
               (repo_root and (repo_root .. '/test/sql/lua/?.lua;') or '') ..
               (repo_root and (repo_root .. '/test/sql-tap/lua/?.lua;') or '') ..
               (repo_root and (repo_root .. '/test/box/lua/?.lua;') or '') ..
               (repo_root and (repo_root .. '/test/luatest/?.lua;') or '') ..
               package.path
-- sqltester obtains its engine from test_run when present. Provide only the
-- configuration interface needed for standalone capture; tests needing the
-- full inspector will fail and be excluded until a test-run adapter exists.
if suite == 'sql-tap' then
    package.preload.test_run = function()
        return {new = function()
            return {get_cfg = function(_, key)
                if key == 'engine' then return cfg.engine end
                return nil
            end}
        end}
    end
end
-- Do NOT add test-run/ to LUA_PATH: sqltester wraps require('test_run') in
-- pcall and treats the module as optional. If the require succeeds,
-- sqltester calls test_run.new() unprotected, which fails standalone with
-- "Inspector not started". Absence of test_run on the path is the sqltester
-- convention for "not running under test-run.py."

-- Execute the test file
local function execute_test()
    if cfg.test_file:match('%.test%.sql$') then
        require('sql_file').run(cfg.test_file)
        test_exit_code = 0
        test_finished = true
    else
        dofile(cfg.test_file)
    end
end
local ok_load, load_err = pcall(execute_test)
local exited = not ok_load and load_err == test_exit_signal
if not ok_load and not exited then
    io.stderr:write('[harness] Test file execution error: ' ..
        tostring(load_err) .. '\n')
end
if exited then ok_load = true end
local engine_tuple = box.space._session_settings:get('sql_default_engine')
local runtime_engine = engine_tuple and engine_tuple[2] or 'unknown'
local stat_after = box.stat.sql()
local cnp_exec_delta = tonumber(stat_after.sql_cnp_exec_count or 0) -
                       tonumber(stat_before.sql_cnp_exec_count or 0)
local llvm_exec_delta = tonumber(stat_after.sql_jit_exec_count or 0) -
                        tonumber(stat_before.sql_jit_exec_count or 0)
local mode_executed = (execution_mode == 'generated' and
                       cnp_exec_delta == 0 and llvm_exec_delta == 0) or
                      (execution_mode == 'cnp' and cnp_exec_delta > 0) or
                      (execution_mode == 'llvm' and llvm_exec_delta > 0)
-- Interpreter-only execution is not sufficient to claim native eligibility:
-- LLVM intentionally declines tiny VDBE programs and unsupported shapes.
-- A successful native compilation establishes eligibility; a positive native
-- execution delta also covers statements served from the positive cache.
local executed_query_indices = {}
local native_compile_attempt_query_indices = {}
local native_compile_success_query_indices = {}
local native_participation_query_indices = {}
local eligible_query_indices = {}
local mode_miss_queries = {}
for index, query in ipairs(captured_queries) do
    if query.interpreter_delta > 0 or query.native_delta > 0 then
        table.insert(executed_query_indices, index)
    end
    if execution_mode ~= 'generated' then
        if query.compile_delta > 0 then
            table.insert(native_compile_attempt_query_indices, index)
        end
        if query.success_delta > 0 then
            table.insert(native_compile_success_query_indices, index)
        end
        if query.native_delta > 0 then
            table.insert(native_participation_query_indices, index)
        end
        -- EXPLAIN may compile successfully without executing the compiled
        -- program; only an actually executed query can miss native entry.
        if (query.interpreter_delta > 0 or query.native_delta > 0) and
           (query.success_delta > 0 or query.native_delta > 0) then
            table.insert(eligible_query_indices, index)
            if query.native_delta == 0 then
                table.insert(mode_miss_queries, index)
            end
        end
    end
end
if runtime_engine ~= cfg.engine then
    io.stderr:write('[harness] Runtime engine changed to ' .. tostring(runtime_engine) .. '\n')
end

-- Restore os.exit
os.exit = _real_os_exit
tap.test = _real_tap_test

io.write(string.format('[harness] Captured %d SQL statements\n', #captured_queries))

-- ── Write snapshots ───────────────────────────────────────────────────────────

local written = 0
local skipped = 0
local errors_seen = 0
local test_ok = ok_load and cfg_errors == 0 and test_exit_code == 0 and
                test_finished and
                #captured_queries > 0 and runtime_engine == cfg.engine and
                not engine_mismatch and mode_executed and
                #mode_miss_queries == 0

for seq, q in ipairs(test_ok and captured_queries or {}) do
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
            metadata       = q.metadata,
            err            = q.err,
            path_class     = q.planner_path_class,
            fallback_reason = q.planner_fallback_reason,
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
        if cfg.forensic then
            local ok_f, err_f = pcall(forensic_mod.write, {
                baselines_root = cfg.baselines_root,
                suite          = suite,
                test_basename  = test_basename,
                seq            = seq,
                engine         = cfg.engine,
                sql            = q.sql,
                program        = q.explain_rows,
                capture_error  = q.explain_error,
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

local rc = (test_ok and errors_seen == 0 and skipped == 0 and
            written == #captured_queries) and 0 or 1
local manifest_path = string.format('%s/manifests/%s/%s.%s.json',
    cfg.baselines_root, suite, test_basename, cfg.engine)
fio.mktree(manifest_path:match('^(.+)/[^/]+$'))
local manifest = {
    manifest_version = 1,
    suite = suite,
    test_file = source_file,
    engine = cfg.engine,
    dispatcher_requested = dispatcher_requested,
    sql_jit_enable = llvm_requested,
    execution_mode = execution_mode,
    mode_executed = mode_executed,
    cnp_exec_delta = cnp_exec_delta,
    llvm_exec_delta = llvm_exec_delta,
    executed_query_indices = executed_query_indices,
    native_compile_attempt_query_indices = native_compile_attempt_query_indices,
    native_compile_success_query_indices = native_compile_success_query_indices,
    native_participation_query_indices = native_participation_query_indices,
    eligible_queries = #eligible_query_indices,
    eligible_query_indices = eligible_query_indices,
    native_participation_queries = #native_participation_query_indices,
    mode_miss_queries = mode_miss_queries,
    runtime_engine = runtime_engine,
    engine_mismatch = engine_mismatch,
    non_ddl_engine_mismatches = non_ddl_engine_mismatches,
    test_exit_code = test_exit_code or 'missing',
    test_load_ok = ok_load,
    test_load_error = ok_load and '' or tostring(load_err),
    cfg_errors = cfg_errors,
    captured_queries = #captured_queries,
    written_snapshots = written,
    skipped_queries = skipped,
    snapshot_errors = errors_seen,
    accepted = rc == 0,
}
local mf, mf_err = io.open(manifest_path, 'w')
if not mf then
    io.stderr:write('[harness] Cannot write manifest: ' .. tostring(mf_err) .. '\n')
    os.exit(1)
end
mf:write(json.encode(manifest), '\n')
mf:close()
io.write('[harness] manifest=' .. manifest_path .. ' accepted=' .. tostring(rc == 0) .. '\n')
os.exit(rc)

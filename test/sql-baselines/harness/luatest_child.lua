-- Install this before box.cfg via TARANTOOL_RUN_BEFORE_BOX_CFG in luatest's
-- default server_instance.lua. One child server is accepted per capture.
local fio = require('fio')
local json = require('json')

local out = assert(os.getenv('SQL_BASELINE_OUT'), 'missing SQL_BASELINE_OUT')
local file = assert(os.getenv('SQL_BASELINE_TEST'), 'missing SQL_BASELINE_TEST')
local engine = assert(os.getenv('SQL_BASELINE_ENGINE'), 'missing SQL_BASELINE_ENGINE')
local mode = assert(os.getenv('SQL_BASELINE_MODE'), 'missing SQL_BASELINE_MODE')
local planner_flag = os.getenv('SQL_BASELINE_PLANNER_FLAG')
local suite, filename = file:match('^([^/]+)/([^/]+)$')
assert(out:sub(1, 1) == '/' and
       ((suite == 'sql-luatest' and filename:match('^[%w_.-]+_test%.lua$')) or
        (suite == 'sql' and filename:match('^[%w_.-]+%.test%.lua$'))))
assert(engine == 'memtx' or engine == 'vinyl')
assert(mode == 'generated' or mode == 'cnp' or mode == 'llvm')
assert(planner_flag == nil or planner_flag == 'off' or planner_flag == 'on')
local basename = file:match('/([^/]+)%.test%.lua$') or
                 file:match('/([^/]+)%.lua$')

-- A second child or a restart would otherwise overwrite qNN files. Refuse
-- the run until a per-server identity is part of the schema contract.
local owner = out .. '/luatest-child-owner'
if not fio.mkdir(owner) then
    -- test-run starts the SQL app once for suite setup, then restarts it for
    -- the selected test. No query ran in the setup instance, so reusing the
    -- empty capture is safe. A second child after SQL was captured is not.
    assert(suite == 'sql' and
           not fio.stat(out .. '/luatest-child-state.json'),
           'capture requires exactly one SQL execution server')
end

local harness_dir = debug.getinfo(1, 'S').source:sub(2):match('^(.+)/[^/]+$')
package.path = harness_dir .. '/?.lua;' .. harness_dir .. '/../lib/?.lua;' ..
               package.path
local snapshot = require('snapshot')
local msgpack = require('msgpack')
local sql_statement = require('sql_statement')
local original_cfg = box.cfg
local installed = false

box.cfg = setmetatable({}, {
    __index = original_cfg,
    __newindex = original_cfg,
    __call = function(_, opts)
        local result = original_cfg(opts)
        assert(not installed, 'box.cfg called twice in luatest capture')
        installed = true
        box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})
        local before = box.stat.sql()
        local original_execute = box.execute
        local count = 0
        local engine_mismatch = false
        local planner_flag_mismatch = false
        local observed_engines = {}
        local initialized_sessions = {}
        local initialized_planner_sessions = {}
        local executed_query_indices = {}
        local native_compile_attempt_query_indices = {}
        local native_compile_success_query_indices = {}
        local native_participation_query_indices = {}
        local eligible_query_indices = {}
        local mode_miss_queries = {}
        local planner_metrics = {}

        box.execute = function(sql, bindings)
            if type(sql) ~= 'string' then
                if bindings ~= nil then
                    return original_execute(sql, bindings)
                end
                return original_execute(sql)
            end
            -- The test body runs in a net.box session, not the startup fiber.
            -- Set its default once; later test-initiated engine changes remain
            -- visible and reject the capture.
            local session = box.session.id()
            if not initialized_sessions[session] then
                box.space._session_settings:update('sql_default_engine',
                                                   {{'=', 2, engine}})
                initialized_sessions[session] = true
            end
            if planner_flag ~= nil and not initialized_planner_sessions[session] then
                box.space._session_settings:update(
                    'sql_new_planner_single_table',
                    {{'=', 2, planner_flag == 'on'}})
                initialized_planner_sessions[session] = true
            end
            local setting = box.space._session_settings:get('sql_default_engine')
            observed_engines[#observed_engines + 1] = setting and setting[2] or 'missing'
            engine_mismatch = engine_mismatch or not setting or setting[2] ~= engine
            if planner_flag ~= nil then
                local planner_setting = box.space._session_settings:get(
                    'sql_new_planner_single_table')
                planner_flag_mismatch = planner_flag_mismatch or
                    planner_setting == nil or
                    planner_setting[2] ~= (planner_flag == 'on')
            end
            local query_before = box.stat.sql()
            local ok, res, returned_err
            if bindings ~= nil then
                ok, res, returned_err = pcall(original_execute, sql, bindings)
            else
                ok, res, returned_err = pcall(original_execute, sql)
            end
            local err = nil
            if not ok then
                err = res
            elseif returned_err ~= nil then
                err = returned_err
            end
            count = count + 1
            local trimmed = sql:match('^%s*(.-)%s*$')
            assert(trimmed ~= '', 'empty SQL query in luatest capture')
            -- Take dispatcher counters before asking EXPLAIN for its planner
            -- snapshot, so observability work is not attributed to the test's
            -- execution.
            local after = box.stat.sql()
            local planner_snapshot = nil
            if err == nil and sql_statement.has_select_plan(trimmed) then
                local explain_ok, explain_res, explain_err
                if bindings ~= nil then
                    explain_ok, explain_res, explain_err =
                        pcall(original_execute,
                              "EXPLAIN (planner = 'snapshot') " .. trimmed,
                              bindings)
                else
                    explain_ok, explain_res, explain_err =
                        pcall(original_execute,
                              "EXPLAIN (planner = 'snapshot') " .. trimmed)
                end
                assert(explain_ok and explain_err == nil and explain_res ~= nil and
                       explain_res.rows ~= nil and explain_res.rows[1] ~= nil and
                       explain_res.rows[1][1] ~= nil,
                       'planner snapshot EXPLAIN failed: ' .. tostring(explain_err))
                local decode_ok
                decode_ok, planner_snapshot = pcall(msgpack.decode,
                    tostring(explain_res.rows[1][1]))
                assert(decode_ok and type(planner_snapshot) == 'table' and
                       planner_snapshot.format ==
                           'tarantool.sql.planner.snapshot' and
                       planner_snapshot.version == 5 and
                       type(planner_snapshot.planner) == 'table',
                       'planner snapshot EXPLAIN returned invalid metadata')
                local routes = planner_snapshot.planner.component_routes
                assert(planner_snapshot.planner.component_status == 'complete' and
                       type(routes) == 'table' and #routes > 0,
                       ('planner snapshot EXPLAIN returned incomplete v5 ledger ' ..
                        '(status=%s, routes=%s, path=%s)'):format(
                           tostring(planner_snapshot.planner.component_status),
                           tostring(type(routes) == 'table' and #routes or routes),
                           tostring(planner_snapshot.path_class)))
                local metrics = planner_snapshot.planner
                planner_metrics[#planner_metrics + 1] = {
                    query_index = count,
                    path_class = planner_snapshot.path_class,
                    fallback_reason = planner_snapshot.fallback_reason,
                    component_status = metrics.component_status,
                    component_routes = metrics.component_routes,
                    candidate_count = metrics.candidate_count,
                    elapsed_us = metrics.elapsed_us,
                    fallback_count = metrics.fallback_count,
                    generated = metrics.generated,
                    dominated = metrics.dominated,
                    truncated = metrics.truncated,
                    retained = metrics.retained,
                }
            end
            local write_ok, write_err = pcall(snapshot.write, {
                baselines_root = out,
                suite = suite,
                test_basename = basename,
                source_file = file,
                seq = count,
                engine = engine,
                sql = trimmed,
                rows = ok and res and res.rows or nil,
                metadata = ok and res and res.metadata or nil,
                err = err,
                path_class = planner_snapshot and planner_snapshot.path_class,
                fallback_reason = planner_snapshot and
                                  planner_snapshot.fallback_reason,
            })
            if not write_ok then
                local error_file = assert(io.open(out .. '/luatest-capture-error', 'w'))
                error_file:write(tostring(write_err), '\n')
                error_file:close()
                error(write_err, 0)
            end
            local query_cnp = tonumber(after.sql_cnp_exec_count or 0) -
                              tonumber(query_before.sql_cnp_exec_count or 0)
            local query_llvm = tonumber(after.sql_jit_exec_count or 0) -
                               tonumber(query_before.sql_jit_exec_count or 0)
            local query_interpreter =
                tonumber(after.sql_interpreter_step_count or 0) -
                tonumber(query_before.sql_interpreter_step_count or 0)
            local selected_native = mode == 'cnp' and query_cnp or
                                    (mode == 'llvm' and query_llvm or 0)
            local compile_key = mode == 'cnp' and 'sql_cnp_compile_count' or
                                'sql_jit_compile_count'
            local success_key = mode == 'cnp' and
                                'sql_cnp_compile_success_count' or
                                'sql_jit_compile_success_count'
            local query_compile = mode == 'generated' and 0 or
                tonumber(after[compile_key] or 0) -
                tonumber(query_before[compile_key] or 0)
            local query_success = mode == 'generated' and 0 or
                tonumber(after[success_key] or 0) -
                tonumber(query_before[success_key] or 0)
            if selected_native > 0 or query_interpreter > 0 then
                executed_query_indices[#executed_query_indices + 1] = count
            end
            if query_compile > 0 then
                native_compile_attempt_query_indices
                    [#native_compile_attempt_query_indices + 1] = count
            end
            if query_success > 0 then
                native_compile_success_query_indices
                    [#native_compile_success_query_indices + 1] = count
            end
            if selected_native > 0 then
                native_participation_query_indices
                    [#native_participation_query_indices + 1] = count
            end
            if (query_interpreter > 0 or selected_native > 0) and
               (query_success > 0 or selected_native > 0) then
                eligible_query_indices[#eligible_query_indices + 1] = count
            end
            if mode ~= 'generated' and query_interpreter > 0 and
               query_success > 0 and
               selected_native == 0 then
                mode_miss_queries[#mode_miss_queries + 1] = count
            end
            local cnp_delta = tonumber(after.sql_cnp_exec_count or 0) -
                              tonumber(before.sql_cnp_exec_count or 0)
            local llvm_delta = tonumber(after.sql_jit_exec_count or 0) -
                               tonumber(before.sql_jit_exec_count or 0)
            local state = {
                test_file = file,
                engine = engine,
                execution_mode = mode,
                captured_queries = count,
                cnp_exec_delta = cnp_delta,
                llvm_exec_delta = llvm_delta,
                engine_mismatch = engine_mismatch,
                planner_flag = planner_flag,
                planner_flag_mismatch = planner_flag_mismatch,
                observed_engines = observed_engines,
                executed_query_indices = executed_query_indices,
                native_compile_attempt_query_indices =
                    native_compile_attempt_query_indices,
                native_compile_success_query_indices =
                    native_compile_success_query_indices,
                eligible_queries = #eligible_query_indices,
                eligible_query_indices = eligible_query_indices,
                native_participation_queries =
                    #native_participation_query_indices,
                native_participation_query_indices =
                    native_participation_query_indices,
                mode_miss_queries = mode_miss_queries,
                planner_metrics = planner_metrics,
            }
            local state_file = assert(io.open(out .. '/luatest-child-state.json', 'w'))
            state_file:write(json.encode(state), '\n')
            state_file:close()
            if not ok then error(res, 0) end
            if returned_err ~= nil then
                return res, returned_err
            end
            return res
        end
        return result
    end,
})

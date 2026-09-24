-- Install this before box.cfg via TARANTOOL_RUN_BEFORE_BOX_CFG in luatest's
-- default server_instance.lua. One child server is accepted per capture.
local fio = require('fio')
local json = require('json')

local out = assert(os.getenv('SQL_BASELINE_OUT'), 'missing SQL_BASELINE_OUT')
local file = assert(os.getenv('SQL_BASELINE_TEST'), 'missing SQL_BASELINE_TEST')
local engine = assert(os.getenv('SQL_BASELINE_ENGINE'), 'missing SQL_BASELINE_ENGINE')
local mode = assert(os.getenv('SQL_BASELINE_MODE'), 'missing SQL_BASELINE_MODE')
local suite, filename = file:match('^([^/]+)/([^/]+)$')
assert(out:sub(1, 1) == '/' and
       ((suite == 'sql-luatest' and filename:match('^[%w_.-]+_test%.lua$')) or
        (suite == 'sql' and filename:match('^[%w_.-]+%.test%.lua$'))))
assert(engine == 'memtx' or engine == 'vinyl')
assert(mode == 'generated' or mode == 'cnp' or mode == 'llvm')
local basename = file:match('/([^/]+)%.lua$')

-- A second child or a restart would otherwise overwrite qNN files. Refuse
-- the run until a per-server identity is part of the schema contract.
local owner = out .. '/luatest-child-owner'
assert(fio.mkdir(owner), 'capture requires exactly one luatest child server')

local harness_dir = debug.getinfo(1, 'S').source:sub(2):match('^(.+)/[^/]+$')
package.path = harness_dir .. '/?.lua;' .. package.path
local snapshot = require('snapshot')
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
        local observed_engines = {}
        local initialized_sessions = {}

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
            local setting = box.space._session_settings:get('sql_default_engine')
            observed_engines[#observed_engines + 1] = setting and setting[2] or 'missing'
            engine_mismatch = engine_mismatch or not setting or setting[2] ~= engine
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
            })
            if not write_ok then
                local error_file = assert(io.open(out .. '/luatest-capture-error', 'w'))
                error_file:write(tostring(write_err), '\n')
                error_file:close()
                error(write_err, 0)
            end
            local after = box.stat.sql()
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
                observed_engines = observed_engines,
            }
            local state_file = assert(io.open(out .. '/luatest-child-state.json', 'w'))
            state_file:write(json.encode(state), '\n')
            state_file:close()
            if not ok then error(res, 0) end
            return res, returned_err
        end
        return result
    end,
})

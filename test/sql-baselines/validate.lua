#!/usr/bin/env tarantool
-- Validate one isolated capture tree before it can be used as a baseline.
-- Usage: tarantool test/sql-baselines/validate.lua <capture-root>

local fio = require('fio')
local json = require('json')
local yaml = require('yaml')
local varbinary = require('varbinary')

local root = arg[1]
if not root or not fio.stat(root) or not fio.stat(root):is_dir() then
    io.stderr:write('Usage: tarantool validate.lua <capture-root>\n')
    os.exit(2)
end
root = fio.abspath(root)

local function walk(path, suffix, result)
    local st = fio.stat(path)
    if not st then return end
    if not st:is_dir() then
        if path:sub(-#suffix) == suffix then result[#result + 1] = path end
        return
    end
    for _, name in ipairs(fio.listdir(path) or {}) do
        walk(path .. '/' .. name, suffix, result)
    end
end

local function read_document(path, decoder)
    local file, err = io.open(path, 'r')
    if not file then return nil, tostring(err) end
    local data = file:read('*a')
    file:close()
    local ok, value = pcall(decoder, data)
    if not ok or type(value) ~= 'table' then
        return nil, ok and 'document is not a map' or tostring(value)
    end
    return value
end

local errors = {}
local function reject(message)
    errors[#errors + 1] = message
end

local manifests = {}
walk(root .. '/manifests', '.json', manifests)
table.sort(manifests)
if #manifests == 0 then reject('no manifests found') end

local expected = {}
local snapshots_checked = 0
local function valid_indices(list, count)
    if type(list) ~= 'table' then return false end
    local previous = 0
    for _, index in ipairs(list) do
        if type(index) ~= 'number' or index % 1 ~= 0 or
           index <= previous or index > count then return false end
        previous = index
    end
    return true
end

local function valid_mode_proof(m)
    local count = m.captured_queries
    local fields = {'executed_query_indices',
                    'native_compile_attempt_query_indices',
                    'native_compile_success_query_indices',
                    'native_participation_query_indices',
                    'eligible_query_indices', 'mode_miss_queries'}
    for _, field in ipairs(fields) do
        if not valid_indices(m[field], count) then return false end
    end
    if m.eligible_queries ~= #m.eligible_query_indices or
       m.native_participation_queries ~= #m.native_participation_query_indices or
       #m.mode_miss_queries ~= 0 then return false end
    local executed, attempt, success, participation, eligible = {}, {}, {}, {}, {}
    for _, i in ipairs(m.executed_query_indices) do executed[i] = true end
    for _, i in ipairs(m.native_compile_attempt_query_indices) do attempt[i] = true end
    for _, i in ipairs(m.native_compile_success_query_indices) do
        if not attempt[i] then return false end
        success[i] = true
    end
    for _, i in ipairs(m.native_participation_query_indices) do
        if not executed[i] then return false end
        participation[i] = true
    end
    for _, i in ipairs(m.eligible_query_indices) do eligible[i] = true end
    for i = 1, count do
        local expected_eligible = m.execution_mode ~= 'generated' and
                                  executed[i] and (success[i] or participation[i])
        if not not eligible[i] ~= not not expected_eligible then return false end
        if eligible[i] and not participation[i] then return false end
    end
    if m.execution_mode == 'generated' and
       (#m.native_compile_attempt_query_indices ~= 0 or
        #m.native_compile_success_query_indices ~= 0 or
        #m.native_participation_query_indices ~= 0 or
        #m.eligible_query_indices ~= 0) then return false end
    if m.execution_mode ~= 'generated' and
       #m.native_participation_query_indices == 0 then return false end
    return true
end

-- Keep this list aligned with the append-only producer reason enum and the
-- snapshot reason codes already reserved by SCHEMA.md for later M3 routes.
local stable_fallback_reasons = {
    UNRESOLVED_INPUT = true,
    UNSUPPORTED_RELATION_COUNT = true,
    UNSUPPORTED_SUBQUERY = true,
    UNSUPPORTED_AGGREGATE = true,
    UNSUPPORTED_COMPOUND = true,
    UNSUPPORTED_CTE = true,
    UNSUPPORTED_DISTINCT = true,
    INVALID_LOGICAL_PLAN = true,
    NO_ACCESS_PATH = true,
    INVALID_CANDIDATE = true,
    UNSUPPORTED_JOIN = true,
    UNSUPPORTED_DML = true,
    UNSUPPORTED_TRIGGER = true,
    UNSUPPORTED_NONDETERMINISTIC = true,
    BUDGET_EXCEEDED = true,
    LOW_CONFIDENCE_STATS = true,
    LOWERING_FAILED = true,
}

local function is_null(value)
    return value == nil or value == box.NULL
end

local function valid_path_class(path)
    if type(path) ~= 'table' then return false end
    local reason = path.reason
    local fallback_to = path.fallback_to
    if is_null(path.taken) then
        return is_null(reason) and is_null(fallback_to)
    end
    if type(path.taken) ~= 'string' then return false end
    if path.taken == 'current_where_c' or path.taken == 'new_planner' then
        return reason == nil and fallback_to == nil
    end
    if path.taken == 'fallback' then
        return type(reason) == 'string' and
               stable_fallback_reasons[reason] == true and
               fallback_to == 'current_where_c'
    end
    local suffix = path.taken:match('^fallback_(.+)$')
    return suffix ~= nil and stable_fallback_reasons[suffix] == true and
           reason == suffix and
           fallback_to == 'current_where_c'
end

local function valid_planner_metrics(manifest)
	local has_version = manifest.planner_metrics_version ~= nil
	local has_metrics = manifest.planner_metrics ~= nil
	if not has_version and not has_metrics then return {} end
	if manifest.planner_metrics_version ~= 2 or
	   type(manifest.planner_metrics) ~= 'table' or
	   type(manifest.captured_queries) ~= 'number' then
		return nil
	end
	local by_index = {}
	local previous = 0
	for _, metric in ipairs(manifest.planner_metrics) do
		local taken = nil
		if type(metric) == 'table' then taken = metric.path_class end
		local fallback_to = nil
		if taken == 'fallback' or
		   type(taken) == 'string' and taken:match('^fallback_') then
			fallback_to = 'current_where_c'
		elseif is_null(taken) then
			fallback_to = box.NULL
		end
		if type(metric) ~= 'table' or
		   type(metric.query_index) ~= 'number' or
		   metric.query_index % 1 ~= 0 or metric.query_index <= previous or
		   metric.query_index > manifest.captured_queries or
		   not valid_path_class({
			taken = metric.path_class,
			reason = metric.fallback_reason,
			fallback_to = fallback_to,
		   }) then
			return nil
		end
		previous = metric.query_index
		by_index[metric.query_index] = metric
	end
	return by_index
end

local function same_nullable(a, b)
	return is_null(a) and is_null(b) or a == b
end

for _, path in ipairs(manifests) do
    local m, err = read_document(path, json.decode)
    local planner_metrics = m and valid_planner_metrics(m) or nil
    if not m then
        reject(path .. ': ' .. err)
    elseif m.manifest_version ~= 1 or m.accepted ~= true or
           m.test_load_ok ~= true or m.test_exit_code ~= 0 or
           m.cfg_errors ~= 0 or m.snapshot_errors ~= 0 or
           m.skipped_queries ~= 0 or
           m.engine_mismatch ~= false or
           m.mode_executed ~= true or
           m.runtime_engine ~= m.engine or
           type(m.captured_queries) ~= 'number' or
           m.captured_queries < 1 or
           m.written_snapshots ~= m.captured_queries or
           planner_metrics == nil or
           not valid_mode_proof(m) then
        reject(path .. ': rejected or inconsistent run outcome')
    elseif (m.engine ~= 'memtx' and m.engine ~= 'vinyl') or
           (m.suite ~= 'sql' and m.suite ~= 'sql-tap' and
            m.suite ~= 'sql-luatest') or
           type(m.test_file) ~= 'string' or
           type(m.dispatcher_requested) ~= 'string' or
           (m.execution_mode ~= 'generated' and m.execution_mode ~= 'cnp' and
            m.execution_mode ~= 'llvm') then
        reject(path .. ': invalid identity')
    else
        local basename = m.test_file:match('/([^/]+)%.test%.lua$') or
                         m.test_file:match('/([^/]+)%.test%.sql$') or
                         m.test_file:match('/([^/]+)%.lua$')
        if not basename or m.test_file:sub(1, #m.suite + 1) ~= m.suite .. '/' then
            reject(path .. ': test path and suite disagree')
        else
            for index = 1, m.captured_queries do
                local filename = string.format('q%02d.%s.yaml', index, m.engine)
                local snapshot_path = root .. '/snapshots/' .. m.suite .. '/' ..
                                      basename .. '/' .. filename
                if expected[snapshot_path] then
                    reject(snapshot_path .. ': duplicate manifest identity')
                end
                expected[snapshot_path] = true
                local s, snapshot_err = read_document(snapshot_path, yaml.decode)
                local metric = planner_metrics[index]
                if not s then
                    reject(snapshot_path .. ': ' .. snapshot_err)
                elseif s.schema_version ~= 1 or s.engine ~= m.engine or
                       type(s.test) ~= 'table' or
                       s.test.suite ~= m.suite or
                       s.test.file ~= m.test_file or
                       s.test.query_index ~= index or
                       (type(s.test.query_sql) ~= 'string' and
                        not varbinary.is(s.test.query_sql)) or
                       #tostring(s.test.query_sql) == 0 or
                       type(s.captured) ~= 'table' or
                       type(s.captured.at) ~= 'string' or
                       type(s.captured.against_commit) ~= 'string' or
                       type(s.captured.tarantool_version) ~= 'string' or
                       type(s.captured.primary_dispatcher) ~= 'string' or
                       type(s.l1_result) ~= 'table' or
                       type(s.l1_result.ok) ~= 'boolean' or
                       type(s.l1_result.rows_sorted) ~= 'boolean' or
                       type(s.l1_result.rows) ~= 'table' or
                       type(s.l2_diagnostic) ~= 'table' or
                       (s.l2_diagnostic.status ~= 'success' and
                        s.l2_diagnostic.status ~= 'error') or
                       s.l1_result.ok ~= (s.l2_diagnostic.status == 'success') or
                       (s.l2_diagnostic.status == 'error' and
                        type(s.l2_diagnostic.error_code) ~= 'string') or
                       not valid_path_class(s.l3_path_class) then
                    reject(snapshot_path .. ': missing or inconsistent v1 fields')
                elseif metric ~= nil and
                       (not same_nullable(metric.path_class,
                                          s.l3_path_class.taken) or
                        not same_nullable(metric.fallback_reason,
                                          s.l3_path_class.reason)) then
                    reject(snapshot_path ..
                           ': planner metrics disagree with snapshot path metadata')
                elseif (s.l1_result.column_names == nil) ~=
                       (s.l1_result.column_types == nil) or
                       (s.l1_result.column_names ~= nil and
                        (type(s.l1_result.column_names) ~= 'table' or
                         type(s.l1_result.column_types) ~= 'table' or
                         #s.l1_result.column_names ~= #s.l1_result.column_types)) then
                    reject(snapshot_path .. ': inconsistent result metadata')
                else
                    snapshots_checked = snapshots_checked + 1
                end
            end
        end
    end
end

local snapshots = {}
walk(root .. '/snapshots', '.yaml', snapshots)
for _, path in ipairs(snapshots) do
    if not expected[path] then reject(path .. ': snapshot has no accepted manifest') end
end

for _, err in ipairs(errors) do io.stderr:write('[invalid] ' .. err .. '\n') end
io.write(string.format('manifests=%d snapshots=%d errors=%d\n',
    #manifests, snapshots_checked, #errors))
os.exit(#errors == 0 and 0 or 1)

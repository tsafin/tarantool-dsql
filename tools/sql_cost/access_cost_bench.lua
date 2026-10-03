#!/usr/bin/env tarantool
-- Exploratory, warm-cache SQL access-cost measurements. No planner knobs are
-- changed here. Run from an empty directory: tarantool access_cost_bench.lua.

local clock = require('clock')
local json = require('json')

local rows = tonumber(os.getenv('SQL_COST_ROWS') or '4096')
local repeats = tonumber(os.getenv('SQL_COST_REPEATS') or '7')
local iterations = tonumber(os.getenv('SQL_COST_ITERATIONS') or '300')
assert(rows >= 128 and rows % 16 == 0 and rows <= 1000000)
assert(repeats >= 1 and repeats <= 1000)
assert(iterations >= 1 and iterations <= 1000000)

box.cfg{log_level = 3}
box.execute([[SET SESSION "sql_seq_scan" = true]])

local function execute(sql, params)
    local result, err
    if params == nil then
        result, err = box.execute(sql)
    else
        result, err = box.execute(sql, params)
    end
    assert(result, tostring(err) .. ' SQL=' .. tostring(sql))
    return result
end

local function emit(row)
    io.stdout:write(json.encode(row), '\n')
    io.stdout:flush()
end

local cases = {
    {name = 'primary_point', sql = 'SELECT payload FROM %s INDEXED BY %s WHERE id = ?', loops = iterations},
    {name = 'secondary_covering', sql = 'SELECT a FROM %s INDEXED BY %s WHERE a = ?', loops = iterations},
    {name = 'secondary_payload', sql = 'SELECT payload FROM %s INDEXED BY %s WHERE a = ?', loops = iterations},
    {name = 'secondary_range', sql = 'SELECT payload FROM %s INDEXED BY %s WHERE a >= ? AND a < ?', loops = math.max(1, math.floor(iterations / 4))},
    {name = 'primary_scan', sql = 'SELECT payload FROM %s INDEXED BY %s', loops = math.max(1, math.floor(iterations / 100))},
}

-- Alternate engine order across repetitions to limit temporal bias. One
-- statement is prepared per engine/case; preparation is outside the timer.
local engines = {}
for _, engine in ipairs({'memtx', 'vinyl'}) do
    local table_name = 'cost_' .. engine
    local secondary = table_name .. '_a'
    execute(('CREATE TABLE %s (id INT PRIMARY KEY, a INT, payload STRING) WITH ENGINE = \'%s\''):format(table_name, engine))
    execute(('CREATE INDEX %s ON %s (a)'):format(secondary, table_name))
    local space = box.space[table_name]
    assert(space, 'SQL table missing: ' .. table_name)
    for i = 1, rows do
        -- Sixteen rows per secondary key. Whether fetching payload requires
        -- another storage access is engine-dependent; do not infer it here.
        space:insert({i, math.floor((i - 1) / 16), ('row-%08d'):format(i)})
    end
    engines[engine] = {table_name = table_name,
                       primary = space.index[0].name, secondary = secondary}
end

local prepared = {}
for _, engine in ipairs({'memtx', 'vinyl'}) do
    local info = engines[engine]
    prepared[engine] = {}
    for _, case in ipairs(cases) do
        local index_name
        if case.name == 'primary_point' or case.name == 'primary_scan' then
            index_name = info.primary
        else
            index_name = info.secondary
        end
        local sql = case.sql:format(info.table_name, index_name)
        local explain_params
        if case.name == 'secondary_range' then
            explain_params = {0, 1}
        elseif case.name ~= 'primary_scan' then
            explain_params = {1}
        end
        local plan = execute('EXPLAIN QUERY PLAN ' .. sql, explain_params).rows
        local stmt, err = box.prepare(sql)
        assert(stmt, tostring(err))
        prepared[engine][case.name] = {id = stmt.stmt_id, sql = sql, plan = plan}
    end
end

for repeat_no = 1, repeats do
    local order = repeat_no % 2 == 1 and {'memtx', 'vinyl'} or {'vinyl', 'memtx'}
    for _, case in ipairs(cases) do
        for _, engine in ipairs(order) do
            local stmt = prepared[engine][case.name]
            local params = nil
            if case.name == 'primary_point' then
                params = {1 + ((repeat_no * 37) % rows)}
            elseif case.name == 'secondary_range' then
                params = {(repeat_no * 7) % (rows / 16),
                          1 + ((repeat_no * 7) % (rows / 16))}
            elseif case.name ~= 'primary_scan' then
                params = {(repeat_no * 7) % (rows / 16)}
            end
            -- Warm each statement and its data path before collecting timing.
            local warm = execute(stmt.id, params)
            local result_rows = #warm.rows
            local expected_rows = case.name == 'primary_scan' and rows or
                (case.name == 'primary_point' and 1 or 16)
            assert(result_rows == expected_rows,
                   ('%s/%s returned %d rows, expected %d'):format(
                       engine, case.name, result_rows, expected_rows))
            local started = clock.monotonic()
            for _ = 1, case.loops do
                assert(#execute(stmt.id, params).rows == result_rows)
            end
            local elapsed_us = (clock.monotonic() - started) * 1000000
            emit({schema_version = 1, engine = engine, access = case.name,
                  repeat_no = repeat_no, rows = rows, result_rows = result_rows,
                  iterations = case.loops, elapsed_us = elapsed_us,
                  per_execution_us = elapsed_us / case.loops,
                  sql = stmt.sql, explain = stmt.plan,
                  tarantool_version = box.info.version,
                  fixture = 'uniform-16-per-secondary-key-v1',
                  cache_state = 'warm', timing_scope = 'prepared_execute_and_materialize'})
        end
    end
end

for _, engine in ipairs({'memtx', 'vinyl'}) do
    for _, case in ipairs(cases) do
        box.unprepare(prepared[engine][case.name].id)
    end
end
os.exit(0)

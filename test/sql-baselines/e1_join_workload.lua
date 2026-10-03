-- Deterministic execution workload for the bounded join-order solver.
-- A fresh process is required for every width triple: widths are cached in C.
local clock = require('clock')
local digest = require('digest')
local json = require('json')

local output = assert(os.getenv('E1_JOIN_OUTPUT'))
local engine = assert(os.getenv('E1_JOIN_ENGINE'))
local config = assert(os.getenv('E1_JOIN_CONFIG'))
local provenance = json.decode(assert(os.getenv('E1_JOIN_PROVENANCE')))
assert(engine == 'memtx' or engine == 'vinyl')

box.cfg({wal_mode = 'none', memtx_memory = 128 * 1024 * 1024,
         vinyl_memory = 128 * 1024 * 1024})
assert(box.execute([[SET SESSION "sql_seq_scan" = true]]))

local suffix = " WITH ENGINE = '" .. engine .. "'"
local ddl = {
    'CREATE TABLE e1_a (id INT PRIMARY KEY, k INT, bucket INT)' .. suffix,
    'CREATE TABLE e1_b (id INT PRIMARY KEY, k INT, flag INT)' .. suffix,
    'CREATE TABLE e1_c (id INT PRIMARY KEY, k INT, band INT)' .. suffix,
    'CREATE TABLE e1_d (id INT PRIMARY KEY, k INT, flag INT)' .. suffix,
    'CREATE INDEX e1_a_k ON e1_a (k)',
    'CREATE INDEX e1_b_k ON e1_b (k)',
    'CREATE INDEX e1_c_k ON e1_c (k)',
    'CREATE INDEX e1_d_k ON e1_d (k)',
}
for _, statement in ipairs(ddl) do
    assert(box.execute(statement))
end
-- Distribution is deliberately nonuniform. k=0 is hot; k=19 is rare.
-- b and d have multiple rows per key, yielding real join fanout.
for id = 1, 80 do
    local k = id <= 32 and 0 or (id % 20)
    assert(box.execute(('INSERT INTO e1_a VALUES (%d, %d, %d)'):format(
        id, k, id % 8)))
end
for id = 1, 120 do
    local k = id <= 48 and 0 or (id % 20)
    assert(box.execute(('INSERT INTO e1_b VALUES (%d, %d, %d)'):format(
        id, k, id % 3)))
end
for id = 1, 60 do
    local k = id <= 18 and 0 or (id % 20)
    assert(box.execute(('INSERT INTO e1_c VALUES (%d, %d, %d)'):format(
        id, k, id % 4)))
end
for id = 1, 50 do
    local k = id <= 10 and 0 or (id % 20)
    assert(box.execute(('INSERT INTO e1_d VALUES (%d, %d, %d)'):format(
        id, k, id % 5)))
end
for _, table_name in ipairs({'e1_a', 'e1_b', 'e1_c', 'e1_d'}) do
    assert(box.execute('ANALYZE ' .. table_name))
end

local queries = {
    {id = 'two-hot', sql = [[SELECT a.id, b.id FROM e1_a a
        JOIN e1_b b ON a.k = b.k WHERE a.k = 0
        ORDER BY a.id, b.id]]},
    {id = 'two-rare', sql = [[SELECT a.id, b.id FROM e1_a a
        JOIN e1_b b ON a.k = b.k WHERE a.k = 19
        ORDER BY a.id, b.id]]},
    {id = 'three-filtered', sql = [[SELECT a.id, b.id, c.id FROM e1_a a
        JOIN e1_b b ON a.k = b.k JOIN e1_c c ON b.k = c.k
        WHERE a.bucket = 7 AND b.flag = 2 AND c.band = 1
        ORDER BY a.id, b.id, c.id]]},
    {id = 'four-selective', sql = [[SELECT a.id, b.id, c.id, d.id
        FROM e1_a a JOIN e1_b b ON a.k = b.k
        JOIN e1_c c ON b.k = c.k JOIN e1_d d ON c.k = d.k
        WHERE a.k = 19 AND d.flag = 4
        ORDER BY a.id, b.id, c.id, d.id]]},
    {id = 'three-empty', sql = [[SELECT a.id, b.id, c.id FROM e1_a a
        JOIN e1_b b ON a.k = b.k JOIN e1_c c ON b.k = c.k
        WHERE a.k = 999 ORDER BY a.id, b.id, c.id]]},
    {id = 'left-join', sql = [[SELECT a.id, b.id FROM e1_a a
        LEFT JOIN e1_b b ON a.k = b.k AND b.flag = 1
        WHERE a.k = 19 ORDER BY a.id, b.id]]},
}

local function hex_sha(value)
    return string.hex(digest.sha256(value))
end

local file = assert(io.open(output, 'w'))
local prepared = {}
for _, query in ipairs(queries) do
    local started = clock.monotonic()
    local stmt = assert(box.prepare(query.sql))
    local prepare_us = math.max(1, math.floor((clock.monotonic() - started) * 1e6))
    local plan = assert(box.execute('EXPLAIN QUERY PLAN ' .. query.sql)).rows
    prepared[query.id] = {stmt = stmt, prepare_us = prepare_us,
                          plan_sha256 = hex_sha(json.encode(plan)), plan = plan}
end

-- Alternate order each round to reduce systematic warming effects. A round
-- contains every query, and both width configurations are run in fresh
-- processes by the Python driver. Repetitions are not concurrent.
for round = 0, 7 do
    for offset = 1, #queries do
        local index = round % 2 == 0 and offset or #queries - offset + 1
        local query = queries[index]
        local info = prepared[query.id]
        local started = clock.monotonic()
        local result = assert(box.execute(info.stmt.stmt_id))
        local elapsed_us = math.max(1, math.floor((clock.monotonic() - started) * 1e6))
        local row = {
            schema_version = 1, workload_id = 'bounded-dp-joins-v1',
            query_id = query.id, sql = query.sql,
            engine = engine, dispatcher = provenance.dispatcher,
            configuration = config, source_commit = provenance.source_commit,
            binary_sha256 = provenance.binary_sha256,
            data_sha256 = provenance.data_sha256,
            statistics_id = provenance.statistics_id,
            widths = provenance.widths, ['repeat'] = round,
            warmup = round < 3, elapsed_us = elapsed_us,
            prepare_us = info.prepare_us, plan_sha256 = info.plan_sha256,
            plan = info.plan, actual_rows = #result.rows,
            result_sha256 = hex_sha(json.encode(result.rows)),
        }
        file:write(json.encode(row), '\n')
    end
end
file:close()
for _, info in pairs(prepared) do
    box.unprepare(info.stmt.stmt_id)
end
os.exit(0)

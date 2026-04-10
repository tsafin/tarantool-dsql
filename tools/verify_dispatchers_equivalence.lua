#!/usr/bin/env tarantool

local fio = require('fio')
local json = require('json')

local root = os.getenv('VDBE_EQUIV_TMPDIR') or fio.cwd()
local memtx_dir = fio.pathjoin(root, 'memtx')
local wal_dir = fio.pathjoin(root, 'wal')
local vinyl_dir = fio.pathjoin(root, 'vinyl')

fio.mktree(memtx_dir)
fio.mktree(wal_dir)
fio.mktree(vinyl_dir)

box.cfg{
    memtx_memory = 268435456,
    memtx_dir = memtx_dir,
    wal_dir = wal_dir,
    vinyl_dir = vinyl_dir,
    log = fio.pathjoin(root, 'tarantool.log'),
}

local function canonicalize(value)
    local value_type = type(value)
    if value == nil or value_type == 'number' or value_type == 'string' or
       value_type == 'boolean' then
        return value
    end
    if value_type ~= 'table' then
        return tostring(value)
    end

    local max_index = 0
    for key, _ in pairs(value) do
        if type(key) ~= 'number' or key <= 0 or key % 1 ~= 0 then
            max_index = -1
            break
        end
        if key > max_index then
            max_index = key
        end
    end

    if max_index >= 0 then
        local result = {}
        for i = 1, max_index do
            result[i] = canonicalize(value[i])
        end
        return result
    end

    local keys = {}
    for key, _ in pairs(value) do
        keys[#keys + 1] = key
    end
    table.sort(keys, function(lhs, rhs)
        return tostring(lhs) < tostring(rhs)
    end)

    local result = {}
    for _, key in ipairs(keys) do
        result[tostring(key)] = canonicalize(value[key])
    end
    return result
end

local function execute_sql(sql, bind)
    local ok, result
    if bind == nil then
        ok, result = pcall(box.execute, sql)
    else
        ok, result = pcall(box.execute, sql, bind)
    end
    if ok then
        local rows
        local row_count
        local metadata
        if type(result) == 'table' then
            rows = canonicalize(result.rows)
            row_count = result.row_count
            metadata = canonicalize(result.metadata)
        else
            rows = nil
            row_count = nil
            metadata = nil
        end
        return {
            ok = true,
            rows = rows,
            row_count = row_count,
            metadata = metadata,
        }
    end
    return {
        ok = false,
        error = tostring(result),
    }
end

local trace = {
    dispatcher_mode = os.getenv('VDBE_DISPATCHER') or 'auto',
    tarantool_version = _TARANTOOL,
    steps = {},
}

local trace_progress = os.getenv('VDBE_EQUIV_TRACE') == '1'

local function record(name, sql, bind)
    if trace_progress then
        io.stderr:write(string.format("[equiv] %s\n", name))
        io.stderr:flush()
    end
    trace.steps[#trace.steps + 1] = {
        name = name,
        sql = sql,
        bind = canonicalize(bind),
        result = execute_sql(sql, bind),
    }
end

record('enable_seq_scan', [[SET SESSION "sql_seq_scan" = true;]])
record('drop_tables', [[DROP TABLE IF EXISTS t_eq]])
record('drop_secondary', [[DROP TABLE IF EXISTS t_aux]])
record('create_main', [[
    CREATE TABLE t_eq(
        id INTEGER PRIMARY KEY,
        grp INTEGER,
        val INTEGER,
        note TEXT,
        CHECK(val >= 0)
    )
]])
record('create_aux', [[
    CREATE TABLE t_aux(
        id INTEGER PRIMARY KEY,
        ref INTEGER,
        payload TEXT
    )
]])

for i = 1, 12 do
    record(string.format('insert_main_%02d', i),
           'INSERT INTO t_eq VALUES (?, ?, ?, ?)',
           {i, i % 3, i * 10, string.format('row-%02d', i)})
end

for i = 1, 6 do
    record(string.format('insert_aux_%02d', i),
           'INSERT INTO t_aux VALUES (?, ?, ?)',
           {i, i, string.format('aux-%02d', i)})
end

record('select_all', [[SELECT id, grp, val, note FROM t_eq ORDER BY id]])
record('select_where', [[
    SELECT id, val FROM t_eq
    WHERE grp = 1 AND val >= 40
    ORDER BY id DESC
]])
record('select_join', [[
    SELECT e.id, e.note, a.payload
    FROM t_eq AS e LEFT JOIN t_aux AS a ON a.ref = e.id
    WHERE e.id <= 6
    ORDER BY e.id
]])
record('select_aggregate', [[
    SELECT grp, COUNT(*), SUM(val), MIN(val), MAX(val)
    FROM t_eq
    GROUP BY grp
    ORDER BY grp
]])
record('select_arithmetic', [[
    SELECT id, val + 5, val * 2, val / 5, val % 7
    FROM t_eq
    WHERE id BETWEEN 3 AND 8
    ORDER BY id
]])
record('update_rows', [[
    UPDATE t_eq
    SET val = val + 7, note = note || '-u'
    WHERE id IN (2, 4, 6)
]])
record('delete_rows', [[DELETE FROM t_eq WHERE id IN (11, 12)]])
record('check_violation', [[INSERT INTO t_eq VALUES (99, 0, -1, 'bad')]])
record('duplicate_pk', [[INSERT INTO t_eq VALUES (1, 0, 1, 'dup')]])
record('final_projection', [[
    SELECT id, grp, val, note
    FROM t_eq
    ORDER BY id
]])
record('final_count', [[SELECT COUNT(*), SUM(val) FROM t_eq]])

print(json.encode(canonicalize(trace)))
os.exit(0)

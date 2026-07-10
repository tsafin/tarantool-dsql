#!/usr/bin/env tarantool
-- test/sql-baselines/perf/aggregate.lua
-- M0.7 offline aggregation tool.
--
-- Reads a directory of perf-trail CSV files (one per CI run, named
-- YYYY-MM-DD-<sha>.csv) and produces a markdown trend table to stdout.
--
-- CLI:
--   tarantool test/sql-baselines/perf/aggregate.lua <csv_dir> \
--       [--last-n=10] [--test-filter=<pattern>]
--
-- Example:
--   tarantool test/sql-baselines/perf/aggregate.lua /tmp/perf-runs --last-n=5
--   tarantool test/sql-baselines/perf/aggregate.lua /tmp/perf-runs --test-filter=select1

local fio = require('fio')

-- ---------------------------------------------------------------------------
-- CSV parser (minimal, RFC-4180 aware)
-- ---------------------------------------------------------------------------
local function csv_parse_line(line)
    local fields = {}
    local i = 1
    local n = #line
    while i <= n do
        if line:sub(i, i) == '"' then
            -- quoted field
            i = i + 1
            local buf = {}
            while i <= n do
                local c = line:sub(i, i)
                if c == '"' then
                    if line:sub(i + 1, i + 1) == '"' then
                        buf[#buf + 1] = '"'
                        i = i + 2
                    else
                        i = i + 1
                        break
                    end
                else
                    buf[#buf + 1] = c
                    i = i + 1
                end
            end
            fields[#fields + 1] = table.concat(buf)
            if line:sub(i, i) == ',' then i = i + 1 end
        else
            local j = line:find(',', i, true)
            if j then
                fields[#fields + 1] = line:sub(i, j - 1)
                i = j + 1
            else
                fields[#fields + 1] = line:sub(i)
                break
            end
        end
    end
    return fields
end

local function read_csv(path)
    local fh, err = io.open(path, 'r')
    if not fh then
        io.stderr:write(string.format('[aggregate] cannot open %s: %s\n', path, tostring(err)))
        return nil
    end
    local rows = {}
    local header = nil
    for line in fh:lines() do
        local fields = csv_parse_line(line)
        if not header then
            header = fields
        else
            local row = {}
            for i, col in ipairs(header) do
                row[col] = fields[i]
            end
            rows[#rows + 1] = row
        end
    end
    fh:close()
    return rows
end

-- ---------------------------------------------------------------------------
-- Argument parsing
-- ---------------------------------------------------------------------------
local function parse_args(argv)
    local opts = {
        csv_dir     = nil,
        last_n      = 10,
        test_filter = nil,
    }
    for _, arg in ipairs(argv) do
        local n = arg:match('^%-%-last%-n=(.+)$')
        if n then opts.last_n = tonumber(n) or 10; goto continue end
        local f = arg:match('^%-%-test%-filter=(.+)$')
        if f then opts.test_filter = f; goto continue end
        if not arg:match('^%-%-') then
            opts.csv_dir = arg
        end
        ::continue::
    end
    return opts
end

-- ---------------------------------------------------------------------------
-- Percentile helper (nearest-rank)
-- ---------------------------------------------------------------------------
local function percentile(sorted, pct)
    local n = #sorted
    if n == 0 then return nil end
    local rank = math.ceil(pct / 100.0 * n)
    if rank < 1 then rank = 1 end
    if rank > n then rank = n end
    return sorted[rank]
end

local function sorted_nums(t)
    local c = {}
    for _, v in ipairs(t) do
        local n = tonumber(v)
        if n then c[#c + 1] = n end
    end
    table.sort(c)
    return c
end

-- ---------------------------------------------------------------------------
-- Main
-- ---------------------------------------------------------------------------
local function main(argv)
    local opts = parse_args(argv)

    if not opts.csv_dir then
        io.stderr:write('Usage: tarantool aggregate.lua <csv_dir> [--last-n=10] [--test-filter=<pattern>]\n')
        os.exit(1)
    end

    -- Enumerate CSV files (skip sample.csv and non-matching names).
    local all_files = fio.listdir(opts.csv_dir)
    if not all_files then
        io.stderr:write(string.format('[aggregate] cannot list directory: %s\n', opts.csv_dir))
        os.exit(1)
    end

    local csv_files = {}
    for _, fname in ipairs(all_files) do
        -- Accept YYYY-MM-DD-<sha>.csv but not sample.csv
        if fname:match('^%d%d%d%d%-%d%d%-%d%d%-[a-f0-9]+%.csv$') then
            csv_files[#csv_files + 1] = fname
        end
    end
    table.sort(csv_files)  -- lexicographic = chronological for YYYY-MM-DD prefix

    -- Keep last N runs.
    if #csv_files > opts.last_n then
        local trimmed = {}
        local start = #csv_files - opts.last_n + 1
        for i = start, #csv_files do
            trimmed[#trimmed + 1] = csv_files[i]
        end
        csv_files = trimmed
    end

    if #csv_files == 0 then
        io.stderr:write('[aggregate] no matching CSV files found\n')
        os.exit(0)
    end

    -- Load all rows, keyed by (test_id, engine, dispatcher).
    -- Structure: data[key] = { meta={test_id,engine,dispatcher}, runs={} }
    -- runs[i] = { date, sha, p50, p95, rows, mem }
    local data   = {}
    local keys   = {}   -- ordered unique keys
    local key_set = {}

    for _, fname in ipairs(csv_files) do
        local date, sha = fname:match('^(%d%d%d%d%-%d%d%-%d%d)%-([a-f0-9]+)%.csv$')
        local path = fio.pathjoin(opts.csv_dir, fname)
        local rows = read_csv(path)
        if rows then
            for _, row in ipairs(rows) do
                local tid = row['test_id'] or ''
                -- Apply test filter if specified.
                if opts.test_filter and not tid:find(opts.test_filter, 1, true) then
                    goto next_row
                end
                local eng  = row['engine'] or ''
                local disp = row['dispatcher'] or ''
                local key  = tid .. '\0' .. eng .. '\0' .. disp
                if not key_set[key] then
                    key_set[key] = true
                    keys[#keys + 1] = key
                    data[key] = {
                        meta = { test_id = tid, engine = eng, dispatcher = disp },
                        runs = {},
                    }
                end
                data[key].runs[#data[key].runs + 1] = {
                    date = date or '?',
                    sha  = sha  or '?',
                    p50  = tonumber(row['time_p50_us']),
                    p95  = tonumber(row['time_p95_us']),
                    rows = tonumber(row['rows_returned']),
                    mem  = tonumber(row['memory_peak_bytes']),
                }
                ::next_row::
            end
        end
    end

    -- Print markdown table.
    -- Columns: test_id | engine | dispatcher | runs | p50_min | p50_median | p50_max | p95_median
    local header = '| test_id | engine | dispatcher | #runs | p50_min_us | p50_med_us | p50_max_us | p95_med_us |'
    local sep    = '|---------|--------|------------|-------|------------|------------|------------|------------|'

    print(string.format('\n## Perf-trail aggregate — last %d run(s)\n', #csv_files))
    print(header)
    print(sep)

    table.sort(keys)
    for _, key in ipairs(keys) do
        local entry = data[key]
        local meta  = entry.meta
        local runs  = entry.runs

        local p50s = {}
        local p95s = {}
        for _, r in ipairs(runs) do
            if r.p50 then p50s[#p50s + 1] = r.p50 end
            if r.p95 then p95s[#p95s + 1] = r.p95 end
        end

        local sp50 = sorted_nums(p50s)
        local sp95 = sorted_nums(p95s)

        local function fmt(v) return v and tostring(v) or '-' end

        local p50_min = fmt(sp50[1])
        local p50_med = fmt(percentile(sp50, 50))
        local p50_max = fmt(sp50[#sp50])
        local p95_med = fmt(percentile(sp95, 50))

        print(string.format('| %s | %s | %s | %d | %s | %s | %s | %s |',
            meta.test_id, meta.engine, meta.dispatcher,
            #runs, p50_min, p50_med, p50_max, p95_med))
    end

    print('')
end

main(arg)

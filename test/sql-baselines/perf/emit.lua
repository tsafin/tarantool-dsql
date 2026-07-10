-- test/sql-baselines/perf/emit.lua
-- M0.7 perf-trail emitter.
--
-- Usage from the M0.2 harness:
--
--   local emit = require('test.sql-baselines.perf.emit')
--   emit.begin_run()
--   emit.record(test_id, engine, dispatcher, function() return box.execute(sql) end)
--   ...
--   emit.finish_run()   -- writes the CSV to test/sql-baselines/perf/<date>-<sha>.csv
--
-- Environment variables:
--   PERF_REPS          -- number of repetitions per query (default: 7, minimum: 5)
--   PERF_OUT_DIR       -- directory for CSV output (default: test/sql-baselines/perf)
--   PERF_COMMIT_SHA    -- 7-char commit SHA override (default: auto-detected via git)

local fiber = require('fiber')
local fio   = require('fio')

local M = {}

-- Internal state: accumulated samples per (test_id, engine, dispatcher) key.
-- Each entry is a list of {time_us, rows_returned, memory_delta} per repetition.
local _samples = {}

-- Minimum repetitions required before a warning is emitted.
local MIN_REPS = 5

-- ---------------------------------------------------------------------------
-- Helper: CSV field escaping (RFC-4180)
-- ---------------------------------------------------------------------------
local function csv_field(v)
    local s = tostring(v)
    if s:find('[,"\n\r]') then
        s = '"' .. s:gsub('"', '""') .. '"'
    end
    return s
end

local function csv_row(fields)
    local parts = {}
    for _, f in ipairs(fields) do
        parts[#parts + 1] = csv_field(f)
    end
    return table.concat(parts, ',')
end

-- ---------------------------------------------------------------------------
-- Helper: percentile over a sorted list of numbers
-- ---------------------------------------------------------------------------
local function percentile(sorted, pct)
    local n = #sorted
    if n == 0 then return 0 end
    -- nearest-rank method
    local rank = math.ceil(pct / 100.0 * n)
    if rank < 1 then rank = 1 end
    if rank > n then rank = n end
    return sorted[rank]
end

local function sorted_copy(t)
    local c = {}
    for i, v in ipairs(t) do c[i] = v end
    table.sort(c)
    return c
end

-- ---------------------------------------------------------------------------
-- Helper: get short commit SHA (7 chars)
-- ---------------------------------------------------------------------------
local function get_commit_sha()
    local override = os.getenv('PERF_COMMIT_SHA')
    if override and #override > 0 then
        return override:sub(1, 7)
    end

    -- Try reading from .git/HEAD directly (works for normal checkouts).
    -- HEAD contains either "ref: refs/heads/<branch>\n" or a bare SHA.
    local head_path = fio.pathjoin(fio.cwd(), '.git', 'HEAD')
    local fh = io.open(head_path, 'r')
    if fh then
        local line = fh:read('*l')
        fh:close()
        if line then
            local ref = line:match('^ref: (.+)$')
            if ref then
                local ref_path = fio.pathjoin(fio.cwd(), '.git', ref)
                local rfh = io.open(ref_path, 'r')
                if rfh then
                    local sha = rfh:read('*l')
                    rfh:close()
                    if sha and #sha >= 7 then
                        return sha:sub(1, 7)
                    end
                end
            elseif #line >= 7 then
                -- detached HEAD — line is the SHA itself
                return line:sub(1, 7)
            end
        end
    end

    -- Fallback: try os.execute into a temp file.
    local tmp = os.tmpname()
    local rc = os.execute('git rev-parse --short HEAD > ' .. tmp .. ' 2>/dev/null')
    if rc == 0 then
        local tfh = io.open(tmp, 'r')
        if tfh then
            local sha = tfh:read('*l')
            tfh:close()
            os.remove(tmp)
            if sha and #sha > 0 then
                return sha:sub(1, 7)
            end
        end
    end
    os.remove(tmp)

    return 'unknown'
end

-- ---------------------------------------------------------------------------
-- Helper: today's date as YYYY-MM-DD
-- ---------------------------------------------------------------------------
local function today_str()
    return os.date('%Y-%m-%d')
end

-- ---------------------------------------------------------------------------
-- Helper: read current memory usage from box.runtime.info()
-- Returns bytes, or -1 if unavailable.
-- ---------------------------------------------------------------------------
local function mem_now()
    local ok, info = pcall(function() return box.runtime.info() end)
    if ok and info and info.used then
        return tonumber(info.used)
    end
    return -1
end

-- ---------------------------------------------------------------------------
-- Public API
-- ---------------------------------------------------------------------------

--- Reset accumulated sample state.  Call at the start of a run.
function M.begin_run()
    _samples = {}
end

--- Record timing for one (test_id, engine, dispatcher, query_fn) combination.
--
-- @param test_id    string — e.g. "sql-tap/select1/q01" (per SCHEMA.md §L7)
-- @param engine     string — "memtx" | "vinyl"
-- @param dispatcher string — "generated" | "cnp" | "llvm"
-- @param query_fn   function() — executes the query; must return a result set
--                   table (with .rows field) or nil.  Called PERF_REPS times.
--
-- Returns nothing; accumulates internally.
function M.record(test_id, engine, dispatcher, query_fn)
    local reps = tonumber(os.getenv('PERF_REPS')) or 7
    if reps < MIN_REPS then
        io.stderr:write(string.format(
            '[perf/emit] WARNING: PERF_REPS=%d < %d for %s; results will be noisy\n',
            reps, MIN_REPS, test_id))
    end

    local key = table.concat({test_id, engine, dispatcher}, '\0')
    if not _samples[key] then
        _samples[key] = {
            test_id    = test_id,
            engine     = engine,
            dispatcher = dispatcher,
            times      = {},
            rows       = {},
            mems       = {},
        }
    end
    local bucket = _samples[key]

    for _ = 1, reps do
        local mem_before = mem_now()
        local t0 = fiber.clock64()
        local result = query_fn()
        local t1 = fiber.clock64()
        local mem_after = mem_now()

        local elapsed_us = tonumber(t1 - t0)  -- fiber.clock64() returns microseconds

        local nrows = 0
        if result and result.rows then
            nrows = #result.rows
        end

        local mem_delta = -1
        if mem_before ~= -1 and mem_after ~= -1 then
            mem_delta = mem_after - mem_before
            if mem_delta < 0 then mem_delta = 0 end
        end

        bucket.times[#bucket.times + 1] = elapsed_us
        bucket.rows[#bucket.rows + 1]   = nrows
        bucket.mems[#bucket.mems + 1]   = mem_delta
    end
end

--- Emit the CSV file and return its path.  Call after all queries have run.
--
-- @param out_dir  optional string — directory for output (overrides env var)
-- @return string  path of the written CSV file
function M.finish_run(out_dir)
    out_dir = out_dir
        or os.getenv('PERF_OUT_DIR')
        or fio.pathjoin(fio.cwd(), 'test', 'sql-baselines', 'perf')

    -- Ensure output directory exists.
    fio.mktree(out_dir)

    local sha      = get_commit_sha()
    local datestr  = today_str()
    local filename = string.format('%s-%s.csv', datestr, sha)
    local path     = fio.pathjoin(out_dir, filename)

    local fh, err = io.open(path, 'w')
    if not fh then
        error(string.format('[perf/emit] cannot open %s for writing: %s', path, tostring(err)))
    end

    -- Header
    fh:write('test_id,engine,dispatcher,time_p50_us,time_p95_us,rows_returned,memory_peak_bytes\n')

    -- One row per (test_id, engine, dispatcher) key
    for _, bucket in pairs(_samples) do
        local n = #bucket.times
        if n < MIN_REPS then
            io.stderr:write(string.format(
                '[perf/emit] WARNING: only %d sample(s) for %s/%s/%s\n',
                n, bucket.test_id, bucket.engine, bucket.dispatcher))
        end

        local st = sorted_copy(bucket.times)
        local p50 = percentile(st, 50)
        local p95 = percentile(st, 95)

        -- rows_returned: median of row counts across reps
        local sr = sorted_copy(bucket.rows)
        local rows_p50 = percentile(sr, 50)

        -- memory_peak_bytes: maximum observed delta (peak allocation pressure)
        local mem_peak = -1
        for _, v in ipairs(bucket.mems) do
            if v ~= -1 then
                if mem_peak == -1 or v > mem_peak then
                    mem_peak = v
                end
            end
        end

        fh:write(csv_row({
            bucket.test_id,
            bucket.engine,
            bucket.dispatcher,
            p50,
            p95,
            rows_p50,
            mem_peak,
        }) .. '\n')
    end

    fh:close()
    io.stderr:write(string.format('[perf/emit] wrote %s\n', path))
    return path
end

return M

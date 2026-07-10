#!/usr/bin/env tarantool
-- classify.lua — M0.1 test auto-classifier for the Tarantool SQL parity corpus.
--
-- Usage:
--   cd <repo-root>
--   ./src/tarantool test/sql-baselines/classify.lua
--
-- Output:
--   test/sql-baselines/classification.yaml  (overwritten in place)
--
-- The classifier:
--   1. Scans test/sql/, test/sql-tap/, test/sql-luatest/ for test files.
--   2. Regex-detects SQL feature markers in the full file text.
--   3. Emits classification.yaml with one entry per file.
--
-- Tag taxonomy derived from docs/vdbe/current_sql_feature_matrix.md.
-- Conservative detection: false-positive tags are preferred over misses.

-- ---------------------------------------------------------------------------
-- Configuration
-- ---------------------------------------------------------------------------

-- Derive paths from arg[0] (the script path) with a sensible fallback for
-- direct invocation. SCRIPT_DIR locates sibling library files; REPO_ROOT
-- is SCRIPT_DIR with the "test/sql-baselines" suffix stripped off, used
-- as the base for scanning the test suites.
local SCRIPT_PATH = arg and arg[0] or "test/sql-baselines/classify.lua"
local SCRIPT_DIR = SCRIPT_PATH:gsub("[/\\][^/\\]+$", "")
if SCRIPT_DIR == "" then SCRIPT_DIR = "." end
local REPO_ROOT = SCRIPT_DIR:gsub("[/\\]?test[/\\]sql%-baselines$", "")
if REPO_ROOT == "" then REPO_ROOT = "." end

-- canonical_yaml.lua is the shared emitter used by snapshot harness (M0.2),
-- diff tool (M0.4), and this classifier. Loaded via dofile() so it works
-- without depending on LUA_PATH configuration.
local yaml = dofile(SCRIPT_DIR .. "/lib/canonical_yaml.lua")

local OUTPUT_FILE = REPO_ROOT .. "/test/sql-baselines/classification.yaml"

local SUITES = {
    { name = "sql",          dir = REPO_ROOT .. "/test/sql",          pattern = "%.test%.lua$" },
    { name = "sql-tap",      dir = REPO_ROOT .. "/test/sql-tap",      pattern = "%.test%.lua$" },
    { name = "sql-luatest",  dir = REPO_ROOT .. "/test/sql-luatest",  pattern = "_test%.lua$"  },
}

-- ---------------------------------------------------------------------------
-- Disabled tests from suite.ini files
-- Parse the "disabled = ..." multi-line field from each suite.ini.
-- ---------------------------------------------------------------------------

local function parse_disabled(ini_path)
    local disabled = {}
    local f = io.open(ini_path, "r")
    if not f then return disabled end
    local content = f:read("*a")
    f:close()

    -- Find the disabled = ... block (ends at first blank line or next key)
    local block = content:match("disabled%s*=%s*(.-)%s*\n%s*\n")
                  or content:match("disabled%s*=%s*(.-)%s*\n[%a_]")
                  or content:match("disabled%s*=%s*(.+)$")
    if not block then return disabled end

    -- Each entry: filename optionally followed by "; <comment>"
    for entry in block:gmatch("([^;\n]+)") do
        local name = entry:match("^%s*([%w%-%._]+%.lua)%s*")
        if name then
            disabled[name] = true
        end
    end
    return disabled
end

-- ---------------------------------------------------------------------------
-- File scanning
-- ---------------------------------------------------------------------------

local function list_files(dir, pattern)
    local files = {}
    -- Use ls to enumerate; tarantool's fio module could also work.
    local handle = io.popen("ls -1 " .. dir .. " 2>/dev/null")
    if not handle then return files end
    for line in handle:lines() do
        if line:match(pattern) then
            files[#files + 1] = line
        end
    end
    handle:close()
    table.sort(files)
    return files
end

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local content = f:read("*a")
    f:close()
    return content
end

-- ---------------------------------------------------------------------------
-- Tag detection rules
-- Each rule is { tag = "name", pattern = lua_pattern | function(text)->bool }
-- Patterns are matched case-insensitively against the full file text.
-- Conservative approach: ANY match anywhere in the file sets the tag.
-- ---------------------------------------------------------------------------

-- Helper: case-insensitive search
local function ci(text, pat)
    return text:lower():find(pat, 1, true) ~= nil
end

-- Helper: regex on lowered text
local function re(text, pat)
    return text:lower():match(pat) ~= nil
end

local TAG_RULES = {
    -- Basic SELECT: any SELECT statement at all
    {
        tag = "single_table_select",
        -- Present if file has SELECT but from only one table (conservative:
        -- tag any file that has SELECT; multi-table is also tagged separately)
        detect = function(t)
            return re(t, "%sselect%s") or re(t, "^select%s") or t:lower():find("\"select ") ~= nil
        end,
    },

    -- Projection: SELECT with explicit column list (not just SELECT *)
    -- We tag this when we see "SELECT <identifier>" without just "*"
    {
        tag = "projection",
        detect = function(t)
            -- Conservative: any SELECT = projection
            return re(t, "select%s+[%w\"'%(%*]")
        end,
    },

    -- Filter: WHERE clause
    {
        tag = "filter",
        detect = function(t)
            return re(t, "%swhere%s")
        end,
    },

    -- ORDER BY
    {
        tag = "order_by",
        detect = function(t)
            return re(t, "order%s+by%s")
        end,
    },

    -- LIMIT clause
    {
        tag = "limit",
        detect = function(t)
            return re(t, "%slimit%s")
        end,
    },

    -- OFFSET clause
    {
        tag = "offset",
        detect = function(t)
            return re(t, "%soffset%s")
        end,
    },

    -- DISTINCT keyword in SELECT
    {
        tag = "distinct",
        detect = function(t)
            return re(t, "select%s+distinct%s") or re(t, "select%s+all%s")
        end,
    },

    -- GROUP BY
    {
        tag = "group_by",
        detect = function(t)
            return re(t, "group%s+by%s")
        end,
    },

    -- HAVING
    {
        tag = "having",
        detect = function(t)
            return re(t, "%shaving%s")
        end,
    },

    -- Aggregate functions: COUNT, SUM, AVG, MIN, MAX, TOTAL, GROUP_CONCAT
    {
        tag = "aggregate",
        detect = function(t)
            local tl = t:lower()
            return tl:find("%Wcount%s*%(") ~= nil
                or tl:find("%Wsum%s*%(") ~= nil
                or tl:find("%Wavg%s*%(") ~= nil
                or tl:find("%Wmin%s*%(") ~= nil
                or tl:find("%Wmax%s*%(") ~= nil
                or tl:find("%Wtotal%s*%(") ~= nil
                or tl:find("%Wgroup_concat%s*%(") ~= nil
        end,
    },

    -- INNER JOIN (explicit or implicit comma join)
    {
        tag = "inner_join",
        detect = function(t)
            local tl = t:lower()
            -- explicit INNER JOIN or JOIN without LEFT/RIGHT/CROSS/NATURAL
            return tl:find("%winner%s+join%s") ~= nil
                or tl:find("%Wjoin%s") ~= nil  -- any JOIN is conservatively inner_join too
        end,
    },

    -- LEFT JOIN / LEFT OUTER JOIN
    {
        tag = "left_join",
        detect = function(t)
            return re(t, "left%s+outer%s+join%s") or re(t, "left%s+join%s")
        end,
    },

    -- CROSS JOIN or comma join (FROM a, b)
    {
        tag = "cross_join",
        detect = function(t)
            return re(t, "cross%s+join%s") or re(t, "from%s+%w[%w_]*%s*,%s*%w")
        end,
    },

    -- NATURAL JOIN
    {
        tag = "natural_join",
        detect = function(t)
            return re(t, "natural%s+join%s")
        end,
    },

    -- Scalar subquery: (SELECT ...) used as expression
    -- Conservative: any nested SELECT in parens
    {
        tag = "scalar_subquery",
        detect = function(t)
            -- Look for (SELECT ... ) pattern — paren before SELECT
            return re(t, "%(select%s")
        end,
    },

    -- EXISTS / NOT EXISTS
    {
        tag = "exists",
        detect = function(t)
            return re(t, "%Wexists%s*%(")
        end,
    },

    -- IN (SELECT ...) subquery
    {
        tag = "in_subquery",
        detect = function(t)
            return re(t, "%Win%s*%(select%s") or re(t, "%Wnot%s+in%s*%(select%s")
        end,
    },

    -- Correlated subquery: references outer table in inner SELECT
    -- Heuristic: nested SELECT that contains a WHERE or ON with an outer ref
    -- Conservative: if there's a subquery at all, also tag correlated_subquery
    {
        tag = "correlated_subquery",
        detect = function(t)
            -- Look for subquery patterns with outer table references
            -- Conservative: tag if there are multiple levels of nested select
            local tl = t:lower()
            -- Count "select" occurrences — if more than one, likely correlated
            local _, count = tl:gsub("%Wselect%s", "")
            return count >= 2 and tl:find("%(select%s") ~= nil
        end,
    },

    -- Derived table / subquery in FROM clause
    {
        tag = "derived_table",
        detect = function(t)
            -- FROM (SELECT ...)
            return re(t, "from%s*%(select%s")
        end,
    },

    -- Non-recursive CTE: WITH ... AS (SELECT ...)
    -- Detect WITH <name> AS (...) pattern. Conservative: any WITH ... AS (
    {
        tag = "cte",
        detect = function(t)
            -- "with <word> as (" is the core CTE pattern
            return (re(t, "%Wwith%s+%w") and re(t, "%Was%s*%("))
                or re(t, "%Wwith%s+%w[%w_,% ]*%s+as%s*%(")
        end,
    },

    -- Recursive CTE: WITH RECURSIVE
    {
        tag = "recursive_cte",
        detect = function(t)
            return re(t, "with%s+recursive%s")
        end,
    },

    -- UNION / UNION ALL
    {
        tag = "union",
        detect = function(t)
            return re(t, "%Wunion%s")
        end,
    },

    -- INTERSECT
    {
        tag = "intersect",
        detect = function(t)
            return re(t, "%Wintersect%s") or re(t, "%Wintersect%)")
        end,
    },

    -- EXCEPT
    {
        tag = "except",
        detect = function(t)
            return re(t, "%Wexcept%s") or re(t, "%Wexcept%)")
        end,
    },

    -- VALUES clause (standalone or in INSERT)
    {
        tag = "values",
        detect = function(t)
            return re(t, "%Wvalues%s*%(")
        end,
    },

    -- INSERT
    {
        tag = "insert",
        detect = function(t)
            return re(t, "%Winsert%s+into%s") or re(t, "%Wreplace%s+into%s")
        end,
    },

    -- UPDATE
    {
        tag = "update",
        detect = function(t)
            return re(t, "%Wupdate%s+%w") and re(t, "%Wset%s+%w")
        end,
    },

    -- DELETE
    {
        tag = "delete",
        detect = function(t)
            return re(t, "%Wdelete%s+from%s")
        end,
    },

    -- REPLACE (conflict resolution form, as separate DML)
    {
        tag = "replace",
        detect = function(t)
            return re(t, "replace%s+into%s") or re(t, "insert%s+or%s+replace%s")
        end,
    },

    -- CREATE/DROP TRIGGER
    {
        tag = "trigger",
        detect = function(t)
            return re(t, "create%s+trigger%s") or re(t, "drop%s+trigger%s")
        end,
    },

    -- CREATE/DROP VIEW
    {
        tag = "view",
        detect = function(t)
            return re(t, "create%s+view%s") or re(t, "drop%s+view%s")
        end,
    },

    -- CREATE TABLE / DDL operations
    {
        tag = "ddl",
        detect = function(t)
            return re(t, "create%s+table%s") or re(t, "drop%s+table%s")
                or re(t, "alter%s+table%s") or re(t, "create%s+index%s")
                or re(t, "drop%s+index%s")
        end,
    },

    -- EXPLAIN / EXPLAIN QUERY PLAN
    {
        tag = "explain",
        detect = function(t)
            return re(t, "explain%s+query%s+plan%s") or re(t, "\"explain%s") or re(t, "'explain%s")
                or re(t, "%[%[explain%s")
        end,
    },

    -- ANALYZE statement
    {
        tag = "analyze",
        detect = function(t)
            return re(t, "%Wanalyze%s")
        end,
    },

    -- CHECK constraints
    {
        tag = "check_constraint",
        detect = function(t)
            return re(t, "%Wcheck%s*%(")
        end,
    },

    -- Foreign key constraints
    {
        tag = "foreign_key",
        detect = function(t)
            return re(t, "foreign%s+key%s") or re(t, "references%s+%w")
        end,
    },

    -- Nondeterministic: uses random() or randomblob() without seeding
    -- Flag any file that calls random() — conservative (random_seed() would clear it)
    {
        tag = "nondeterministic",
        detect = function(t)
            return re(t, "%Wrandom%s*%(") or re(t, "%Wrandomblob%s*%(")
        end,
    },

    -- Transaction control: BEGIN/COMMIT/ROLLBACK/SAVEPOINT
    {
        tag = "transaction",
        detect = function(t)
            return re(t, "%Wbegin%s") or re(t, "%Wcommit%s") or re(t, "%Wrollback%s")
                or re(t, "%Wsavepoint%s")
        end,
    },

    -- Index hints: INDEXED BY / NOT INDEXED
    {
        tag = "index_hint",
        detect = function(t)
            return re(t, "indexed%s+by%s") or re(t, "not%s+indexed%s")
        end,
    },

    -- Type casting / CAST expressions
    {
        tag = "cast",
        detect = function(t)
            return re(t, "%Wcast%s*%(")
        end,
    },

    -- Collation: COLLATE keyword
    {
        tag = "collation",
        detect = function(t)
            return re(t, "%Wcollate%s")
        end,
    },
}

-- ---------------------------------------------------------------------------
-- Classify a single file
-- ---------------------------------------------------------------------------

local function classify_file(suite_name, file_name, file_path, disabled_set)
    local content = read_file(file_path)
    if not content then
        return {
            suite       = suite_name,
            file        = file_name,
            disabled    = false,
            error       = "unreadable",
            feature_tags = {},
        }
    end

    -- Detect feature tags
    local tags = {}
    for _, rule in ipairs(TAG_RULES) do
        local ok, result = pcall(rule.detect, content)
        if ok and result then
            tags[#tags + 1] = rule.tag
        end
    end
    table.sort(tags)

    local entry = {
        suite        = suite_name,
        file         = file_name,
        feature_tags = tags,
    }

    -- Disabled status
    if disabled_set[file_name] then
        entry.disabled = true
    end

    -- Nondeterministic is both a tag and a flag
    -- (the tag is already in feature_tags; add explicit flag for harness use)
    for _, t in ipairs(tags) do
        if t == "nondeterministic" then
            entry.nondeterministic = true
            break
        end
    end

    return entry
end

-- ---------------------------------------------------------------------------
-- Main
-- ---------------------------------------------------------------------------

local function main()
    io.write("classify.lua: scanning test suites...\n")

    local all_entries = {}
    local tag_counts = {}  -- for statistics

    for _, suite in ipairs(SUITES) do
        local ini_path = suite.dir .. "/suite.ini"
        local disabled = parse_disabled(ini_path)

        local files = list_files(suite.dir, suite.pattern)
        io.write(string.format("  %s: %d test files found\n", suite.name, #files))

        for _, fname in ipairs(files) do
            local fpath = suite.dir .. "/" .. fname
            local entry = classify_file(suite.name, fname, fpath, disabled)
            all_entries[#all_entries + 1] = entry

            -- Accumulate tag frequency
            if entry.feature_tags then
                for _, tag in ipairs(entry.feature_tags) do
                    tag_counts[tag] = (tag_counts[tag] or 0) + 1
                end
            end
        end
    end

    -- Build YAML document
    -- Top-level structure: { classification_version, generated_at, tests: [...] }
    local doc = {
        classification_version = 1,
        generated_at = os.date("!%Y-%m-%dT%H:%M:%SZ"),
        total_tests = #all_entries,
        tests = all_entries,
    }

    -- Write output: header comments first, then canonical YAML body via the
    -- shared emitter (M.emit_nodoc skips the leading "---" so the file stays
    -- a clean YAML stream after the comment block).
    local f, err = io.open(OUTPUT_FILE, "w")
    if not f then
        error("Cannot open output file: " .. tostring(err))
    end
    f:write("# Parity corpus classification — generated by classify.lua\n")
    f:write("# DO NOT EDIT manually — re-run classify.lua to regenerate.\n")
    f:write("# Schema: test/sql-baselines/SCHEMA.md\n")
    f:write("#\n")
    f:write(yaml.emit_nodoc(doc))
    f:close()

    -- Print tag frequency summary
    io.write("\nTag frequency distribution:\n")
    -- Sort by count descending
    local sorted_tags = {}
    for tag, count in pairs(tag_counts) do
        sorted_tags[#sorted_tags + 1] = { tag = tag, count = count }
    end
    table.sort(sorted_tags, function(a, b)
        if a.count ~= b.count then return a.count > b.count end
        return a.tag < b.tag
    end)
    for _, item in ipairs(sorted_tags) do
        io.write(string.format("  %-30s  %d\n", item.tag, item.count))
    end

    io.write(string.format("\nWrote %s (%d entries)\n", OUTPUT_FILE, #all_entries))
end

local ok, err = pcall(main)
if not ok then
    io.stderr:write("ERROR: " .. tostring(err) .. "\n")
    os.exit(1)
end
os.exit(0)

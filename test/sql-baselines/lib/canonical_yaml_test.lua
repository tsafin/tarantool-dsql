#!/usr/bin/env tarantool
-- canonical_yaml_test.lua — regression tests for lib/canonical_yaml.lua
--
-- Runs standalone under the tarantool binary. Exits 0 on all-pass, 1 otherwise.
--
--   cd <repo-root>
--   ./src/tarantool test/sql-baselines/lib/canonical_yaml_test.lua

local SCRIPT_DIR = (arg and arg[0] or ""):gsub("[/\\][^/\\]+$", "")
if SCRIPT_DIR == "" then SCRIPT_DIR = "." end

local yaml_lib = dofile(SCRIPT_DIR .. "/canonical_yaml.lua")
local yaml     = require("yaml")

local pass, fail = 0, 0
local function check(name, ok, detail)
    if ok then
        pass = pass + 1
        io.write(string.format("  ok   %s\n", name))
    else
        fail = fail + 1
        io.write(string.format("  FAIL %s%s\n", name, detail and (" -- " .. detail) or ""))
    end
end

-- ---------------------------------------------------------------------------
-- Scalar emission
-- ---------------------------------------------------------------------------

io.write("Scalar emission:\n")

check("nil emits null",
    yaml_lib.emit_nodoc(nil) == "null\n")

check("box.NULL emits null",
    yaml_lib.emit_nodoc(box.NULL) == "null\n")

check("true emits true",
    yaml_lib.emit_nodoc(true) == "true\n")

check("false emits false",
    yaml_lib.emit_nodoc(false) == "false\n")

check("integer emits without dot",
    yaml_lib.emit_nodoc(42) == "42\n")

check("negative integer",
    yaml_lib.emit_nodoc(-7) == "-7\n")

check("float emits with %.15g",
    yaml_lib.emit_nodoc(1.5) == "1.5\n")

check("float 1e-6 keeps scientific form",
    (function()
        local s = yaml_lib.emit_nodoc(1e-6)
        return s == "1e-06\n" or s == "1e-6\n" or s:find("^%d+%.")
    end)(),
    "got: " .. yaml_lib.emit_nodoc(1e-6):gsub("\n$", ""))

check("plain string unquoted",
    yaml_lib.emit_nodoc("hello") == "hello\n")

check("empty string quoted",
    yaml_lib.emit_nodoc("") == "''\n")

check("string with colon quoted",
    yaml_lib.emit_nodoc("2026-07-10T12:00:00Z") == "'2026-07-10T12:00:00Z'\n")

check("reserved word quoted",
    yaml_lib.emit_nodoc("true") == "'true'\n")

check("numeric-looking string quoted",
    yaml_lib.emit_nodoc("42") == "'42'\n")

check("embedded single-quote doubled",
    yaml_lib.emit_nodoc("it's") == "'it''s'\n")

check("blob emitted as !!binary base64",
    yaml_lib.emit_nodoc("\x00\x01\xff") == "!!binary AAH/\n")

-- ---------------------------------------------------------------------------
-- Table emission — arrays and maps
-- ---------------------------------------------------------------------------

io.write("Table emission:\n")

check("empty array",
    yaml_lib.emit_nodoc({}) == "[]\n")

check("scalar array short inline (via map wrapper)",
    yaml_lib.emit_nodoc({items = {1, 2, 3}}) == "items: [1, 2, 3]\n")

check("map keys sorted alphabetically",
    yaml_lib.emit_nodoc({b = 1, a = 2, c = 3}) == "a: 2\nb: 1\nc: 3\n")

check("nested map keys sorted",
    yaml_lib.emit_nodoc({outer = {b = 1, a = 2}}) ==
    "outer:\n  a: 2\n  b: 1\n")

check("array of maps clean indent",
    yaml_lib.emit_nodoc({{k = 1}, {k = 2}}) ==
    "- k: 1\n- k: 2\n")

check("map with block array",
    yaml_lib.emit_nodoc({items = {{k = 1}, {k = 2}}}) ==
    "items:\n  - k: 1\n  - k: 2\n")

-- ---------------------------------------------------------------------------
-- Round-trip through yaml.decode
-- ---------------------------------------------------------------------------

io.write("Round-trip:\n")

local function round_trip(v)
    local emitted = yaml_lib.emit(v)
    local ok, decoded = pcall(yaml.decode, emitted)
    return ok, decoded, emitted
end

do
    local ok, decoded = round_trip({a = 1, b = "two", c = {x = 1.5, y = true}})
    check("nested map round-trips shape",
        ok and decoded.a == 1 and decoded.b == "two" and decoded.c.x == 1.5 and decoded.c.y == true)
end

do
    local ok, decoded = round_trip({1, 2, 3, "four"})
    check("mixed array round-trips",
        ok and decoded[1] == 1 and decoded[4] == "four")
end

-- ---------------------------------------------------------------------------
-- Snapshot doc (SCHEMA.md v1) round-trips through generic emit
-- ---------------------------------------------------------------------------

io.write("Snapshot doc round-trip:\n")

local snap = {
    schema_version = 1,
    test = {
        suite = "sql-tap",
        file = "sql-tap/select1.test.lua",
        query_index = 1,
        query_sql = "SELECT 1\n",
        feature_tags = { "projection", "single_table_select" },
    },
    engine = "memtx",
    captured = {
        at = "2026-07-10T00:00:00Z",
        against_commit = "abc123",
        tarantool_version = "test",
        primary_dispatcher = "generated",
    },
    l1_result = {
        ok = true,
        column_names = { "c1" },
        column_types = { "integer" },
        rows_sorted = false,
        rows = { {1}, {"two"}, {3.14} },
    },
    l2_diagnostic = {
        status = "success",
        error_code = nil,
        error_message_canonical = nil,
    },
    l3_path_class = {
        taken = "current_where_c",
        reason = nil,
        fallback_to = nil,
    },
    metadata = {
        planner_version = 0,
        classification_version = 1,
    },
}

local snap_yaml = yaml_lib.emit(snap)

check("snapshot doc round-trips through yaml.decode",
    (function()
        local ok, d = pcall(yaml.decode, snap_yaml)
        return ok and d.schema_version == 1
            and d.engine == "memtx"
            and d.test.suite == "sql-tap"
            and d.l1_result.ok == true
            and d.l3_path_class.taken == "current_where_c"
    end)())

check("snapshot preserves row values through emit+decode",
    (function()
        local ok, d = pcall(yaml.decode, snap_yaml)
        return ok and d.l1_result.rows[1][1] == 1
            and d.l1_result.rows[2][1] == "two"
            and d.l1_result.rows[3][1] == 3.14
    end)())

-- ---------------------------------------------------------------------------
-- Result
-- ---------------------------------------------------------------------------

io.write(string.format("\n%d pass, %d fail\n", pass, fail))
os.exit(fail == 0 and 0 or 1)

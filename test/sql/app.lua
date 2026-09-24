#!/usr/bin/env tarantool

-- SQL parity capture uses the normal test-run server so tests that depend on
-- test_run keep their original inspector and console semantics. Ordinary runs
-- leave this hook unset and follow the exact existing startup path.
local baseline_hook = os.getenv('SQL_BASELINE_HOOK')
if baseline_hook ~= nil then
    assert(baseline_hook:sub(1, 1) == '/', 'SQL_BASELINE_HOOK must be absolute')
    dofile(baseline_hook)
end

box.cfg{
    listen              = os.getenv("LISTEN"),
    pid_file            = "tarantool.pid",
    memtx_max_tuple_size = 5 * 1024 * 1024,
    vinyl_max_tuple_size = 5 * 1024 * 1024,
}

require('fiber').set_max_slice(100500)
require('console').listen(os.getenv('ADMIN'))

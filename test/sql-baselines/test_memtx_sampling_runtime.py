#!/usr/bin/env python3
"""Exercise engine-dispatched memtx sampling in a real active transaction."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("TARANTOOL_BINARY", ROOT / "build/src/tarantool"))
BUILD_DIR = Path(os.environ.get("TARANTOOL_BUILD_DIR", BINARY.parent.parent))
MODULE_DIR = BUILD_DIR / "test/box"


class MemtxSamplingRuntimeTest(unittest.TestCase):
    def test_engine_dispatch_transaction_limits_and_errors(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        if not (MODULE_DIR / "sql_stats_sample_memtx.so").is_file():
            self.skipTest(f"Memtx sampler test module not found: {MODULE_DIR}")
        lua = f"""
box.cfg{{log_level = 0}}
package.cpath = '{MODULE_DIR}/?.so;' .. package.cpath
local sample = require('sql_stats_sample_memtx').sample
local space = box.schema.space.create('sample_memtx', {{engine = 'memtx'}})
local primary = space:create_index('pk', {{type = 'hash'}})
local function rejected(result, code)
    assert(result.rc == -1 and result.code == code)
    assert(result.rows == 0 and result.bytes == 0)
    assert(result.delivered == 0 and result.delivered_bytes == 0)
end
rejected(sample(space.id, 3, 1024, 1), box.error.UNSUPPORTED)
box.begin()
local tuple = space:insert{{1, 'payload'}}
local size = tuple:bsize()
local function accepted(result, count, population)
    assert(result.rc == 0 and result.code == 0)
    assert(result.rows == count and result.delivered == count)
    assert(result.population_known and result.population == population)
    assert(result.bytes == count * size)
    assert(result.bytes == result.delivered_bytes)
    assert(result.with_replacement and result.fields_match)
end
-- Uncommitted tuples must be visible in the caller's transaction; repeated
-- draws are intentional and count toward both limits.
local rows = sample(space.id, 3, 1024, 7)
accepted(rows, 3, 1)
for _, id in ipairs(rows.ids) do assert(id == 1) end
accepted(sample(space.id, 20, 2 * size, 7), 2, 1)
accepted(sample(space.id, 20, size - 1, 7), 0, 1)
for _, invalid in ipairs{{'fields', 'sink', 'request'}} do
    rejected(sample(space.id, 3, 1024, 7, invalid), box.error.ILLEGAL_PARAMS)
end
rejected(sample(space.id, 0, 1024, 7), box.error.ILLEGAL_PARAMS)
rejected(sample(space.id, 3, 0, 7), box.error.ILLEGAL_PARAMS)
rejected(sample(4294967295, 3, 1024, 7), box.error.ILLEGAL_PARAMS)
box.rollback()
assert(space:count() == 0)
box.begin()
accepted(sample(space.id, 3, 1024, 7), 0, 0)
box.rollback()
for i = 1, 3 do space:insert{{i, 'payload'}} end
local secondary = space:create_index('by_payload', {{
    parts = {{{{field = 2, type = 'string'}}}}, unique = false,
}})
box.begin()
local first = sample(space.id, 12, 1024, 41)
local repeat_draw = sample(space.id, 12, 1024, 41)
accepted(first, 12, 3)
accepted(repeat_draw, 12, 3)
for i, id in ipairs(first.ids) do
    assert(id >= 1 and id <= 3 and repeat_draw.ids[i] == id)
end
local secondary_draw = sample(space.id, 12, 1024, 41, nil, secondary.id)
accepted(secondary_draw, 12, 3)
for _, id in ipairs(secondary_draw.ids) do assert(id >= 1 and id <= 3) end
box.rollback()
local no_pk = box.schema.space.create('sample_no_pk', {{engine = 'memtx'}})
box.begin()
rejected(sample(no_pk.id, 3, 1024, 1), box.error.UNSUPPORTED)
box.rollback()
no_pk:drop()
assert(primary.id == 0)
space:drop()
os.exit(0)
"""
        with tempfile.TemporaryDirectory(prefix="memtx-sampling-runtime-") as temp:
            subprocess.run([str(BINARY), "-e", lua], cwd=temp, check=True,
                           timeout=30, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()

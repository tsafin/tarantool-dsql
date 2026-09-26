#!/usr/bin/env python3
"""Exercise bounded, fail-closed Vinyl SQL statistics sampling."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("TARANTOOL_BINARY", ROOT / "build/src/tarantool"))
BUILD_DIR = Path(os.environ.get("TARANTOOL_BUILD_DIR", BINARY.parent.parent))
MODULE_DIR = BUILD_DIR / "test/box"
MODULE = MODULE_DIR / "sql_stats_sample_vinyl.so"


class VinylSamplingGuardTest(unittest.TestCase):
    def test_bounded_reservoir_and_work_budget_failures(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        if not MODULE.is_file():
            self.skipTest(f"Vinyl sampler test module not found: {MODULE}")

        lua = f"""
box.cfg{{listen = 0, vinyl_read_threads = 2}}
package.cpath = '{MODULE_DIR}/?.so;' .. package.cpath
local sample = require('sql_stats_sample_vinyl')
local space = box.schema.space.create('sql_stats_sample_vinyl', {{
    engine = 'vinyl',
}})
space:create_index('pk')
for id = 1, 64 do space:insert{{id, 'initial'}} end
box.snapshot()
space.index.pk:compact()
local function take(rows, bytes, seed, tuples, sources, pages, keys, buffer)
    return sample.sample(space.id, rows or 8, bytes or 1024 * 1024,
        seed or 1, tuples or 1024, sources or 1024, pages or 1024,
        keys or 1024, buffer or 1024 * 1024)
end
local function rejected(result)
    assert(result.rc == -1 and result.code ~= 0)
    assert(result.rows == 0 and result.delivered == 0)
    assert(not result.population_known)
end
-- The caller's transaction view and write set are used, like an ordinary
-- Vinyl index iterator; rollback leaves the committed population unchanged.
box.begin()
space:insert{{65, 'uncommitted'}}
local transactional = take(128, 1024 * 1024, 43)
assert(transactional.rc == 0 and transactional.population == 65)
local found_own_write = false
for _, id in ipairs(transactional.ids) do
    if id == 65 then found_own_write = true end
end
assert(found_own_write)
box.rollback()
-- Each source/page/key/visible-tuple/buffer/payload limit is fail-closed.
rejected(take(8, 1024 * 1024, 1, 1024, 0, 1024, 1024, 1024 * 1024))
rejected(take(8, 1024 * 1024, 1, 1024, 1024, 0, 1024, 1024 * 1024))
rejected(take(8, 1024 * 1024, 1, 1024, 1024, 1024, 1, 1024 * 1024))
rejected(take(8, 1024 * 1024, 1, 2, 1024, 1024, 1024, 1024 * 1024))
rejected(take(8, 1, 1, 1024, 1024, 1024, 1024, 1024 * 1024))
rejected(take(8, 1024 * 1024, 1, 1024, 1024, 1024, 1024, 1))
-- Successful sampling observes EOF, reports the complete visible population,
-- and selects without replacement from the primary-index order.
local result = take(8, 1024 * 1024, 42)
assert(result.rc == 0 and result.code == 0)
assert(result.population_known and result.population == 64)
assert(result.rows == 8 and result.delivered == 8)
assert(not result.with_replacement)
local ids = {{}}
for _, id in ipairs(result.ids) do
    assert(id >= 1 and id <= 64 and not ids[id])
    ids[id] = true
end
local same = take(8, 1024 * 1024, 42)
for i, id in ipairs(result.ids) do assert(same.ids[i] == id) end
-- A deterministic seed sweep should not collapse to the first keys. The
-- loose bounds make this a quality smoke test, not a statistical proof.
local frequency = {{}}
for i = 1, 64 do frequency[i] = 0 end
for seed = 1, 512 do
    local draw = take(1, 1024 * 1024, seed)
    assert(draw.rc == 0 and draw.population == 64 and draw.rows == 1)
    frequency[draw.ids[1]] = frequency[draw.ids[1]] + 1
end
for id = 1, 64 do assert(frequency[id] >= 1 and frequency[id] <= 20) end
-- Update/delete and another compaction must be reflected by a new snapshot.
space:update({{2}}, {{'=', 2, 'updated'}})
space:delete({{3}})
box.snapshot()
space.index.pk:compact()
local changed = take(64, 1024 * 1024, 11)
assert(changed.rc == 0 and changed.population == 63 and changed.rows == 63)
local visible = {{}}
for _, id in ipairs(changed.ids) do visible[id] = true end
assert(visible[2] and not visible[3])
space:drop()
os.exit(0)
"""
        with tempfile.TemporaryDirectory(prefix="vinyl-sampling-guard-") as temp:
            subprocess.run([str(BINARY), "-e", lua], cwd=temp, check=True,
                           stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Vinyl SQL statistics sampling fails closed until bounded sampling exists."""

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
    def test_vinyl_sampling_is_unsupported_without_delivering_rows(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        if not MODULE.is_file():
            self.skipTest(f"Vinyl sampler test module not found: {MODULE}")

        lua = f"""
box.cfg{{listen = 0}}
package.cpath = '{MODULE_DIR}/?.so;' .. package.cpath
local sample = require('sql_stats_sample_vinyl')
local space = box.schema.space.create('sql_stats_sample_vinyl', {{
    engine = 'vinyl',
}})
space:create_index('pk')
space:insert({{1}})
local rc, code, rows, delivered = sample.sample(space.id)
assert(rc == -1)
assert(code == box.error.UNSUPPORTED)
assert(rows == 0)
assert(delivered == 0)
space:drop()
os.exit(0)
"""
        with tempfile.TemporaryDirectory(prefix="vinyl-sampling-guard-") as temp:
            subprocess.run([str(BINARY), "-e", lua], cwd=temp, check=True,
                           stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()

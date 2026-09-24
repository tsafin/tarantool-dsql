#!/usr/bin/env python3
"""Regression checks for the fail-closed SQL snapshot comparison contract."""

import os
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
DIFF = ROOT / "test/sql-baselines/diff.lua"
BINARY = Path(os.environ.get("TARANTOOL_BINARY", ROOT / "build/src/tarantool"))


def snapshot(sql="SELECT 1", cell="1", col_type="integer"):
    return f"""---
schema_version: 1
engine: memtx
test:
  suite: sql-tap
  file: sql-tap/example.test.lua
  query_index: 1
  query_sql: '{sql}'
captured:
  tarantool_version: test
l1_result:
  ok: true
  rows_sorted: true
  rows:
    - [{cell}]
  column_names: [x]
  column_types: [{col_type}]
l2_diagnostic:
  status: success
l3_path_class:
  taken: current_where_c
"""


class SnapshotDiffTest(unittest.TestCase):
    def setUp(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        self.temp = tempfile.TemporaryDirectory(prefix="sql-diff-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name) / "base"
        self.candidate = Path(self.temp.name) / "candidate"
        self.base.mkdir()
        self.candidate.mkdir()
        self.write(snapshot(), snapshot())

    def write(self, base, candidate):
        (self.base / "q01.memtx.yaml").write_text(base)
        (self.candidate / "q01.memtx.yaml").write_text(candidate)

    def diff(self):
        return subprocess.run(
            [str(BINARY), str(DIFF), str(self.base), str(self.candidate),
             "--format=json"], capture_output=True, text=True, check=False)

    def test_identical(self):
        self.assertEqual(self.diff().returncode, 0)

    def test_sql_identity_is_hard_gate(self):
        self.write(snapshot(), snapshot(sql="SELECT 2"))
        result = self.diff()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("test.query_sql", result.stdout)

    def test_row_type_is_hard_gate(self):
        self.write(snapshot(), snapshot(cell="'1'"))
        result = self.diff()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("l1_result.rows", result.stdout)
        fields = json.loads(result.stdout)["diffs"][0]["fields"]
        rows = next(field for field in fields if field["name"] == "l1_result.rows")
        self.assertEqual(rows["baseline"], [[1]])
        self.assertEqual(rows["candidate"], [["1"]])

    def test_column_type_is_hard_gate(self):
        self.write(snapshot(), snapshot(col_type="string"))
        self.assertEqual(self.diff().returncode, 1)

    def test_missing_snapshot_is_hard_gate(self):
        (self.candidate / "q01.memtx.yaml").unlink()
        (self.candidate / "q02.memtx.yaml").write_text(snapshot())
        self.assertEqual(self.diff().returncode, 1)

    def test_malformed_snapshot_fails_closed(self):
        self.write(snapshot(), "---\ninvalid: true\n")
        self.assertEqual(self.diff().returncode, 2)


if __name__ == "__main__":
    unittest.main()

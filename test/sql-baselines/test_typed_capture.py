#!/usr/bin/env python3
"""End-to-end typed-value capture regression."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("TARANTOOL_BINARY", ROOT / "build/src/tarantool"))
HARNESS = ROOT / "test/sql-baselines/harness/run.lua"
FIXTURE = ROOT / "test/sql-baselines/harness/fixtures/typed_sql.test.lua"
VALIDATE = ROOT / "test/sql-baselines/validate.lua"


class TypedCaptureTest(unittest.TestCase):
    def test_extended_values_and_containers(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        with tempfile.TemporaryDirectory(prefix="sql-typed-capture-") as temp:
            temp = Path(temp)
            work = temp / "work"
            work.mkdir()
            out = temp / "capture"
            env = os.environ.copy()
            env["VDBE_DISPATCHER"] = "generated"
            env["SQL_JIT_ENABLE"] = "0"
            subprocess.run([str(BINARY), str(HARNESS), str(FIXTURE),
                            "--suite=sql-tap", "--engine=memtx",
                            f"--out={out}", f"--work-dir={work}"],
                           check=True, env=env, stdout=subprocess.DEVNULL)
            subprocess.run([str(BINARY), str(VALIDATE), str(out)],
                           check=True, stdout=subprocess.DEVNULL)
            snapshot = (out / "snapshots/sql-tap/typed_sql/q01.memtx.yaml").read_text()
            self.assertIn("sql_type: decimal", snapshot)
            self.assertIn("sql_type: datetime", snapshot)
            self.assertIn("sql_type: array", snapshot)
            self.assertIn("sql_type: map", snapshot)
            self.assertIn("taken: current_where_c", snapshot)
            self.assertNotIn("table: 0x", snapshot)

    def test_forensic_vdbe_program_listing(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        with tempfile.TemporaryDirectory(prefix="sql-forensic-capture-") as temp:
            temp = Path(temp)
            work = temp / "work"
            work.mkdir()
            out = temp / "capture"
            env = os.environ.copy()
            env["VDBE_DISPATCHER"] = "generated"
            env["SQL_JIT_ENABLE"] = "0"
            subprocess.run([str(BINARY), str(HARNESS), str(FIXTURE),
                            "--suite=sql-tap", "--engine=memtx",
                            f"--out={out}", f"--work-dir={work}", "--forensic"],
                           check=True, env=env, stdout=subprocess.DEVNULL)
            trace = out / "forensics/sql-tap/typed_sql/q01.memtx.generated.trace-query"
            contents = trace.read_text()
            self.assertIn("static program listing from SQL EXPLAIN", contents)
            self.assertIn("not a dynamic execution trace", contents)
            self.assertIn(",Init,", contents)
            self.assertIn("EXPLAIN_TEXT", contents)
            self.assertNotIn("PLACEHOLDER", contents)


if __name__ == "__main__":
    unittest.main()

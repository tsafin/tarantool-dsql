#!/usr/bin/env python3
"""End-to-end typed-value capture regression."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("TARANTOOL_BINARY", ROOT / "build/src/tarantool"))
HARNESS = ROOT / "test/sql-baselines/harness/run.lua"
FIXTURE = ROOT / "test/sql-baselines/harness/fixtures/typed_sql.test.lua"
FALLBACK_FIXTURE = ROOT / "test/sql-baselines/harness/fixtures/fallback_sql.test.lua"
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

    def test_fallback_reason_survives_sql_snapshot_capture(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        with tempfile.TemporaryDirectory(prefix="sql-fallback-capture-") as temp:
            temp = Path(temp)
            work = temp / "work"
            work.mkdir()
            out = temp / "capture"
            env = os.environ.copy()
            env["VDBE_DISPATCHER"] = "generated"
            env["SQL_JIT_ENABLE"] = "0"
            subprocess.run([str(BINARY), str(HARNESS), str(FALLBACK_FIXTURE),
                            "--suite=sql-tap", "--engine=memtx",
                            f"--out={out}", f"--work-dir={work}"],
                           check=True, env=env, stdout=subprocess.DEVNULL)
            subprocess.run([str(BINARY), str(VALIDATE), str(out)],
                           check=True, stdout=subprocess.DEVNULL)
            snapshot = (out / "snapshots/sql-tap/fallback_sql/q01.memtx.yaml").read_text()
            self.assertIn("taken: fallback", snapshot)
            self.assertIn("reason: UNSUPPORTED_RELATION_COUNT", snapshot)
            self.assertIn("fallback_to: current_where_c", snapshot)
            manifest = json.loads((out / "manifests/sql-tap/fallback_sql.memtx.json").read_text())
            self.assertEqual(manifest["planner_metrics_version"], 2)
            self.assertEqual(manifest["planner_metrics"][0]["path_class"], "fallback")
            self.assertEqual(manifest["planner_metrics"][0]["fallback_count"], 1)
            self.assertGreater(manifest["planner_metrics"][0]["generated"], 0)
            self.assertGreater(manifest["planner_metrics"][0]["retained"], 0)
            for metric in ("generated", "dominated", "truncated", "retained"):
                self.assertIn(metric, manifest["planner_metrics"][0])
            for seq, reason in enumerate((
                    "UNSUPPORTED_AGGREGATE", "UNSUPPORTED_COMPOUND",
                    "UNSUPPORTED_DISTINCT", "UNSUPPORTED_SUBQUERY",
                    "UNSUPPORTED_CTE", "UNSUPPORTED_AGGREGATE"), start=2):
                snapshot = (out / f"snapshots/sql-tap/fallback_sql/q{seq:02d}.memtx.yaml").read_text()
                self.assertIn("taken: fallback", snapshot)
                self.assertIn(f"reason: {reason}", snapshot)
                self.assertIn("fallback_to: current_where_c", snapshot)
                metrics = manifest["planner_metrics"][seq - 1]
                self.assertEqual(metrics["path_class"], "fallback")
                self.assertEqual(metrics["fallback_count"], 1)

            path = out / "snapshots/sql-tap/fallback_sql/q01.memtx.yaml"
            valid_snapshot = path.read_text()
            invalid_fields = (
                ("reason: UNSUPPORTED_RELATION_COUNT", "reason: null"),
                ("reason: UNSUPPORTED_RELATION_COUNT", "reason: UNKNOWN_REASON"),
                ("fallback_to: current_where_c", "fallback_to: new_planner"),
                ("taken: fallback", "taken: current_where_c"),
            )
            for original, replacement in invalid_fields:
                with self.subTest(replacement=replacement):
                    self.assertIn(original, valid_snapshot)
                    path.write_text(valid_snapshot.replace(original, replacement, 1))
                    result = subprocess.run(
                        [str(BINARY), str(VALIDATE), str(out)],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True)
                    self.assertNotEqual(result.returncode, 0,
                                        "validator accepted inconsistent path metadata")
            encoded_fallback = valid_snapshot.replace(
                "taken: fallback", "taken: fallback_UNSUPPORTED_RELATION_COUNT", 1)
            encoded_fallback = encoded_fallback.replace(
                "reason: UNSUPPORTED_RELATION_COUNT", "reason: null", 1)
            path.write_text(encoded_fallback)
            result = subprocess.run(
                [str(BINARY), str(VALIDATE), str(out)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertNotEqual(result.returncode, 0,
                                "validator accepted encoded fallback without reason")
            path.write_text(valid_snapshot)

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

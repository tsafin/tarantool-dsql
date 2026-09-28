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
            self.assertIn("taken: fallback", snapshot)
            self.assertIn("reason: UNSUPPORTED_RELATION_COUNT", snapshot)
            self.assertIn("fallback_to: current_where_c", snapshot)
            self.assertNotIn("table: 0x", snapshot)
            rejected_select = (out / "snapshots/sql-tap/typed_sql/q02.memtx.yaml").read_text()
            self.assertIn("status: error", rejected_select)
            self.assertIn("taken: null", rejected_select)
            no_planner_path = (out / "snapshots/sql-tap/typed_sql/q03.memtx.yaml").read_text()
            self.assertIn("taken: null", no_planner_path)
            manifest = json.loads((out / "manifests/sql-tap/typed_sql.memtx.json").read_text())
            self.assertEqual(manifest["captured_queries"], 3)
            self.assertEqual(len(manifest["planner_metrics"]), 1)
            self.assertEqual(manifest["planner_metrics"][0]["path_class"], "fallback")
            self.assertEqual(manifest["planner_metrics"][0]["fallback_count"], 1)

    def test_fixed_planner_flag_capture(self):
        if not BINARY.is_file():
            self.skipTest(f"Tarantool binary not found: {BINARY}")
        with tempfile.TemporaryDirectory(prefix="sql-planner-flag-capture-") as temp:
            temp = Path(temp)
            work = temp / "work"
            work.mkdir()
            out = temp / "capture"
            env = os.environ.copy()
            env["VDBE_DISPATCHER"] = "generated"
            env["SQL_JIT_ENABLE"] = "0"
            subprocess.run([str(BINARY), str(HARNESS), str(FIXTURE),
                            "--suite=sql-tap", "--engine=memtx",
                            "--planner-flag=on", f"--out={out}",
                            f"--work-dir={work}"],
                           check=True, env=env, stdout=subprocess.DEVNULL)
            manifest = json.loads((out / "manifests/sql-tap/typed_sql.memtx.json").read_text())
            self.assertEqual(manifest["planner_flag"], "on")
            self.assertTrue(manifest["accepted"])

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
            self.assertEqual(manifest["component_ledger_version"], 1)
            self.assertEqual(manifest["captured_queries"], 15)
            self.assertEqual(len(manifest["planner_metrics"]), 12)
            self.assertEqual(manifest["planner_metrics"][0]["path_class"], "fallback")
            self.assertEqual(manifest["planner_metrics"][0]["fallback_count"], 1)
            self.assertGreater(manifest["planner_metrics"][0]["generated"], 0)
            self.assertGreater(manifest["planner_metrics"][0]["retained"], 0)
            for metric in ("generated", "dominated", "truncated", "retained"):
                self.assertIn(metric, manifest["planner_metrics"][0])
            expected_routes = (
                ("fallback", "UNSUPPORTED_AGGREGATE"),
                ("mixed", None),
                ("fallback", "UNSUPPORTED_DISTINCT"),
                ("fallback", "UNSUPPORTED_SUBQUERY"),
                ("fallback", "UNSUPPORTED_CTE"),
                ("fallback", "UNSUPPORTED_AGGREGATE"),
                ("current_where_c", None),
                ("direct_values", None),
                ("direct_op_count", None),
            )
            for seq, (route, reason) in enumerate(expected_routes, start=2):
                snapshot = (out / f"snapshots/sql-tap/fallback_sql/q{seq:02d}.memtx.yaml").read_text()
                metrics = manifest["planner_metrics"][seq - 1]
                self.assertEqual(metrics["path_class"], route)
                self.assertEqual(metrics["fallback_reason"], reason)
                self.assertEqual(metrics["component_status"], "complete")
                self.assertGreater(len(metrics["component_routes"]), 0)
                self.assertIn(f"taken: {route}", snapshot)
                if reason is not None:
                    self.assertIn(f"reason: {reason}", snapshot)
                    self.assertIn("fallback_to: current_where_c", snapshot)
                else:
                    self.assertNotIn("reason:", snapshot)
                    self.assertNotIn("fallback_to:", snapshot)
            enabled_routes = [
                metric for metric in manifest["planner_metrics"]
                if metric["path_class"] == "new_planner"
            ]
            self.assertTrue(enabled_routes)
            enabled_route = enabled_routes[-1]
            self.assertEqual(enabled_route["path_class"], "new_planner")
            self.assertEqual(enabled_route["fallback_reason"], None)
            self.assertEqual(enabled_route["component_status"], "complete")
            self.assertEqual(enabled_route["component_routes"][0]["route"],
                             "new_planner")

            recursive_anchors = [
                component
                for metric in manifest["planner_metrics"]
                for component in metric["component_routes"]
                if component["role"] == "recursive_anchor"
            ]
            self.assertEqual(len(recursive_anchors), 1)
            self.assertEqual(recursive_anchors[0]["route"], "direct_values")

            path = out / "snapshots/sql-tap/fallback_sql/q01.memtx.yaml"
            valid_snapshot = path.read_text()
            invalid_fields = (
                ("reason: UNSUPPORTED_RELATION_COUNT", "reason: null"),
                ("reason: UNSUPPORTED_RELATION_COUNT", "reason: UNKNOWN_REASON"),
                ("fallback_to: current_where_c", "fallback_to: new_planner"),
                ("taken: fallback", "taken: current_where_c"),
                ("taken: fallback", "taken: null"),
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

            manifest_path = out / "manifests/sql-tap/fallback_sql.memtx.json"
            valid_manifest = manifest_path.read_text()
            inconsistent_manifest = json.loads(valid_manifest)
            inconsistent_manifest["planner_metrics"][0]["fallback_reason"] = \
                "UNSUPPORTED_SUBQUERY"
            manifest_path.write_text(json.dumps(inconsistent_manifest))
            result = subprocess.run(
                [str(BINARY), str(VALIDATE), str(out)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertNotEqual(result.returncode, 0,
                                "validator accepted planner metrics that disagree with snapshot")

            invalid_component_manifest = json.loads(valid_manifest)
            invalid_component_manifest["planner_metrics"][2]["component_routes"][1]["parent_id"] = 999
            manifest_path.write_text(json.dumps(invalid_component_manifest))
            result = subprocess.run(
                [str(BINARY), str(VALIDATE), str(out)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True)
            self.assertNotEqual(result.returncode, 0,
                                "validator accepted a missing component parent")

            mismatched_summary_manifest = json.loads(valid_manifest)
            mismatched_summary_manifest["planner_metrics"][2]["path_class"] = "fallback"
            manifest_path.write_text(json.dumps(mismatched_summary_manifest))
            result = subprocess.run(
                [str(BINARY), str(VALIDATE), str(out)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True)
            self.assertNotEqual(result.returncode, 0,
                                "validator accepted summary/ledger route disagreement")

            incomplete_manifest = json.loads(valid_manifest)
            incomplete_manifest["planner_metrics"].pop(0)
            manifest_path.write_text(json.dumps(incomplete_manifest))
            result = subprocess.run(
                [str(BINARY), str(VALIDATE), str(out)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertNotEqual(result.returncode, 0,
                                "validator accepted missing SELECT planner metrics")
            manifest_path.write_text(valid_manifest)

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

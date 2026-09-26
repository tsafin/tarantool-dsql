#!/usr/bin/env python3
"""Unit checks for offline planner A/B bounds and metric comparison."""
import argparse
import unittest
import planner_ab as ab


class PlannerABTest(unittest.TestCase):
    def test_width_bounds_and_explicit_environment(self):
        self.assertEqual(ab.widths("1,8,16"), (1, 8, 16))
        for value in ("1,2", "0,5,10", "1,5,65", "one,5,10"):
            with self.assertRaises(argparse.ArgumentTypeError):
                ab.widths(value)
        env = ab.environment((1, 5, 10))
        self.assertEqual([env[key] for key in ab.WIDTH_KEYS], ["1", "5", "10"])

    def test_execution_mode_environment(self):
        expected = {
            "generated": ("generated", "0"),
            "cnp": ("cnp", "0"),
            "llvm": ("generated", "1"),
        }
        for mode, values in expected.items():
            env = ab.environment((2, 8, 16), mode)
            self.assertEqual((env["VDBE_DISPATCHER"], env["SQL_JIT_ENABLE"]), values)
        with self.assertRaises(ValueError):
            ab.environment((1, 5, 10), "unknown")

    def test_reviewed_query_budget(self):
        policy = {"included": [{"test": "sql-tap/join.test.lua", "engines": ["memtx"],
                                "evidence": {"memtx": {"captured_queries": 10}}}]}
        self.assertEqual(ab.selected(policy, ["join.test.lua"], "memtx", 10), 10)
        for engine, budget in (("vinyl", 10), ("memtx", 9)):
            with self.assertRaises(ValueError):
                ab.selected(policy, ["join.test.lua"], engine, budget)

    def test_reviewed_luatest_budget_requires_generated_evidence(self):
        policy = {"included": [{"test": "sql-luatest/base_test.lua",
                   "engines": ["memtx"], "evidence": {"memtx": {"modes": {
                       "generated": {"status": "passed", "captured_queries": 3}}}}}]}
        self.assertEqual(ab.selected_luatest(policy, ["base_test.lua"], "memtx", 3), 3)
        for engine, budget in (("vinyl", 3), ("memtx", 2)):
            with self.assertRaises(ValueError):
                ab.selected_luatest(policy, ["base_test.lua"], engine, budget)
        policy["included"][0]["evidence"]["memtx"]["modes"]["generated"]["status"] = "failed"
        with self.assertRaises(ValueError):
            ab.selected_luatest(policy, ["base_test.lua"], "memtx", 3)

    def test_reviewed_sql_budget_uses_normal_runner_audit_evidence(self):
        policy = {"included": [{"test": "sql/autoincrement.test.lua",
                   "engines": ["memtx"], "evidence": {"memtx": {"capture": {
                       "audit_status": "captured", "audit_queries": 3}}}}]}
        self.assertEqual(ab.selected_sql(policy, ["autoincrement.test.lua"],
                                         "memtx", 3), 3)
        for engine, budget in (("vinyl", 3), ("memtx", 2)):
            with self.assertRaises(ValueError):
                ab.selected_sql(policy, ["autoincrement.test.lua"], engine, budget)
        vinyl_policy = {"included": [{"test": "sql/transition.test.lua",
                         "engines": ["vinyl"], "evidence": {"vinyl": {
                             "capture": {"audit_status": "timeout"},
                             "matrix": {"status": "accepted",
                                        "mode_queries": {"generated": 44}}}}}]}
        self.assertEqual(ab.selected_sql(vinyl_policy,
                         ["transition.test.lua"], "vinyl", 44), 44)
        vinyl_policy["included"][0]["evidence"]["vinyl"]["matrix"]["status"] = "failed"
        with self.assertRaises(ValueError):
            ab.selected_sql(vinyl_policy, ["transition.test.lua"], "vinyl", 44)

    def test_full_sql_corpus_selection_tracks_engine_eligibility(self):
        import json
        from pathlib import Path
        policy = json.loads((Path(__file__).parent / "corpus.json").read_text())
        memtx = ab.full_sql_tests(policy, "memtx")
        vinyl = ab.full_sql_tests(policy, "vinyl")
        self.assertEqual(memtx, sorted(memtx))
        for engine, names in (("memtx", memtx), ("vinyl", vinyl)):
            expected = sorted(row["test"][len("sql/"):]
                              for row in policy["included"]
                              if row["test"].startswith("sql/") and
                              row["test"].endswith(".test.lua") and
                              engine in row["engines"])
            self.assertEqual(names, expected)
        self.assertTrue(memtx)
        self.assertTrue(vinyl)
        self.assertTrue(all(ab.selected_sql(policy, [name], "memtx", 2**63 - 1) > 0
                            for name in memtx))

    def test_explain_output_is_not_classified_as_semantic_result_drift(self):
        from pathlib import Path
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = root / "snapshots/sql-luatest/test/q001.memtx.yaml"
            snapshot.parent.mkdir(parents=True)
            snapshot.write_text("test:\n  query_sql: 'EXPLAIN QUERY PLAN SELECT 1'\n")
            report = ab.explain_changes({"diffs": [{"query_id":
                "sql-luatest/test/q001.memtx"}]}, root, "sql-luatest")
            self.assertEqual(report["explain_output_differences"],
                             ["sql-luatest/test/q001.memtx"])
            self.assertTrue(report["semantic_result_parity"])

    def test_luatest_per_test_capture_merge_and_collision(self):
        from pathlib import Path
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, dest = root / "one", root / "all"
            (source / "snapshots/sql-luatest/a").mkdir(parents=True)
            (source / "manifests/sql-luatest").mkdir(parents=True)
            (source / "snapshots/sql-luatest/a/q001.yaml").write_text("snap")
            (source / "manifests/sql-luatest/a.memtx.json").write_text("manifest")
            (source / "luatest-child-state.json").write_text("transient")
            (source / "luatest-capture-error").write_text("transient")
            ab.merge_capture(source, dest)
            self.assertTrue((dest / "snapshots/sql-luatest/a/q001.yaml").is_file())
            self.assertFalse((dest / "luatest-child-state.json").exists())
            self.assertFalse((dest / "luatest-capture-error").exists())
            with self.assertRaises(ValueError):
                ab.merge_capture(source, dest)

    def test_elapsed_does_not_imply_structural_width_effect(self):
        row = dict.fromkeys(ab.METRICS, 1)
        row["path_class"] = "fallback"
        other = dict(row, elapsed_us=100)
        report = ab.metric_delta({"q1": row}, {"q1": other})
        self.assertFalse(report["width_effect_observed"])
        other["truncated"] += 2
        self.assertTrue(ab.metric_delta({"q1": row}, {"q1": other})["width_effect_observed"])
        with self.assertRaises(ValueError):
            ab.metric_delta({"q1": row}, {"q2": other})

    def test_full_corpus_selection_tracks_engine_eligibility(self):
        import json
        from pathlib import Path
        policy = json.loads((Path(__file__).parent / "corpus.json").read_text())
        memtx = ab.full_corpus_tests(policy, "memtx")
        vinyl = ab.full_corpus_tests(policy, "vinyl")
        self.assertEqual(len(memtx), 232)
        self.assertEqual(len(vinyl), 224)
        self.assertTrue(set(vinyl) < set(memtx))


if __name__ == "__main__":
    unittest.main()

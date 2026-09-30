#!/usr/bin/env python3
"""Unit checks for planner-flag route inventory classification."""
import json
from pathlib import Path
import sys
import unittest

import planner_flag_ab as ab


class PlannerFlagABTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = json.loads((Path(__file__).parent / "corpus.json").read_text())

    def test_invoke_tolerates_non_utf8_subprocess_output(self):
        result = ab.invoke([
            sys.executable, "-c",
            "import sys; sys.stdout.buffer.write(b'capture\\x8ecomplete')",
        ])
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "capture\ufffdcomplete")

    def test_documented_classes_are_not_feature_acceptance(self):
        policy = json.loads((Path(__file__).parent /
                             "planner_flag_route_classes.json").read_text())
        for suite, classes in policy.items():
            transitions = [{"from": {"route": row[0], "reason": row[1]},
                            "to": {"route": row[2], "reason": row[3]},
                            "queries": 1, "examples": []}
                           for row in classes["classes"]]
            reviewed = ab.classify_route_transitions(transitions, classes)
            self.assertTrue(all(row["class_review"] == "documented"
                                for row in reviewed), suite)

    def test_unlisted_transition_is_marked_unreviewed(self):
        transition = {"from": {"route": "new_planner", "reason": None},
                      "to": {"route": "fallback", "reason": "NEW_REASON"},
                      "queries": 3, "examples": ["snapshots/example"]}
        result = ab.classify_route_transitions([transition], {"classes": []})
        self.assertEqual(result[0]["class_review"], "unreviewed")

    def test_default_sql_selection_applies_documented_iproto_exclusion(self):
        tests, excluded = ab.select_tests(
            self.policy, "sql", "memtx", "llvm")
        self.assertEqual(len(tests), 33)
        self.assertNotIn("sql/iproto.test.lua", tests)
        self.assertIn("sql/iproto.test.lua", excluded)

    def test_explicitly_selecting_excluded_iproto_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "EXECUTE observes"):
            ab.select_tests(self.policy, "sql", "memtx", "generated",
                            ["sql/iproto.test.lua"])

    def test_direct_values_exclusion_applies_only_to_native_dispatch(self):
        test = "sql-luatest/gh_8676_exists_in_multiselect_test.lua"
        generated, generated_excluded = ab.select_tests(
            self.policy, "sql-luatest", "memtx", "generated")
        native, native_excluded = ab.select_tests(
            self.policy, "sql-luatest", "memtx", "cnp")
        self.assertIn(test, generated)
        self.assertEqual(generated_excluded, {})
        self.assertNotIn(test, native)
        self.assertIn(test, native_excluded)

    def test_multiline_commented_explain_is_plan_output(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = root / "snapshots/sql-tap/whereG/q108.memtx.yaml"
            snapshot.parent.mkdir(parents=True)
            snapshot.write_text(
                "test:\n  query_sql: '-- ANALYZE;\n"
                "        EXPLAIN QUERY PLAN SELECT 1;\n'\n")
            record = {"query_id": "snapshots/sql-tap/whereG/q108.memtx",
                      "fields": [{"name": "l1_result.rows"}]}
            explain, semantic = ab.explain_output_diffs([record], root)
            self.assertEqual(explain, [record])
            self.assertEqual(semantic, [])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Unit checks for planner-flag route inventory classification."""
import json
from pathlib import Path
import unittest

import planner_flag_ab as ab


class PlannerFlagABTest(unittest.TestCase):
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

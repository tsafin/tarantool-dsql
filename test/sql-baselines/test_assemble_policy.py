#!/usr/bin/env python3
"""Tests for reviewed full-corpus policy assembly."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("assemble_policy.py")
SPEC = importlib.util.spec_from_file_location("assemble_policy", MODULE_PATH)
assemble_policy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(assemble_policy)


class AssemblePolicyTest(unittest.TestCase):
    def test_post_baseline_exclusion_is_preserved(self):
        with tempfile.TemporaryDirectory(prefix="sql-corpus-review-") as temp:
            repo = Path(temp)
            rows_by_suite = {
                "sql-tap": {"test": "sql-tap/base.test.lua",
                            "decision": "include"},
                "sql": {"test": "sql/diagnostic.test.lua",
                        "decision": "exclude",
                        "introduced_after_baseline": True},
                "sql-luatest": {"test": "sql-luatest/base_test.lua",
                                "decision": "include"},
            }
            reviews = []
            for suite, row in rows_by_suite.items():
                path = repo / f"{suite}-review.json"
                (repo / "test" / suite).mkdir(parents=True)
                (repo / "test" / row["test"]).touch()
                engine_decisions = {}
                for engine in assemble_policy.corpus.ENGINES:
                    decision = {
                        "decision": row["decision"],
                        "category": "planner_diagnostic" if
                                    row["decision"] == "exclude" else
                                    "verified_parity",
                        "reason": "diagnostic-only test" if
                                 row["decision"] == "exclude" else
                                 "reviewed SQL workload",
                        "evidence": {"runner": "passed"},
                    }
                    if row.get("introduced_after_baseline"):
                        decision["introduced_after_baseline"] = True
                    engine_decisions[engine] = decision
                path.write_text(json.dumps({"suite": suite,
                                            "tests": [{"test": row["test"],
                                                       "engines": engine_decisions}]}))
                reviews.append(path)

            policy = assemble_policy.assemble(repo, reviews, "a" * 40)
            exclusions = [entry for entry in policy["excluded"]
                          if entry["test"] == "sql/diagnostic.test.lua"]
            self.assertEqual(len(exclusions), 2)
            self.assertTrue(all(entry["introduced_after_baseline"]
                                for entry in exclusions))

    def test_post_baseline_marker_only_applies_to_exclusions(self):
        with tempfile.TemporaryDirectory(prefix="sql-corpus-review-") as temp:
            repo = Path(temp)
            for suite in assemble_policy.corpus.SUITES:
                (repo / "test" / suite).mkdir(parents=True)
            test = "sql-tap/example.test.lua"
            (repo / "test" / test).touch()
            decisions = {engine: {
                "decision": "include", "category": "verified_parity",
                "reason": "reviewed", "evidence": {"runner": "passed"},
                "introduced_after_baseline": True,
            } for engine in assemble_policy.corpus.ENGINES}
            review = repo / "review.json"
            review.write_text(json.dumps({"suite": "sql-tap", "tests": [{
                "test": test, "engines": decisions}]}))
            with self.assertRaisesRegex(ValueError, "invalid post-baseline review"):
                assemble_policy.assemble(repo, [review], "a" * 40)


if __name__ == "__main__":
    unittest.main()

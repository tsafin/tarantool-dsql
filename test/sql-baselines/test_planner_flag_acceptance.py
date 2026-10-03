"""Tests for aggregate M3.5 planner-flag acceptance."""

import unittest

import planner_flag_acceptance as gate


def passing_report(suite, mode, commit="current"):
    engines = {}
    for engine in gate.ENGINES:
        tests, queries = gate.EXPECTED[(suite, mode, engine)]
        engines[engine] = {
            "tests": tests,
            "queries": queries,
            "test_ids": sorted(gate.PRODUCER_CASES) if suite == "sql-luatest" else [],
            "excluded_tests": gate.EXCLUSIONS.get((suite, mode), {}),
            "route_review_required": False,
            "unreviewed_route_transition_count": 0,
            "enabled_route_counts": {"new_planner": 1},
            "off_on_semantic_parity": {"passed": True, "semantic_diffs": 0},
            "off_repeat_semantic_parity": {"passed": True, "semantic_diffs": 0},
        }
    return {"suite": suite, "mode": mode, "source_commit": commit,
            "semantic_parity_passed": True,
            "route_review_required": False, "engines": engines}


def complete_matrix():
    return [passing_report(suite, mode)
            for suite in gate.REQUIRED_SUITES
            for mode in gate.REQUIRED_MODES]


def passing_producer_matrix(commit="current"):
    return {"source_commit": commit, "cases": [
        {"test": test, "mode": mode, "engine": engine,
         "status": "passed", "component_ledger_version": 1,
         "captured_queries": 1,
         "cnp_exec_delta": 1 if mode == "cnp" else 0,
         "llvm_exec_delta": 1 if mode == "llvm" else 0}
        for test in gate.PRODUCER_CASES
        for mode in gate.REQUIRED_MODES
        for engine in gate.ENGINES]}


class PlannerFlagAcceptanceTest(unittest.TestCase):
    def test_roadmap_only_drift_is_allowed(self):
        self.assertIn("docs/vdbe/roadmap.md", gate.ALLOWED_REPORT_DRIFT)

    def test_complete_current_matrix_passes(self):
        result = gate.evaluate(complete_matrix(), "current",
                               passing_producer_matrix())
        self.assertTrue(result["feature_acceptance_passed"],
                        result["feature_acceptance_blockers"])

    def test_missing_report_blocks(self):
        reports = complete_matrix()[:-1]
        result = gate.evaluate(reports, "current", passing_producer_matrix())
        self.assertFalse(result["feature_acceptance_passed"])
        self.assertTrue(any("missing suite/mode" in item
                            for item in result["feature_acceptance_blockers"]))

    def test_stale_source_blocks(self):
        reports = complete_matrix()
        reports[0]["source_commit"] = "stale"
        result = gate.evaluate(reports, "current", passing_producer_matrix())
        self.assertFalse(result["feature_acceptance_passed"])
        self.assertTrue(any("inconsistent suite-report source" in item
                            for item in result["feature_acceptance_blockers"]))

    def test_semantic_or_route_failure_blocks(self):
        reports = complete_matrix()
        reports[0]["engines"]["memtx"]["off_repeat_semantic_parity"] = {
            "passed": False, "semantic_diffs": 1}
        result = gate.evaluate(reports, "current", passing_producer_matrix())
        self.assertFalse(result["feature_acceptance_passed"])

    def test_missing_producer_case_blocks(self):
        reports = complete_matrix()
        report = next(item for item in reports
                      if item["suite"] == "sql-luatest" and
                      item["mode"] == "llvm")
        producer_report = passing_producer_matrix()
        producer_report["cases"].pop()
        result = gate.evaluate(reports, "current", producer_report)
        self.assertFalse(result["feature_acceptance_passed"])
        self.assertTrue(any("missing focused producer case" in item
                            for item in result["feature_acceptance_blockers"]))

    def test_embedded_dml_case_is_not_required_for_ledger_gate(self):
        self.assertNotIn("planner_insert_select_snapshot_test.lua",
                         gate.PRODUCER_CASES)
        result = gate.evaluate(complete_matrix(), "current",
                               passing_producer_matrix())
        self.assertTrue(result["feature_acceptance_passed"],
                        result["feature_acceptance_blockers"])


if __name__ == "__main__":
    unittest.main()

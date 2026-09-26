#!/usr/bin/env python3
"""Coverage-policy checks for the SQL parity corpus."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("corpus.py")
SPEC = importlib.util.spec_from_file_location("sql_corpus", MODULE_PATH)
corpus = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(corpus)


class CorpusPolicyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sql-corpus-policy-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        for suite in corpus.SUITES:
            (self.repo / "test" / suite).mkdir(parents=True)
        (self.repo / "test/sql-tap/example.test.lua").touch()

    def policy(self, scope="full-corpus", excluded=None):
        return {"scope": scope, "policy_version": 1,
                "baseline_commit": "a" * 40,
                "included": [{"test": "sql-tap/example.test.lua",
                 "engines": ["memtx"], "reason": "reviewed capture",
                 "category": "verified_parity",
                 "evidence": {"memtx": {"matrix": "passed"}}}],
                "excluded": excluded or []}

    def test_full_policy_requires_each_engine_decision(self):
        with self.assertRaisesRegex(ValueError, "unreviewed corpus engine"):
            corpus.inventory(self.repo, self.policy())

    def test_full_policy_accepts_explicit_engine_exclusion(self):
        policy = self.policy(excluded=[{
            "test": "sql-tap/example.test.lua", "engines": ["vinyl"],
            "reason": "normal runner does not support this engine",
            "category": "no_engine_variant", "evidence": {"runner": "unscheduled"}}])
        (self.repo / "test/sql-tap/second.test.lua").touch()
        policy["included"].append({"test": "sql-tap/second.test.lua",
                                   "engines": ["vinyl"],
                                   "reason": "reviewed capture",
                                   "category": "verified_parity",
                                   "evidence": {"vinyl": {"matrix": "passed"}}})
        policy["excluded"].append({"test": "sql-tap/second.test.lua",
                                   "engines": ["memtx"],
                                   "reason": "unsupported engine",
                                   "category": "no_engine_variant",
                                   "evidence": {"runner": "unscheduled"}})
        rows = corpus.inventory(self.repo, policy)
        self.assertEqual(rows[0]["engines"], ["memtx"])
        self.assertEqual(rows[0]["pending_engines"], [])

    def test_post_baseline_exclusion_is_valid_when_test_is_absent(self):
        test = "sql/planner_diagnostic.test.lua"
        policy = self.policy(excluded=[{
            "test": test, "engines": ["memtx", "vinyl"],
            "reason": "diagnostic-only planner assertion",
            "category": "planner_diagnostic",
            "evidence": {"normal_runner": "passed"},
            "introduced_after_baseline": True,
        }])
        policy["included"][0]["engines"].append("vinyl")
        policy["included"][0]["evidence"]["vinyl"] = {"matrix": "passed"}
        with self.assertRaisesRegex(ValueError, "invalid exclusion policy"):
            corpus.inventory(self.repo, policy)
        rows = corpus.inventory(self.repo, policy,
                                allow_post_baseline_absent=True)
        self.assertEqual(len(rows), 1)
        (self.repo / "test/sql" / "planner_diagnostic.test.lua").touch()
        rows = corpus.inventory(self.repo, policy)
        added = next(row for row in rows if row["test"] == test)
        self.assertEqual(added["status"], "excluded")
        self.assertEqual(added["excluded_engines"], {
            "memtx": "diagnostic-only planner assertion",
            "vinyl": "diagnostic-only planner assertion",
        })

    def test_absent_exclusion_requires_post_baseline_marker(self):
        policy = self.policy(excluded=[{
            "test": "sql/planner_diagnostic.test.lua",
            "engines": ["memtx", "vinyl"],
            "reason": "diagnostic-only planner assertion",
            "category": "planner_diagnostic",
            "evidence": {"normal_runner": "passed"},
        }])
        with self.assertRaisesRegex(ValueError, "invalid exclusion policy"):
            corpus.inventory(self.repo, policy)

    def test_full_policy_requires_both_engines_represented(self):
        policy = self.policy(excluded=[{
            "test": "sql-tap/example.test.lua", "engines": ["vinyl"],
            "reason": "unsupported engine",
            "category": "no_engine_variant", "evidence": {"runner": "unscheduled"}}])
        with self.assertRaisesRegex(ValueError, "both engines"):
            corpus.inventory(self.repo, policy)

    def test_policy_scope_and_reason_are_typed(self):
        policy = self.policy(scope="typo")
        with self.assertRaisesRegex(ValueError, "invalid corpus scope"):
            corpus.inventory(self.repo, policy)
        policy = self.policy(scope="seed-smoke")
        policy["included"][0]["reason"] = 42
        with self.assertRaisesRegex(ValueError, "invalid engine policy"):
            corpus.inventory(self.repo, policy)

    def test_full_policy_requires_named_baseline(self):
        policy = self.policy()
        policy["baseline_commit"] = "master"
        with self.assertRaisesRegex(ValueError, "40-character baseline"):
            corpus.inventory(self.repo, policy)

    def test_overlapping_inclusion_and_exclusion_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate inclusion/exclusion"):
            corpus.inventory(self.repo, self.policy(excluded=[{
                "test": "sql-tap/example.test.lua", "engines": ["memtx"],
                "reason": "conflict", "category": "verified_parity",
                "evidence": {"runner": "passed"}}]))

    def test_full_policy_rejects_pending_decision_category(self):
        policy = self.policy(excluded=[{
            "test": "sql-tap/example.test.lua", "engines": ["vinyl"],
            "reason": "audit still running", "category": "parity_pending",
            "evidence": {"runner": "passed"}}])
        with self.assertRaisesRegex(ValueError, "unreviewed full-corpus exclusion"):
            corpus.inventory(self.repo, policy)

    def test_full_policy_requires_per_engine_evidence(self):
        policy = self.policy(excluded=[{
            "test": "sql-tap/example.test.lua", "engines": ["vinyl"],
            "reason": "unsupported engine", "category": "no_engine_variant",
            "evidence": {"runner": "unscheduled"}}])
        policy["included"][0]["evidence"] = {}
        with self.assertRaisesRegex(ValueError, "missing full-corpus inclusion evidence"):
            corpus.inventory(self.repo, policy)
        policy["included"][0]["evidence"] = {"memtx": {"matrix": "passed"}}
        policy["excluded"][0]["evidence"] = {}
        with self.assertRaisesRegex(ValueError, "missing full-corpus exclusion evidence"):
            corpus.inventory(self.repo, policy)

    def test_seed_policy_can_keep_unreviewed_engines(self):
        rows = corpus.inventory(self.repo, self.policy(scope="seed-smoke"))
        self.assertEqual(rows[0]["pending_engines"], ["vinyl"])

    def test_capture_query_and_byte_budgets(self):
        root = self.repo / "capture"
        snapshots = root / "snapshots/sql-tap/example"
        snapshots.mkdir(parents=True)
        (snapshots / "q01.memtx.yaml").write_bytes(b"123456")
        actual = {("sql-tap/example.test.lua", "memtx"):
                  {"captured_queries": 1}}
        policy = {"capture_limits": {"max_queries_per_test": 1,
                                     "max_snapshot_bytes_per_test": 6}}
        corpus.enforce_budgets(root, actual, policy)
        policy["capture_limits"]["max_queries_per_test"] = 0
        with self.assertRaisesRegex(ValueError, "invalid capture_limits"):
            corpus.enforce_budgets(root, actual, policy)
        policy["capture_limits"]["max_queries_per_test"] = 1
        policy["capture_limits"]["max_snapshot_bytes_per_test"] = 5
        with self.assertRaisesRegex(ValueError, "byte budget exceeded"):
            corpus.enforce_budgets(root, actual, policy)

    def test_mode_proof_rejects_claim_without_native_execution(self):
        manifest = {"test_file": "sql-tap/example.test.lua",
                    "execution_mode": "llvm", "captured_queries": 3,
                    "executed_query_indices": [1, 2],
                    "native_compile_attempt_query_indices": [1, 2, 3],
                    "native_compile_success_query_indices": [2, 3],
                    "native_participation_query_indices": [2],
                    "eligible_query_indices": [2], "eligible_queries": 1,
                    "native_participation_queries": 1,
                    "mode_miss_queries": []}
        corpus.check_mode_proof(manifest)
        manifest["native_compile_attempt_query_indices"] = [1]
        with self.assertRaisesRegex(ValueError, "invalid native participation"):
            corpus.check_mode_proof(manifest)
        manifest["native_compile_attempt_query_indices"] = [1, 2, 3]
        manifest["native_participation_query_indices"] = []
        with self.assertRaisesRegex(ValueError, "invalid native participation"):
            corpus.check_mode_proof(manifest)
        manifest["eligible_query_indices"] = []
        manifest["eligible_queries"] = 0
        manifest["native_compile_success_query_indices"] = []
        manifest["native_participation_queries"] = 0
        with self.assertRaisesRegex(ValueError, "native mode has no attributed query"):
            corpus.check_mode_proof(manifest)

    def test_generated_mode_proof_has_no_native_claims(self):
        manifest = {"test_file": "sql-tap/example.test.lua",
                    "execution_mode": "generated", "captured_queries": 1,
                    "executed_query_indices": [1],
                    "native_compile_attempt_query_indices": [],
                    "native_compile_success_query_indices": [],
                    "native_participation_query_indices": [],
                    "eligible_query_indices": [], "eligible_queries": 0,
                    "native_participation_queries": 0,
                    "mode_miss_queries": []}
        corpus.check_mode_proof(manifest)
        manifest["native_compile_attempt_query_indices"] = [1]
        with self.assertRaisesRegex(ValueError, "generated run claims native work"):
            corpus.check_mode_proof(manifest)


if __name__ == "__main__":
    unittest.main()

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
        return {"scope": scope, "included": [{"test": "sql-tap/example.test.lua",
                 "engines": ["memtx"], "reason": "reviewed capture"}],
                "excluded": excluded or []}

    def test_full_policy_requires_each_engine_decision(self):
        with self.assertRaisesRegex(ValueError, "unreviewed corpus engine"):
            corpus.inventory(self.repo, self.policy())

    def test_full_policy_accepts_explicit_engine_exclusion(self):
        rows = corpus.inventory(self.repo, self.policy(excluded=[{
            "test": "sql-tap/example.test.lua", "engines": ["vinyl"],
            "reason": "normal runner does not support this engine"}]))
        self.assertEqual(rows[0]["engines"], ["memtx"])
        self.assertEqual(rows[0]["pending_engines"], [])

    def test_overlapping_inclusion_and_exclusion_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate inclusion/exclusion"):
            corpus.inventory(self.repo, self.policy(excluded=[{
                "test": "sql-tap/example.test.lua", "engines": ["memtx"],
                "reason": "conflict"}]))

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


if __name__ == "__main__":
    unittest.main()

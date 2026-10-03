#!/usr/bin/env python3
"""Contract tests for the paired JOIN execution reporter."""

import copy
import unittest

import e1_join_run as join


def observations():
    rows = []
    for engine in ("memtx", "vinyl"):
        for config, widths in join.CONFIGS.items():
            for query in join.QUERIES:
                for repeat in range(8):
                    rows.append({
                        "schema_version": 1, "workload_id": "bounded-dp-joins-v1",
                        "engine": engine, "configuration": config,
                        "query_id": query, "sql": "SELECT 1", "repeat": repeat,
                        "warmup": repeat < 3, "widths": list(widths),
                        "dispatcher": "generated", "source_commit": "a" * 40,
                        "binary_sha256": "b" * 64, "data_sha256": "c" * 64,
                        "statistics_id": "stats-v1", "actual_rows": 1,
                        "result_sha256": "d" * 64, "plan_sha256": "e" * 64,
                        "elapsed_us": 10 if config == "default" else 20,
                        "prepare_us": 5, "estimated_rows": 2,
                        "cardinalities": [{"stage_id": "join-output",
                                           "estimated_rows": 2,
                                           "actual_rows": 1}],
                    })
    return rows


class JoinRunnerTest(unittest.TestCase):
    def test_complete_pair(self):
        report = join.validate_and_report(observations(), ("memtx", "vinyl"))
        item = report["engines"]["memtx"]["queries"]["two-hot"]
        self.assertEqual(item["candidate_over_default_ratio"]["median"], 2)
        self.assertEqual(item["default_elapsed_us"]["n"], 5)
        self.assertEqual(report["engines"]["memtx"]["aggregate"]
                         ["default_elapsed_us"]["n"], len(join.QUERIES) * 5)
        self.assertFalse(item["plan_changed"])
        self.assertEqual(item["default_q_error"], 2)

    def test_result_drift_rejected(self):
        rows = observations()
        rows[-1]["result_sha256"] = "f" * 64
        with self.assertRaisesRegex(ValueError, "JOIN result changed"):
            join.validate_and_report(rows, ("memtx", "vinyl"))

    def test_width_or_statistics_drift_rejected(self):
        rows = observations()
        rows[-1]["widths"] = [1, 5, 10]
        with self.assertRaisesRegex(ValueError, "width metadata"):
            join.validate_and_report(rows, ("memtx", "vinyl"))
        rows = observations()
        rows[-1]["statistics_id"] = "other"
        with self.assertRaisesRegex(ValueError, "provenance"):
            join.validate_and_report(rows, ("memtx", "vinyl"))

    def test_missing_and_duplicate_repeats_rejected(self):
        rows = observations()
        with self.assertRaisesRegex(ValueError, "missing repetition"):
            join.validate_and_report(rows[:-1], ("memtx", "vinyl"))
        rows = observations()
        rows.append(copy.deepcopy(rows[-1]))
        with self.assertRaisesRegex(ValueError, "duplicate repetition"):
            join.validate_and_report(rows, ("memtx", "vinyl"))

    def test_plan_change_is_observed_not_rejected(self):
        rows = observations()
        for row in rows:
            if row["configuration"] == "candidate" and row["query_id"] == "two-hot":
                row["plan_sha256"] = "f" * 64
        report = join.validate_and_report(rows, ("memtx", "vinyl"))
        self.assertEqual(report["engines"]["memtx"]["plan_changes"], 1)


if __name__ == "__main__":
    unittest.main()

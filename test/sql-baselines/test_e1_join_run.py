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
                        "schema_version": 1, "workload_id": "bounded-dp-joins-v2",
                        "engine": engine, "configuration": config,
                        "query_id": query, "sql": "SELECT 1", "repeat": repeat,
                        "warmup": repeat < 3, "widths": list(widths),
                        "oracle_relation_limit": 0,
                        "dispatcher": "generated", "source_commit": "a" * 40,
                        "binary_sha256": "b" * 64, "data_sha256": "c" * 64,
                        "statistics_id": "stats-v1", "actual_rows": 1,
                        "result_sha256": "d" * 64, "plan_sha256": "e" * 64,
                        "elapsed_us": 10 if config == "default" else 20,
                        "prepare_us": 5, "estimated_rows": 2,
                        "prepare_samples_us": [5] + [6] * 7,
                        "planner_metric_samples": [
                            {"generated": 4, "dominated": 0,
                             "truncated": 0, "retained": 4,
                             "planner_elapsed_us": 2,
                             "peak_frontier": 2, "peak_solver_bytes": 512}
                            for _ in range(8)],
                        "executor_prefixes": ([
                            {"relation_mask": (1 << depth) - 1,
                             "estimated_rows": 2, "actual_rows": 1}
                            for depth in range(1, {"two": 2, "three": 3,
                                                   "four": 4}[
                                query.split("-", 1)[0]] + 1)]
                            if query in join.PREFIX_QUERIES else None),
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
        self.assertEqual(item["graph_class"], "edge-skew")
        self.assertEqual(item["default_repeated_prepare_us"]["n"], 7)
        self.assertEqual(item["candidate_over_default_prepare_ratio"]["median"], 1)
        self.assertEqual(item["default_planner"]["paths_generated"], 4)
        self.assertEqual(item["default_planner"]["where_planning_us"]["n"], 7)

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

    def test_repeated_prepare_contract(self):
        rows = observations()
        rows[-1]["prepare_samples_us"] = [5]
        with self.assertRaisesRegex(ValueError, "repeated preparation"):
            join.validate_and_report(rows, ("memtx", "vinyl"))
        rows = observations()
        rows[-1]["prepare_samples_us"][0] = 9
        with self.assertRaisesRegex(ValueError, "repeated preparation"):
            join.validate_and_report(rows, ("memtx", "vinyl"))
        rows = observations()
        rows[-1]["prepare_samples_us"] = [5] + [9] * 7
        with self.assertRaisesRegex(ValueError, "unstable repeated preparation"):
            join.validate_and_report(rows, ("memtx", "vinyl"))

    def test_planner_metric_contract(self):
        rows = observations()
        rows[-1]["planner_metric_samples"] = []
        with self.assertRaisesRegex(ValueError, "invalid planner metric"):
            join.validate_and_report(rows, ("memtx", "vinyl"))
        rows = observations()
        rows[-1]["planner_metric_samples"][1]["generated"] = 5
        with self.assertRaisesRegex(ValueError, "unstable planner path"):
            join.validate_and_report(rows, ("memtx", "vinyl"))

    def test_executor_prefix_contract(self):
        rows = observations()
        target = next(row for row in rows if row["query_id"] == "four-star")
        target["executor_prefixes"][-1]["actual_rows"] = 2
        with self.assertRaisesRegex(ValueError, "executor JOIN final stage"):
            join.validate_and_report(rows, ("memtx", "vinyl"))

    def test_exact_oracle_eligibility_and_parity(self):
        self.assertIn("four-dense-cycle", join.ORACLE_QUERIES)
        self.assertNotIn("three-cross-constrained", join.ORACLE_QUERIES)
        self.assertNotIn("left-join", join.ORACLE_QUERIES)
        rows = observations()
        oracle = []
        for row in rows:
            if row["configuration"] == "default" and \
                    row["query_id"] in join.ORACLE_QUERIES:
                copy_row = copy.deepcopy(row)
                copy_row["configuration"] = join.ORACLE_CONFIG
                copy_row["oracle_relation_limit"] = 4
                oracle.append(copy_row)
        report = join.validate_and_report(rows + oracle,
                                          ("memtx", "vinyl"), True)
        self.assertIn("oracle_estimate_quality", report)
        self.assertNotIn("exact_oracle",
                         report["engines"]["memtx"]["queries"]["left-join"])
        self.assertNotIn("exact_oracle",
                         report["engines"]["memtx"]["queries"]
                         ["three-cross-constrained"])
        drift = copy.deepcopy(oracle)
        drift[-1]["actual_rows"] = 3
        drift[-1]["cardinalities"][0]["actual_rows"] = 3
        drift[-1]["executor_prefixes"][-1]["actual_rows"] = 3
        with self.assertRaisesRegex(ValueError, "oracle JOIN result changed"):
            join.validate_and_report(rows + drift, ("memtx", "vinyl"), True)
        wrong = copy.deepcopy(oracle)
        wrong[-1]["oracle_relation_limit"] = 0
        with self.assertRaisesRegex(ValueError, "oracle eligibility"):
            join.validate_and_report(rows + wrong, ("memtx", "vinyl"), True)


if __name__ == "__main__":
    unittest.main()

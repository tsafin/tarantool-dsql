#!/usr/bin/env python3
"""Tests for the standalone E1/GATE measurement analyzer."""

import copy
import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("e1_measure.py")
SPEC = importlib.util.spec_from_file_location("e1_measure", SCRIPT)
e1_measure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(e1_measure)


def observation(configuration, repeat, elapsed, estimate=100, actual=50,
                warmup=False, stage="join-output"):
    return {
        "schema_version": 1,
        "workload_id": "analytic-v1",
        "query_id": "join-skew-01",
        "engine": "memtx",
        "dispatcher": "generated",
        "configuration": configuration,
        "source_commit": "0123456789abcdef",
        "binary_sha256": "a" * 64,
        "data_sha256": "b" * 64,
        "statistics_id": "stats-v1-seed-17",
        "repeat": repeat,
        "warmup": warmup,
        "elapsed_us": elapsed,
        "cardinalities": [{"stage_id": stage,
                            "estimated_rows": estimate,
                            "actual_rows": actual}],
    }


class E1MeasureTest(unittest.TestCase):
    def test_summarizes_qerror_latency_and_paired_cost(self):
        rows = [observation("default", 1, 100, estimate=100, actual=50),
                observation("default", 2, 120, estimate=40, actual=50),
                observation("candidate", 1, 200, estimate=50, actual=50),
                observation("candidate", 2, 240, estimate=50, actual=50)]
        result = e1_measure.analyze(rows, "default", "candidate")
        base = result["groups"]["analytic-v1/memtx/generated/default"]
        self.assertEqual(base["execution_samples"], 2)
        self.assertEqual(base["q_error"]["median"], 1.625)
        compare = result["paired_comparisons"]["analytic-v1/memtx/generated"]
        self.assertEqual(compare["paired_samples"], 2)
        self.assertEqual(compare["candidate_over_baseline_latency_ratio"]["median"], 2)

    def test_warmups_are_excluded(self):
        rows = [observation("default", 0, 10_000, warmup=True),
                observation("default", 1, 100),
                observation("candidate", 0, 10_000, warmup=True),
                observation("candidate", 1, 110)]
        result = e1_measure.analyze(rows, "default", "candidate")
        self.assertEqual(result["groups"]["analytic-v1/memtx/generated/default"]
                         ["execution_samples"], 1)
        self.assertEqual(result["paired_comparisons"]
                         ["analytic-v1/memtx/generated"]["paired_samples"], 1)

    def test_rejects_warmup_only_input(self):
        rows = [observation("default", 0, 10_000, warmup=True),
                observation("candidate", 0, 10_000, warmup=True)]
        with self.assertRaisesRegex(ValueError, "no non-warmup observations"):
            e1_measure.analyze(rows, "default", "candidate")

    def test_zero_cardinality_mismatch_is_unbounded(self):
        rows = [observation("default", 1, 100, estimate=0, actual=10),
                observation("candidate", 1, 100, estimate=10, actual=10)]
        result = e1_measure.analyze(rows, "default", "candidate")
        summary = result["groups"]["analytic-v1/memtx/generated/default"]
        self.assertEqual(summary["unbounded_q_error_stages"], 1)
        self.assertIsNone(summary["q_error"])

    def test_rejects_stage_mismatch_across_repeats_by_configuration(self):
        baseline = observation("default", 1, 100, stage="join-output")
        candidate = observation("candidate", 1, 100, stage="scan-output")
        self.assertNotEqual(baseline["cardinalities"][0]["stage_id"],
                            candidate["cardinalities"][0]["stage_id"])
        with self.assertRaisesRegex(ValueError, "unmatched cardinality stages"):
            e1_measure.analyze([baseline, candidate], "default", "candidate")

    def test_rejects_actual_cardinality_mismatch_between_configurations(self):
        baseline = observation("default", 1, 100, actual=50)
        candidate = observation("candidate", 1, 100, actual=49)
        with self.assertRaisesRegex(ValueError,
                                    "unmatched actual cardinalities"):
            e1_measure.analyze([baseline, candidate], "default", "candidate")

    def test_rejects_duplicate_and_bad_numeric_fields(self):
        row = observation("default", 1, 100)
        e1_measure.validate_row(row, "in-memory", 1)
        duplicate_stage = copy.deepcopy(row)
        duplicate_stage["cardinalities"].append(copy.deepcopy(
            duplicate_stage["cardinalities"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate stage"):
            e1_measure.validate_row(duplicate_stage, "in-memory", 2)
        bad = copy.deepcopy(row)
        bad["elapsed_us"] = 0
        with self.assertRaisesRegex(ValueError, "elapsed_us"):
            e1_measure.validate_row(bad, "in-memory", 3)


if __name__ == "__main__":
    unittest.main()

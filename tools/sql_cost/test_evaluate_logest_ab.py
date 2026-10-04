import json
from pathlib import Path
import tempfile
import unittest

from evaluate_logest_ab import (candidate_cost, evaluate_candidate,
                                CAPTURE_ACCESSES, load_planner_estimates,
                                plan_estimated_rows, quantize_model,
                                sql_log_est, unforced_winner)


def scaled_model():
    return {
        "training_rows": [128, 256],
        "engines": {
            "memtx": {
                "primary_scan_us_per_input_row": 0.30,
                "primary_output_us_per_row": 0.50,
                "secondary_startup_us": 2.0,
                "secondary_output_us_per_row": 0.60,
            },
            "vinyl": {
                "primary_scan_us_per_input_row": 2.0,
                "primary_output_us_per_row": 1.0,
                "secondary_startup_us": 3.0,
                "secondary_output_us_per_row": 8.0,
            },
        },
    }


def report(rows=512):
    accesses = {}
    values = {
        "memtx": (100, 200, 10, 120),
        "vinyl": (100, 200, 10, 300),
    }
    for engine, (primary_short, primary_broad,
                 secondary_short, secondary_broad) in values.items():
        costs = {
            "primary_filtered": primary_short,
            "primary_range": primary_short,
            "primary_tail": primary_short,
            "primary_broad": primary_broad,
            "secondary_payload": secondary_short,
            "secondary_range": secondary_short,
            "secondary_tail": secondary_short,
            "secondary_broad": secondary_broad,
        }
        for access, value in costs.items():
            accesses.setdefault(access, {})[engine + "_median_us"] = value
    plan = lambda engine: [[0, 0, 0, "SEARCH TABLE cost_" + engine +
                            " USING COVERING INDEX cost_" + engine +
                            "_a (a>?) (~262144 rows)"]]
    return {
        "rows": rows,
        "storage_state": "memory",
        "accesses": accesses,
        "unforced_broad_plan": {engine: plan(engine) for engine in values},
        "unforced_tail_plan": {engine: plan(engine) for engine in values},
    }


def planner_estimates():
    result = {engine: {} for engine in ("memtx", "vinyl")}
    for engine in result:
        result[engine] = {
            "primary_scan": 1024,
            "primary_filtered": 500,
            "primary_range": 500,
            "primary_broad": 500,
            "primary_tail": 500,
            "secondary_payload": 16,
            "secondary_range": 16,
            "secondary_broad": 256,
            "secondary_tail": 256,
        }
    return result


class LogEstABTest(unittest.TestCase):
    def test_sql_log_est_matches_c_examples(self):
        self.assertEqual(sql_log_est(0), 0)
        self.assertEqual(sql_log_est(1), 0)
        self.assertEqual(sql_log_est(2), 10)
        self.assertEqual(sql_log_est(4), 20)
        self.assertEqual(sql_log_est(10), 33)
        self.assertEqual(sql_log_est(18), 42)
        self.assertEqual(sql_log_est(25), 46)
        with self.assertRaisesRegex(ValueError, "non-negative integer"):
            sql_log_est(-1)

    def test_quantized_candidate_preserves_expected_engine_split(self):
        model = quantize_model(scaled_model())
        memtx = candidate_cost(model["engines"]["memtx"], 512, 256)
        vinyl = candidate_cost(model["engines"]["vinyl"], 512, 256)
        self.assertEqual(memtx["winner"], "secondary")
        self.assertEqual(vinyl["winner"], "primary")
        self.assertLess(memtx["secondary_logest"], memtx["primary_logest"])
        self.assertGreater(vinyl["secondary_logest"], vinyl["primary_logest"])

    def test_invalid_work_unit_rejected(self):
        with self.assertRaisesRegex(ValueError, "work unit"):
            quantize_model(scaled_model(), 0)

    def test_unforced_plan_parser(self):
        capture = report()
        self.assertEqual(unforced_winner(capture, "vinyl", "broad"),
                         "secondary")
        self.assertIsNone(unforced_winner(capture, "vinyl", "filtered"))

    def test_plan_estimate_parser_is_strict(self):
        plan = [[0, 0, 0, "SEARCH TABLE t (~262144 rows)"]]
        self.assertEqual(plan_estimated_rows(plan), 262144)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            plan_estimated_rows([[0, 0, 0, "SCAN TABLE t"]])
        with self.assertRaisesRegex(ValueError, "exactly one"):
            plan_estimated_rows(plan + plan)

    def test_raw_capture_estimates_must_be_stable(self):
        manifest = {
            "run_id": "run", "source_commit": "commit",
            "binary_sha256": "binary", "rows": 512,
            "storage_state": "memory",
        }
        capture_report = dict(manifest)
        observations = []
        for engine in ("memtx", "vinyl"):
            for access in CAPTURE_ACCESSES:
                observations.append({
                    **manifest,
                    "engine": engine,
                    "access": access,
                    "explain": [[0, 0, 0, "SCAN TABLE t (~42 rows)"]],
                })
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "observations.jsonl"
            path.write_text("".join(json.dumps(item) + "\n"
                                    for item in observations))
            estimates = load_planner_estimates(directory, manifest,
                                               capture_report)
            self.assertEqual(estimates["vinyl"]["secondary_tail"], 42)
            observations.append({
                **manifest,
                "engine": "vinyl",
                "access": "secondary_tail",
                "explain": [[0, 0, 0, "SCAN TABLE t (~43 rows)"]],
            })
            path.write_text("".join(json.dumps(item) + "\n"
                                    for item in observations))
            with self.assertRaisesRegex(ValueError, "unstable"):
                load_planner_estimates(directory, manifest, capture_report)

    def test_ab_separates_candidate_and_production_coverage(self):
        result = evaluate_candidate(quantize_model(scaled_model()), report(),
                                    planner_estimates())
        self.assertEqual(result["candidate_ranking_total"], 8)
        self.assertEqual(result["oracle_cardinality_candidate_ranking_matches"],
                         8)
        self.assertEqual(result["planner_cardinality_candidate_ranking_matches"],
                         7)
        self.assertEqual(result["planner_cardinality_match_delta"], -1)
        self.assertEqual(result["oracle_cardinality_direct_matches"], 4)
        self.assertEqual(result["planner_cardinality_direct_matches"], 3)
        self.assertEqual(result["direct_comparison_total"], 4)
        self.assertEqual(result["production_choice_total"], 4)
        self.assertEqual(result["production_choice_matches"], 3)
        self.assertFalse(result["engines"]["vinyl"]["broad"]
                         ["production_match"])
        vinyl_broad = result["engines"]["vinyl"]["broad"]
        self.assertEqual(vinyl_broad["observed_winner"], "primary")
        self.assertEqual(vinyl_broad["planner_cardinality_candidate"]["winner"],
                         "secondary")
        self.assertFalse(vinyl_broad["planner_cardinality_candidate"]
                         ["ranking_match"])


if __name__ == "__main__":
    unittest.main()

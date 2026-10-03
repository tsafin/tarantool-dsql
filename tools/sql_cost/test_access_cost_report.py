import json
from pathlib import Path
import tempfile
import unittest

from access_cost_report import ACCESSES, ENGINES, summarize


def observations():
    result = []
    for repeat in range(1, 6):
        for access in ACCESSES:
            for engine in ENGINES:
                elapsed = repeat * (2 if engine == "vinyl" else 1)
                result.append({
                    "schema_version": 1, "fixture": "fixture-v1", "rows": 128,
                    "tarantool_version": "test-build",
                    "engine": engine, "access": access, "repeat_no": repeat,
                    "result_rows": 16,
                    "per_execution_us": elapsed, "elapsed_us": elapsed * 10,
                    "iterations": 10, "cache_state": "warm",
                    "timing_scope": "prepared_execute_and_materialize",
                    "explain": [[0, 0, 0, "SEARCH TABLE"]], "sql": "SELECT 1",
                })
    return result


class ReportTest(unittest.TestCase):
    def summarize_rows(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "observations.jsonl"
            path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            return summarize(path)

    def test_paired_medians(self):
        report = self.summarize_rows(observations())
        self.assertEqual(report["accesses"]["primary_point"]["memtx_median_us"], 3)
        self.assertEqual(report["accesses"]["primary_point"]["vinyl_to_memtx_ratio"], 2)

    def test_missing_pair_rejected(self):
        with self.assertRaisesRegex(ValueError, "unpaired"):
            self.summarize_rows(observations()[:-1])

    def test_duplicate_rejected(self):
        rows = observations()
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.summarize_rows(rows + [rows[0]])

    def test_mixed_fixture_rejected(self):
        rows = observations()
        rows[-1]["rows"] = 256
        with self.assertRaisesRegex(ValueError, "mixed fixtures"):
            self.summarize_rows(rows)

    def test_timing_scope_rejected(self):
        rows = observations()
        rows[0]["cache_state"] = "cold"
        with self.assertRaisesRegex(ValueError, "mixed timing scope"):
            self.summarize_rows(rows)

    def test_plan_drift_rejected(self):
        rows = observations()
        rows[-1]["explain"] = [[0, 0, 0, "SCAN TABLE"]]
        with self.assertRaisesRegex(ValueError, "plan, or result shape changed"):
            self.summarize_rows(rows)

    def test_too_few_repeats_rejected(self):
        rows = [row for row in observations() if row["repeat_no"] != 5]
        with self.assertRaisesRegex(ValueError, "at least five"):
            self.summarize_rows(rows)


if __name__ == "__main__":
    unittest.main()

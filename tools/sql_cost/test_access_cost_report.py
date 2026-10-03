import json
from pathlib import Path
import tempfile
import unittest

from access_cost_report import ACCESSES, ENGINES, summarize
from run_access_cost import BENCH


def observations():
    result = []
    for repeat in range(1, 6):
        for access in ACCESSES:
            for engine in ENGINES:
                elapsed = repeat * (2 if engine == "vinyl" else 1)
                result.append({
                    "schema_version": 2, "fixture": "fixture-v1", "rows": 128,
                    "tarantool_version": "test-build",
                    "source_commit": "a" * 40, "binary_sha256": "b" * 64,
                    "run_id": "run-1", "storage_state": "memory",
                    "engine": engine, "access": access, "repeat_no": repeat,
                    "result_rows": 16,
                    "per_execution_us": elapsed, "elapsed_us": elapsed * 10,
                    "iterations": 10, "cache_state": "uncontrolled",
                    "warmup_scope": "one_execution",
                    "timing_scope": "prepared_execute_and_materialize",
                    "vinyl_counters": ({"run_count": 0,
                                        "disk_read_pages": 0, "disk_lookup": 0,
                                        "cache_lookup": 0, "cache_get_rows": 0,
                                        "memory_get_rows": 0}
                                       if engine == "vinyl" else None),
                    "explain": [[0, 0, 0, "SEARCH TABLE"]], "sql": "SELECT 1",
                })
    return result


class ReportTest(unittest.TestCase):
    def test_benchmark_path_works_from_temporary_directory(self):
        self.assertTrue(BENCH.is_absolute())
        self.assertTrue(BENCH.is_file())

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

    def test_mixed_provenance_rejected(self):
        rows = observations()
        rows[-1]["binary_sha256"] = "c" * 64
        with self.assertRaisesRegex(ValueError, "mixed provenance"):
            self.summarize_rows(rows)

    def test_dumped_requires_run(self):
        rows = observations()
        for row in rows:
            row["storage_state"] = "dumped"
        with self.assertRaisesRegex(ValueError, "no Vinyl run"):
            self.summarize_rows(rows)


if __name__ == "__main__":
    unittest.main()

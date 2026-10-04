import unittest

from calibrate_access_cost import evaluate, fit, predict


def report():
    samples = {
        "primary_filtered": (10, 30),
        "primary_range": (11, 31),
        "primary_scan": (42, 62),
        "primary_broad": (24, 44),
        "secondary_payload": (2, 3),
        "secondary_range": (2.5, 4),
        "secondary_broad": (15, 60),
        "primary_tail": (12, 32),
        "secondary_tail": (2, 3),
    }
    return {
        "rows": 128, "storage_state": "memory",
        "accesses": {name: {"memtx_median_us": values[0],
                            "vinyl_median_us": values[1]}
                     for name, values in samples.items()},
    }


class CalibrationTest(unittest.TestCase):
    def test_engine_specific_fit_and_ranking(self):
        model = fit(report())
        self.assertEqual(model["unit"], "prepared_materialized_sql_us")
        self.assertEqual(predict(model["engines"]["memtx"], "primary", 16), 10)
        self.assertEqual(predict(model["engines"]["vinyl"], "secondary", 16), 3)
        validation = evaluate(model, report())
        self.assertEqual(validation["ranking_total"], 8)
        self.assertEqual(validation["ranking_matches"], 8)
        self.assertFalse(validation["engines"]["vinyl"]["broad"]
                         ["predicted_secondary_over_primary"] < 1)

    def test_negative_coefficient_rejected(self):
        bad = report()
        bad["accesses"]["primary_scan"]["memtx_median_us"] = 1
        with self.assertRaisesRegex(ValueError, "negative empirical"):
            fit(bad)

    def test_mismatched_fixture_rejected(self):
        model = fit(report())
        other = report()
        other["rows"] = 256
        with self.assertRaisesRegex(ValueError, "fixture sizes differ"):
            evaluate(model, other)


if __name__ == "__main__":
    unittest.main()

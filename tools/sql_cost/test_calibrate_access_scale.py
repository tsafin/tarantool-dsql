import unittest

from calibrate_access_scale import evaluate_scaled, fit_scaled


def report(rows):
    accesses = {}
    for engine, scan, emit, secondary_start, secondary_rate in (
            ("memtx", 0.3, 0.5, 2.0, 0.6),
            ("vinyl", 2.0, 1.0, 3.0, 8.0)):
        primary_short = scan * rows
        primary_broad = primary_short + emit * (rows // 2 - 16)
        secondary_broad = secondary_start + secondary_rate * (rows // 2 - 16)
        values = {
            "primary_filtered": primary_short, "primary_range": primary_short,
            "primary_tail": primary_short, "primary_broad": primary_broad,
            "secondary_payload": secondary_start,
            "secondary_range": secondary_start,
            "secondary_tail": secondary_start,
            "secondary_broad": secondary_broad,
        }
        for name, value in values.items():
            accesses.setdefault(name, {})[engine + "_median_us"] = value
    return {"rows": rows, "storage_state": "memory", "accesses": accesses}


class SizeTransferTest(unittest.TestCase):
    def test_transfer_to_held_out_size(self):
        model = fit_scaled([report(128), report(256)])
        result = evaluate_scaled(model, report(512))
        self.assertEqual(result["ranking_matches"], 8)
        self.assertEqual(result["ranking_total"], 8)
        self.assertGreater(result["engines"]["vinyl"]["broad"]
                           ["predicted_secondary_over_primary"], 1)

    def test_duplicate_train_size_rejected(self):
        with self.assertRaisesRegex(ValueError, "distinct training"):
            fit_scaled([report(128), report(128)])

    def test_training_size_cannot_be_validation(self):
        model = fit_scaled([report(128), report(256)])
        with self.assertRaisesRegex(ValueError, "used in training"):
            evaluate_scaled(model, report(128))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Tests for compact SQL-luatest review evidence."""

import importlib.util
from pathlib import Path
import sys
import unittest


HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
SPEC = importlib.util.spec_from_file_location("review_luatest",
                                              HERE / "review_luatest.py")
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


class MatrixEvidenceTest(unittest.TestCase):
    def test_keeps_mode_counts_and_parity_without_logs(self):
        modes = {
            "generated": {"status": "passed", "captured_queries": 2,
                          "detail": "verbose runner log"},
            "cnp": {"status": "passed", "captured_queries": 2,
                    "eligible_queries": 1,
                    "native_participation_queries": 1,
                    "parity": "passed", "parity_detail": "verbose diff"},
            "llvm": {"status": "capture_failed", "detail": "failed"},
            "generated-repeat": {"status": "not_run"},
        }
        result = review.matrix_evidence(modes, "partial capture")
        self.assertEqual(result["source_guard"], "partial capture")
        self.assertEqual(result["modes"]["cnp"], {
            "status": "passed", "captured_queries": 2,
            "eligible_queries": 1, "native_participation_queries": 1,
            "parity": "passed",
        })
        self.assertEqual(result["modes"]["llvm"],
                         {"status": "capture_failed"})
        self.assertNotIn("detail", str(result))


if __name__ == "__main__":
    unittest.main()

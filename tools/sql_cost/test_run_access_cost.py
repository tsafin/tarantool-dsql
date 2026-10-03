import unittest
from unittest import mock

import run_access_cost


class RunnerTest(unittest.TestCase):
    def test_embedded_revision(self):
        with mock.patch.object(run_access_cost, "git", return_value="a" * 40) as git:
            result = run_access_cost.binary_source_commit(
                "Tarantool 1.3.2-19607-ge688db7ff9\nTarget: Linux")
        self.assertEqual(result, "a" * 40)
        git.assert_called_once_with("rev-parse", "--verify",
                                    "e688db7ff9^{commit}")

    def test_version_without_revision_rejected(self):
        with self.assertRaisesRegex(ValueError, "no embedded Git revision"):
            run_access_cost.binary_source_commit("Tarantool 1.3.2")


if __name__ == "__main__":
    unittest.main()

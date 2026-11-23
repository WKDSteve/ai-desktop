"""Tests of tools/check-dashboard-size.py: `python -m unittest discover tools`."""

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).parent
spec = importlib.util.spec_from_file_location("check_dashboard_size", HERE / "check-dashboard-size.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


def run(root):
    return subprocess.run(
        [sys.executable, str(HERE / "check-dashboard-size.py"), "--root", str(root)], capture_output=True, text=True
    )


class DashboardSize(unittest.TestCase):
    def test_the_dashboard_of_the_repository_is_within_its_budget(self):
        result = run(HERE.parent / "dashboard")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_line_endings_count_as_lf(self):
        with tempfile.TemporaryDirectory() as folder:
            (pathlib.Path(folder) / "a.css").write_bytes(b"a\r\nb\r\n")
            self.assertEqual([item[:2] for item in check.sizes(folder)], [("a.css", 4)])

    def test_a_file_over_the_budget_is_refused_and_listed(self):
        with tempfile.TemporaryDirectory() as folder:
            (pathlib.Path(folder) / "big.js").write_bytes(b"x" * (check.BUDGET_RAW + 1))
            (pathlib.Path(folder) / "small.css").write_bytes(b"a{}")
            result = run(folder)
            self.assertEqual(result.returncode, 1)
            self.assertIn("big.js", result.stdout)
            self.assertIn("(4 over)", result.stderr)  # 1 byte over in big.js + the 3 bytes of small.css

    def test_incompressible_bytes_can_break_the_gzip_budget_alone(self):
        import os

        with tempfile.TemporaryDirectory() as folder:
            (pathlib.Path(folder) / "noise.js").write_bytes(os.urandom(check.BUDGET_GZIP))
            result = run(folder)
            self.assertEqual(result.returncode, 1)
            self.assertIn("gzipped", result.stderr)

    def test_an_empty_root_is_no_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(run(folder).returncode, 2)

    def test_the_budget_is_about_ten_percent_over_the_size_it_was_set_at(self):
        # The numbers of the docstring (2026-11-23); a budget far from them is a decision someone should see.
        self.assertTrue(73596 * 1.05 < check.BUDGET_RAW < 73596 * 1.15)
        self.assertTrue(22392 * 1.05 < check.BUDGET_GZIP < 22392 * 1.15)


if __name__ == "__main__":
    unittest.main()

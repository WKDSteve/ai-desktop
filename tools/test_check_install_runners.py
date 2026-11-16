"""Tests of tools/check-install-runners.py."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_install_runners", HERE / "check-install-runners.py")
assert spec and spec.loader
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

HEAD = "name: demo\non:\n  push:\njobs:\n"


class CheckInstallRunnersTest(unittest.TestCase):
    def problems(self, jobs: str) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "demo.yml"
            path.write_text(HEAD + jobs, encoding="utf-8")
            return check.problems(path)

    def test_the_repository_is_clean(self) -> None:
        self.assertEqual(check.main([]), 0)

    def test_the_old_published_job_is_refused(self) -> None:
        # rolling.yml before 2026-11-16: the stamped script on the author's machine, then the uninstaller.
        found = self.problems(
            "  published:\n"
            "    runs-on: [self-hosted, windows]\n"
            "    steps:\n"
            "      - run: |\n"
            "          & ([scriptblock]::Create($text)) -AllowElevated\n"
            "          Start-Process unins000.exe -ArgumentList '/VERYSILENT'\n")
        self.assertEqual(len(found), 1, found)
        self.assertIn("job published", found[0])
        self.assertIn("unins000", found[0])

    def test_the_installer_check_on_the_self_hosted_runner_is_refused(self) -> None:
        found = self.problems(
            "  build:\n"
            "    runs-on: [self-hosted, windows]\n"
            "    steps:\n"
            "      - uses: ./.github/actions/installer-verify\n")
        self.assertEqual(len(found), 1, found)

    def test_on_a_hosted_vm_everything_is_allowed(self) -> None:
        self.assertEqual(self.problems(
            "  verify:\n"
            "    runs-on: windows-latest\n"
            "    steps:\n"
            "      - uses: ./.github/actions/installer-verify\n"
            "      - run: irm https://x/install.ps1 | iex\n"), [])

    def test_the_line_needs_the_arm64_stop_on_the_self_hosted_runner(self) -> None:
        job = ("  defender:\n"
               "    runs-on: [self-hosted, windows]\n"
               "    steps:\n"
               "      - env:\n"
               "{arch}"
               "          LINE: irm https://x/install.ps1 | iex\n"
               "        run: $env:LINE | powershell -Command -\n")
        found = self.problems(job.format(arch=""))
        self.assertEqual(len(found), 1, found)
        self.assertIn("without PROCESSOR_ARCHITECTURE: ARM64", found[0])
        self.assertEqual(self.problems(job.format(arch="          PROCESSOR_ARCHITECTURE: ARM64\n")), [])

    def test_the_arm64_stop_does_not_excuse_a_real_installer(self) -> None:
        found = self.problems(
            "  sneaky:\n"
            "    runs-on: [self-hosted, windows]\n"
            "    env:\n"
            "      PROCESSOR_ARCHITECTURE: ARM64\n"
            "    steps:\n"
            "      - run: .\\setup.exe /VERYSILENT\n")
        self.assertEqual(len(found), 1, found)

    def test_a_comment_that_names_the_check_does_not_count(self) -> None:
        self.assertEqual(self.problems(
            "  build:\n"
            "    runs-on: [self-hosted, windows]\n"
            "    steps:\n"
            "      - run: make\n"
            "\n"
            "  # The installer is proven by ./.github/actions/installer-verify on a VM; unins000 too.\n"
            "  publish:\n"
            "    runs-on: [self-hosted, windows]\n"
            "    steps:\n"
            "      - run: echo publish\n"), [])


if __name__ == "__main__":
    unittest.main()

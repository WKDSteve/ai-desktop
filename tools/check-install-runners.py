#!/usr/bin/env python3
"""Fail when a job on the self-hosted runner installs the app.

The self-hosted runner is the author's machine, where ai-desktop is installed for real. A job there that runs the
setup or the install script installs over that copy (same AppId, same Start menu link, same port 8080) and its
uninstall takes it away: the `published` job of rolling.yml did exactly that four times between 2026-11-14 and
2026-11-16, until it moved to windows-latest. Whatever installs runs on a throw-away GitHub-hosted VM.

A job counts as self-hosted when its `runs-on` names `self-hosted`. It may not hold any of INSTALLS (the installer
check, the setup's own switches, the uninstaller). It may hold the install line (RUNS_THE_LINE) only when the job
sets PROCESSOR_ARCHITECTURE to ARM64: install.ps1 then stops before it downloads anything ("no build for Windows on
ARM64"), which is how the Defender check of ci.yml runs the line without installing.

    python tools/check-install-runners.py [WORKFLOW ...]      (default: .github/workflows/*.yml)

Exit 0: clean. Exit 1: the jobs are listed with what they hold.

A plain reader of the YAML on purpose (no PyYAML on the runner): a job is a key indented by two spaces under the
top-level `jobs:`, and its text runs to the next such key. Comment lines are left out, so a comment that names the
installer check does not count.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# What installs or removes the app. Matched case-insensitively in the job's text.
INSTALLS = (
    "installer-verify",
    "verify-installer",
    "unins000",
    "/verysilent",
    "-allowelevated",
    "install_allow_elevated",
)
# The install line, or the script of the release-kit run any other way.
RUNS_THE_LINE = ("install.ps1", "| iex", "invoke-expression")
ARM64 = re.compile(r"^\s*PROCESSOR_ARCHITECTURE:\s*['\"]?ARM64['\"]?\s*(#.*)?$", re.MULTILINE)
JOB = re.compile(r"^  ([A-Za-z0-9_-]+):\s*(#.*)?$")
RUNS_ON = re.compile(r"^    runs-on:\s*(.+?)\s*(#.*)?$", re.MULTILINE)


def jobs(text: str) -> dict[str, str]:
    """The jobs of a workflow: name -> its lines without comment lines."""
    found: dict[str, list[str]] = {}
    in_jobs = False
    current: str | None = None
    for line in text.splitlines():
        if line.strip().startswith("#"):
            continue
        if not line.startswith(" ") and line.strip():
            in_jobs = line.rstrip() == "jobs:"
            current = None
            continue
        if not in_jobs:
            continue
        m = JOB.match(line)
        if m:
            current = m.group(1)
            found[current] = []
        elif current is not None:
            found[current].append(line)
    return {name: "\n".join(lines) for name, lines in found.items()}


def problems(path: Path) -> list[str]:
    out = []
    for name, body in jobs(path.read_text(encoding="utf-8")).items():
        runs_on = RUNS_ON.search(body)
        if not runs_on or "self-hosted" not in runs_on.group(1):
            continue
        lower = body.lower()
        installs = [m for m in INSTALLS if m in lower]
        if installs:
            out.append(f"{path.name}: job {name} runs on the self-hosted runner and installs ({', '.join(installs)})")
            continue
        line = [m for m in RUNS_THE_LINE if m in lower]
        if line and not ARM64.search(body):
            out.append(f"{path.name}: job {name} runs on the self-hosted runner and runs the install script "
                       f"({', '.join(line)}) without PROCESSOR_ARCHITECTURE: ARM64")
    return out


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv] or sorted((ROOT / ".github" / "workflows").glob("*.yml"))
    found = [p for path in paths for p in problems(path)]
    for p in found:
        print(p)
    if found:
        print("whatever installs the app runs on a GitHub-hosted VM (windows-latest), never on the self-hosted runner")
        return 1
    print(f"ok: no job on the self-hosted runner installs ({len(paths)} workflow(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

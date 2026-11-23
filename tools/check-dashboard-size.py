#!/usr/bin/env python3
"""Fail when the dashboard (dashboard/) is bigger than its budget.

The dashboard is a handful of files with no build step; every byte of them goes to the browser as written (the server
sends no Content-Encoding), so the size is the sum of the files. This adds up what a first load
fetches, the bytes of every file under the root with line endings counted as LF (a checkout on Windows may write CRLF),
and the same files compressed one by one with gzip (what a compressing server or proxy would send), and compares both
with a budget:

    python tools/check-dashboard-size.py [--root dashboard]

The budgets are the size when they were set (2026-11-23: 73596 bytes, 22392 gzipped) plus about 10 %. A change that
needs more raises the budget in the same commit, with the reason in its message: the number is the decision, and a page
grows by a few lines at a time unless something keeps count.

Exit 0: within budget. Exit 1: over, the files are listed by size. Exit 2: no file under the root (a wrong root checks
nothing, and that is no pass).
"""

import argparse
import gzip
import os
import sys

# Bytes of all files (LF), and of all files gzipped one by one (level 9).
BUDGET_RAW = 81000
BUDGET_GZIP = 24700


def sizes(root):
    """(path, raw bytes, gzipped bytes) of every file under root, the path relative to root with forward slashes."""
    found = []
    for folder, _dirs, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            with open(path, "rb") as handle:
                data = handle.read().replace(b"\r\n", b"\n")
            packed = len(gzip.compress(data, 9, mtime=0))
            found.append((os.path.relpath(path, root).replace(os.sep, "/"), len(data), packed))
    return sorted(found)


def check(found, budget_raw=BUDGET_RAW, budget_gzip=BUDGET_GZIP):
    """The lines of the findings (empty when within budget)."""
    raw = sum(item[1] for item in found)
    packed = sum(item[2] for item in found)
    lines = []
    if raw > budget_raw:
        lines.append(f"the dashboard is {raw} bytes, the budget {budget_raw} ({raw - budget_raw} over)")
    if packed > budget_gzip:
        lines.append(f"gzipped it is {packed} bytes, the budget {budget_gzip} ({packed - budget_gzip} over)")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default="dashboard", help="the folder of the dashboard")
    args = parser.parse_args()
    found = sizes(args.root)
    if not found:
        print(f"no file under {args.root}", file=sys.stderr)
        return 2
    raw = sum(item[1] for item in found)
    packed = sum(item[2] for item in found)
    print(f"{args.root}: {raw} bytes (budget {BUDGET_RAW}), gzipped {packed} (budget {BUDGET_GZIP})")
    problems = check(found)
    if problems:
        for path, size, small in sorted(found, key=lambda item: -item[1]):
            print(f"  {path}: {size} bytes, {small} gzipped")
        for line in problems:
            print(line, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

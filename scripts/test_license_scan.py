#!/usr/bin/env python3
"""Test harness for scripts/license_scan.py — written FIRST (test-first).

Standalone (no pytest dependency; the dev image pins only cmake, ninja,
python3, git, jq, zstd). Exits 0 iff all checks pass, 1 otherwise, and prints
a machine-readable JSON report (spec section 15, constraint 4).

Checks:
  1. The real repo scan passes (every third_party/ entry pinned in
     LICENSES/manifest.json, license files present, licenses OSS-allowlisted).
  2. The negative fixture testkit/fixtures/license_scan/missing_entry — a
     vendored third_party/orphan_dep with NO manifest entry — FAILS the scan
     with a diagnostic naming the orphan.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SCAN = REPO / "scripts" / "license_scan.py"
FIXTURE = REPO / "testkit" / "fixtures" / "license_scan" / "missing_entry"


def run_scan(root: pathlib.Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCAN), "--root", str(root)],
        capture_output=True,
        text=True,
    )


def main() -> int:
    results = []

    # 1. Real repo: scan must pass.
    real = run_scan(REPO)
    results.append(
        {
            "check": "repo_scan_passes",
            "ok": real.returncode == 0,
            "exit_code": real.returncode,
            "stdout": real.stdout.strip(),
        }
    )

    # 2. Negative fixture: scan must fail and name the orphan dependency.
    neg = run_scan(FIXTURE)
    neg_report_ok = False
    names_orphan = False
    try:
        report = json.loads(neg.stdout)
        neg_report_ok = report.get("ok") is False and len(report.get("errors", [])) > 0
        names_orphan = any("orphan_dep" in e.get("path", "") for e in report.get("errors", []))
    except (json.JSONDecodeError, AttributeError):
        pass
    results.append(
        {
            "check": "missing_entry_fixture_fails",
            "ok": neg.returncode == 1 and neg_report_ok and names_orphan,
            "exit_code": neg.returncode,
            "stdout": neg.stdout.strip(),
        }
    )

    ok = all(r["ok"] for r in results)
    print(json.dumps({"ok": ok, "results": results}, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""File-size ratchet: first-party source files stay under the soft line limit.

A file may exceed the limit only with a recorded exception in
scripts/filesize_exceptions.json ({"path": "reason"}). Vendored code
(third_party/), build output, and non-source trees are out of scope.
Exit 0 when clean, 1 with a JSON report of violations otherwise.
"""

import json
import pathlib
import sys

LIMIT = 500
ROOT = pathlib.Path(__file__).resolve().parent.parent
SCOPES = ["core", "cli", "api", "ts", "formats", "testkit", "replay", "model", "editor", "tools", "scripts"]
SUFFIXES = {".cpp", ".h", ".mm", ".m", ".ts", ".py"}
EXCEPTIONS_FILE = ROOT / "scripts" / "filesize_exceptions.json"


def main() -> int:
    exceptions = {}
    if EXCEPTIONS_FILE.exists():
        exceptions = json.loads(EXCEPTIONS_FILE.read_text())
    violations = []
    for scope in SCOPES:
        base = ROOT / scope
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            if "third_party" in path.parts or "build" in path.parts:
                continue
            rel = str(path.relative_to(ROOT))
            lines = sum(1 for _ in path.open(encoding="utf-8", errors="replace"))
            if lines > LIMIT and rel not in exceptions:
                violations.append({"path": rel, "lines": lines, "limit": LIMIT})
    stale = [p for p in exceptions if not (ROOT / p).exists()]
    report = {"ok": not violations and not stale, "violations": violations, "stale_exceptions": stale}
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

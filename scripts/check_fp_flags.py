#!/usr/bin/env python3
"""check_fp_flags.py — verify no compiled target opts into fast-math.

Scaffold half of the deterministic-FP contract (cmake/DeterministicFP.cmake is
the configure-time half; m0-math-stdlib extends this into the full per-target
policy check, e.g. requiring -ffp-contract=off on every sim translation unit).

Scans a compile_commands.json for flags that change floating-point results:
fast-math bundles, unsafe-math sub-flags, and fast FMA contraction.

Output is machine-readable JSON. Usage:
    scripts/check_fp_flags.py [build/dev/compile_commands.json]
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

BANNED = re.compile(
    r"(^|\s)("
    r"-ffast-math|-Ofast|-funsafe-math-optimizations|-fassociative-math|"
    r"-freciprocal-math|-menable-unsafe-fp-math|-ffp-contract=fast|"
    r"/fp:fast|-ffinite-math-only"
    r")(\s|$)"
)


def main() -> int:
    db_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/dev/compile_commands.json")
    try:
        commands = json.loads(db_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(json.dumps({"ok": False, "errors": [{"kind": "db_unreadable", "path": str(db_path), "detail": str(exc)}]}))
        return 1

    violations = []
    for entry in commands:
        command = entry.get("command") or " ".join(entry.get("arguments", []))
        match = BANNED.search(command)
        if match:
            violations.append({"file": entry.get("file"), "flag": match.group(2)})

    report = {
        "ok": not violations,
        "compile_commands": str(db_path),
        "translation_units": len(commands),
        "violations": violations,
    }
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

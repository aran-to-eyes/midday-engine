#!/usr/bin/env python3
"""license_scan.py — enforce the third_party/ <-> LICENSES/manifest.json contract.

Spec section 15, constraint 1: every dependency is OSS-licensed and tracked in
the LICENSES/ manifest. This scan fails (exit 1) if:

  * a directory under third_party/ has no manifest entry        (unpinned code)
  * a manifest entry has no directory under third_party/        (stale pin)
  * an entry is missing required fields (name, version, commit,
    license, role, path)                                        (unusable pin)
  * an entry's license is not on the OSS allowlist              (license drift)
  * a vendored directory ships no LICENSE/COPYING file          (unverifiable)

Output is machine-readable JSON (constraint 4). Usage:
    scripts/license_scan.py [--root <repo-root>]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REQUIRED_FIELDS = ("name", "version", "commit", "license", "role", "path")

# MIT/BSD/Apache/zlib preferred; LGPL acceptable if dynamically linked (the
# scan flags LGPL entries unless they declare "linkage": "dynamic").
ALLOWED_LICENSES = re.compile(
    r"^(MIT|BSD(-[0-9]-Clause)?|Apache-2\.0|Zlib|ISC|BSL-1\.0|Unlicense|"
    r"CC0-1\.0|Public Domain|MPL-2\.0|LGPL-2\.1|LGPL-3\.0)"
)
LICENSE_FILE = re.compile(r"^(LICENSE|LICENCE|COPYING|COPYRIGHT)", re.IGNORECASE)


def scan(root: pathlib.Path) -> dict:
    errors: list[dict] = []
    manifest_path = root / "LICENSES" / "manifest.json"
    third_party = root / "third_party"

    try:
        manifest = json.loads(manifest_path.read_text())
        entries = manifest["dependencies"]
    except (OSError, json.JSONDecodeError, KeyError) as exc:
        return {
            "ok": False,
            "errors": [{"kind": "manifest_unreadable", "path": str(manifest_path), "detail": str(exc)}],
        }

    by_path = {}
    for entry in entries:
        missing = [f for f in REQUIRED_FIELDS if not entry.get(f)]
        label = entry.get("path") or entry.get("name") or "<unnamed>"
        if missing:
            errors.append({"kind": "entry_missing_fields", "path": label, "fields": missing})
            continue
        by_path[entry["path"]] = entry

        lic = entry["license"]
        if not ALLOWED_LICENSES.match(lic):
            errors.append({"kind": "license_not_allowed", "path": label, "license": lic})
        elif lic.startswith("LGPL") and entry.get("linkage") != "dynamic":
            errors.append({"kind": "lgpl_requires_dynamic_linkage", "path": label, "license": lic})

        vendored = root / entry["path"]
        if not vendored.is_dir():
            errors.append({"kind": "stale_entry_no_directory", "path": label})
        elif not any(LICENSE_FILE.match(p.name) for p in vendored.iterdir() if p.is_file()):
            errors.append({"kind": "no_license_file_in_tree", "path": label})

    if third_party.is_dir():
        for child in sorted(third_party.iterdir()):
            if not child.is_dir():
                continue
            rel = child.relative_to(root).as_posix()
            if rel not in by_path:
                errors.append({"kind": "unpinned_third_party_dir", "path": rel})

    return {
        "ok": not errors,
        "manifest": str(manifest_path.relative_to(root)),
        "pinned": sorted(by_path),
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent,
        help="repo root to scan (default: this script's repo)",
    )
    args = parser.parse_args()

    report = scan(args.root.resolve())
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

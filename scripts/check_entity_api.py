#!/usr/bin/env python3
"""check_entity_api.py — no PUBLIC code-assembled entity API (spec section 7).

Entities are BORN FROM DATA: the loader (core/loader/) is the sole content
spawn path, so a scene/prefab is the single source of truth and the same
bytes replay identically. Nothing above the runtime may assemble an entity
in code — that would be a second, invisible spawn path outside the journal
and outside the format.

This scan fails (exit 1) when a first-party source file OUTSIDE the runtime
calls the ECS structural-assembly surface directly:

  World::spawn / ->spawn / .spawn            (create an entity)
  World::queue_spawn / ->queue_spawn / .     (deferred create)
  world.emplace / world->emplace / World::   (attach a component)

`spawn` and `queue_spawn` are World-exclusive verbs, matched on any call
receiver. `emplace` is matched ONLY on a `world`/`World` receiver — the
codebase's universal name for the ECS world — so std::optional/std::variant
`.emplace()` (sim.writer.emplace, sim.bus.emplace, ...) is never a false
positive. Line (`//`) comments are stripped before matching.

SANCTIONED (may assemble):
  * anything under core/    — the runtime internals: the loader, the ECS
                             itself, the structural queue.
  * *_test.cpp              — tests legitimately build worlds by hand.
  * scripts/entity_api_exceptions.json {"path": "reason"} — an explicit,
    reasoned allowlist for the rare non-core, non-test assembler
    (currently the batch-binding throughput micro-benchmark).

Everything else (cli/, ts/, formats/, a future game/, ...) must go through
the loader / the public spawn verbs. Output is machine-readable JSON
(spec section 9). Usage:
    scripts/check_entity_api.py [--root <repo-root>]
Self-test:
    scripts/check_entity_api.py --self-test
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import tempfile

# First-party trees scanned for violations. core/ is deliberately ABSENT —
# it is the sanctioned runtime. A future game/ tree is listed ahead of need
# so the day it appears it is policed, not exempt.
SCOPES = ["cli", "ts", "formats", "replay", "model", "editor", "tools",
          "testkit", "api", "examples", "game"]
SUFFIXES = {".cpp", ".h", ".mm", ".m"}
EXCEPTIONS_FILE = "scripts/entity_api_exceptions.json"

# (rule name, call-shape regex). `spawn`/`queue_spawn` are World-exclusive so
# any call receiver counts; `emplace` requires a world/World receiver so the
# ubiquitous std::optional::emplace is not swept in.
RULES = [
    ("spawn", re.compile(r"(?:\.|->|\bWorld\s*::)\s*spawn\s*\(")),
    ("queue_spawn", re.compile(r"(?:\.|->|\bWorld\s*::)\s*queue_spawn\s*\(")),
    ("emplace", re.compile(r"(?:\bworld\s*(?:\.|->)|\bWorld\s*::)\s*emplace\s*\(")),
]

# Strip a // line comment (outside strings) so a doc line mentioning
# world.spawn( is not flagged. Good enough: these call-shapes never appear in
# block comments in first-party code, and the token set carries no quotes.
LINE_COMMENT_RE = re.compile(r"//.*$")


def is_sanctioned(rel: str, exceptions: dict) -> bool:
    return (rel.startswith("core/")
            or rel.endswith("_test.cpp")
            or rel in exceptions)


def scan(root: pathlib.Path) -> dict:
    exceptions_path = root / EXCEPTIONS_FILE
    exceptions = {}
    if exceptions_path.exists():
        exceptions = json.loads(exceptions_path.read_text(encoding="utf-8"))
    violations = []
    files_scanned = 0
    for scope in SCOPES:
        base = root / scope
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            if "third_party" in path.parts or "build" in path.parts:
                continue
            rel = path.relative_to(root).as_posix()
            files_scanned += 1
            if is_sanctioned(rel, exceptions):
                continue
            for line_number, raw in enumerate(
                    path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                line = LINE_COMMENT_RE.sub("", raw)
                for rule, pattern in RULES:
                    if pattern.search(line):
                        violations.append({
                            "rule": rule,
                            "file": rel,
                            "line": line_number,
                            "text": raw.strip(),
                        })
    stale = [p for p in exceptions if not (root / p).exists()]
    return {
        "ok": not violations and not stale,
        "files_scanned": files_scanned,
        "violations": violations,
        "stale_exceptions": stale,
    }


def self_test() -> int:
    """The scanner must FIND a planted code-assembled entity, PASS a sanctioned
    caller (test + allowlisted bench), and NOT trip over std::optional::emplace
    (falsifiability guard, license_scan negative-fixture precedent)."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        scripts = root / "scripts"
        scripts.mkdir()
        (scripts / "entity_api_exceptions.json").write_text(
            '{"ts/runtime/bench.cpp": "throughput micro-benchmark"}\n')

        cli = root / "cli" / "verbs"
        cli.mkdir(parents=True)
        # PLANTED violations: a verb assembling entities in code.
        (cli / "leak.cpp").write_text(
            "auto e = world.spawn();\n"
            "world.emplace(e, Transform{});\n"
            "sim.world->queue_spawn();\n"
            "World::spawn();  // qualified\n"
            "sim.writer.emplace(std::move(w));  // std::optional — NOT a hit\n"
            "// world.spawn() in a comment — NOT a hit\n")

        rt = root / "ts" / "runtime"
        rt.mkdir(parents=True)
        # Sanctioned: allowlisted benchmark + a test file — both assemble freely.
        (rt / "bench.cpp").write_text("auto e = world.spawn();\nworld.emplace(e, P{});\n")
        (rt / "views_test.cpp").write_text("auto e = world.spawn();\nworld.emplace(e, P{});\n")

        core = root / "core" / "loader"
        core.mkdir(parents=True)
        # Sanctioned: the runtime IS the spawn path.
        (core / "spawn.cpp").write_text("const auto e = world.spawn(&err);\n")

        report = scan(root)
        found = {(v["file"], v["rule"], v["line"]) for v in report["violations"]}
        expect = {
            ("cli/verbs/leak.cpp", "spawn", 1),
            ("cli/verbs/leak.cpp", "emplace", 2),
            ("cli/verbs/leak.cpp", "queue_spawn", 3),
            ("cli/verbs/leak.cpp", "spawn", 4),
        }
        ok = (not report["ok"]
              and found == expect            # exactly the four planted hits
              and not report["stale_exceptions"])
        print(json.dumps({"self_test_ok": ok, "report": report}, indent=2))
        return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    report = scan(pathlib.Path(args.root))
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

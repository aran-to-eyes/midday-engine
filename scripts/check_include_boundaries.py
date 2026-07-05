#!/usr/bin/env python3
"""check_include_boundaries.py — three MECHANICAL, self-testing boundaries.

Spec section 5 (RHI seam), CONCERNS #6 (core/ logging purity), and the M1-exit
Phase 1 structural CRLF guard (journal binary-mode) each ban a textual
pattern outside — or, for logging purity, inside without review — a narrow
scope. All three are pure source scans (no build): any runner, no toolchain.

1. RHI seam (spec section 5): all engine code targets the RHI, never a GPU
   API directly. Fails when a first-party source file includes a backend
   header outside the backend directory that owns it:

     Vulkan family  (vulkan/..., vulkan.h, volk.h, vk_mem_alloc.h, vk_video/...)
         -> only under core/rhi/vulkan/
     shader toolchain (glslang/..., SPIRV/..., spirv_cross/...)
         -> only under core/rhi/shadercomp/
     Metal family   (#import/#include <Metal/...>, <MetalKit/...>, <QuartzCore/...>)
         -> only under core/rhi/metal/

   Vendored code (third_party/) is out of scope; it IS the backend
   implementation detail being quarantined.

2. core/ logging purity (CONCERNS #6): core/ is machine-readable-logging-only
   (core/base/log.h — "no printf-style logging anywhere in engine code").
   Fails when a first-party file under core/** uses printf, fprintf,
   std::cout, std::cerr, or `#include <iostream>`, EXCEPT the reviewed
   call sites in scripts/core_stderr_allowlist.json ({"path": [{"line",
   "reason"}, ...]}) — each a loud-abort invariant break or a driver/test
   diagnostic that predates (or sits outside) the structured Logger. A
   planted print OUTSIDE the allowlist is a gate failure; a stale allowlist
   entry (the line no longer exists, or no longer matches a banned pattern)
   is one too, so the allowlist can never silently outlive its own code.

3. Journal binary-mode (M1-exit Phase 1 structural CRLF guard): the one
   correctly-declined Windows byte-cmp (Aurora D-9: MSVC FP drift must not
   permanent-red a cross-host compare) is offset by enforcing BY
   CONSTRUCTION that the journal reader/writer never opens a file in
   text mode, where CRLF translation would corrupt deterministic bytes.
   core/journal/** may only reach the file system through
   core/base/file_io.h's `open_file(path, "wb"/"rb")` (binary mode is
   `open_file`'s only sanctioned interior); a direct `fopen`/`std::fopen`
   call, or an `open_file` call whose mode string lacks 'b', fails the scan.

Output is machine-readable JSON (spec section 9 ethos). Usage:
    scripts/check_include_boundaries.py [--root <repo-root>]
Self-test:
    scripts/check_include_boundaries.py --self-test
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import tempfile

# First-party scopes for the RHI seam (mirrors check_file_sizes.py;
# examples/ additionally — authored corpus files must not reach around the
# seam either).
RHI_SCOPES = ["core", "cli", "api", "ts", "formats", "testkit", "replay", "model",
              "editor", "tools", "examples"]
SUFFIXES = {".cpp", ".h", ".mm", ".m"}

INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^">]+)[">]')
LINE_COMMENT_RE = re.compile(r"//.*$")

# (rule name, include-path regex, allowed directory prefix)
RHI_RULES = [
    ("vulkan", re.compile(r"^(vulkan/|vk_video/|volk\.h$|vk_mem_alloc\.h$|vulkan\.h$)"),
     "core/rhi/vulkan/"),
    ("shadercomp", re.compile(r"^(glslang/|SPIRV/|spirv_cross/|spirv[._])"),
     "core/rhi/shadercomp/"),
    ("metal", re.compile(r"^(Metal/|MetalKit/|QuartzCore/|Foundation/NSObjCRuntime)"),
     "core/rhi/metal/"),
]

# core/ logging purity (CONCERNS #6).
CORE_STDERR_ALLOWLIST_FILE = "scripts/core_stderr_allowlist.json"
STDERR_RULES = [
    ("printf", re.compile(r"\bprintf\s*\(")),
    ("fprintf", re.compile(r"\bfprintf\s*\(")),
    ("cout", re.compile(r"\b(?:std::)?cout\b")),
    ("cerr", re.compile(r"\b(?:std::)?cerr\b")),
]

# Journal binary-mode (structural CRLF guard).
JOURNAL_SCOPE = "core/journal"
DIRECT_FOPEN_RE = re.compile(r"\b(?:std::)?fopen\s*\(")
OPEN_FILE_CALL_RE = re.compile(r'\bopen_file\s*\([^,]*,\s*"([^"]*)"')


def _iter_source_files(root: pathlib.Path, scopes: list[str]):
    for scope in scopes:
        base = root / scope
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            if "third_party" in path.parts or "build" in path.parts:
                continue
            yield path


def scan_rhi(root: pathlib.Path) -> dict:
    violations = []
    files_scanned = 0
    for path in _iter_source_files(root, RHI_SCOPES):
        rel = path.relative_to(root).as_posix()
        files_scanned += 1
        for line_number, line in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            include = match.group(1)
            for rule, pattern, allowed in RHI_RULES:
                if pattern.match(include) and not rel.startswith(allowed):
                    violations.append({
                        "rule": rule,
                        "file": rel,
                        "line": line_number,
                        "include": include,
                        "allowed_under": allowed,
                    })
    return {"ok": not violations, "files_scanned": files_scanned, "violations": violations}


def _load_json(root: pathlib.Path, rel: str) -> dict:
    path = root / rel
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def scan_core_stderr(root: pathlib.Path) -> dict:
    allowlist = _load_json(root, CORE_STDERR_ALLOWLIST_FILE)
    allowed_keys = {(file, entry["line"]) for file, entries in allowlist.items() for entry in entries}
    matched_keys = set()
    violations = []
    files_scanned = 0
    for path in _iter_source_files(root, ["core"]):
        rel = path.relative_to(root).as_posix()
        files_scanned += 1
        for line_number, raw in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            line = LINE_COMMENT_RE.sub("", raw)
            hit_rule = None
            include_match = INCLUDE_RE.match(line)
            if include_match and include_match.group(1) == "iostream":
                hit_rule = "iostream"
            else:
                for rule, pattern in STDERR_RULES:
                    if pattern.search(line):
                        hit_rule = rule
                        break
            if hit_rule is None:
                continue
            key = (rel, line_number)
            if key in allowed_keys:
                matched_keys.add(key)
                continue
            violations.append({"rule": hit_rule, "file": rel, "line": line_number, "text": raw.strip()})
    stale = sorted(f"{file}:{line}" for file, line in allowed_keys if (file, line) not in matched_keys)
    return {
        "ok": not violations and not stale,
        "files_scanned": files_scanned,
        "violations": violations,
        "stale_allowlist": stale,
    }


def scan_journal_binary_mode(root: pathlib.Path) -> dict:
    violations = []
    files_scanned = 0
    for path in _iter_source_files(root, ["core"]):
        rel = path.relative_to(root).as_posix()
        if not (rel == JOURNAL_SCOPE or rel.startswith(JOURNAL_SCOPE + "/")):
            continue
        files_scanned += 1
        for line_number, raw in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            line = LINE_COMMENT_RE.sub("", raw)
            if DIRECT_FOPEN_RE.search(line):
                violations.append({
                    "rule": "direct_fopen",
                    "file": rel,
                    "line": line_number,
                    "text": raw.strip(),
                })
                continue
            mode_match = OPEN_FILE_CALL_RE.search(line)
            if mode_match and "b" not in mode_match.group(1):
                violations.append({
                    "rule": "text_mode_open_file",
                    "file": rel,
                    "line": line_number,
                    "text": raw.strip(),
                })
    return {"ok": not violations, "files_scanned": files_scanned, "violations": violations}


def full_scan(root: pathlib.Path) -> dict:
    rhi = scan_rhi(root)
    core_stderr = scan_core_stderr(root)
    journal = scan_journal_binary_mode(root)
    return {
        "ok": rhi["ok"] and core_stderr["ok"] and journal["ok"],
        "rhi_boundaries": rhi,
        "core_stderr_purity": core_stderr,
        "journal_binary_mode": journal,
    }


def _self_test_rhi(root: pathlib.Path) -> bool:
    bad = root / "core" / "renderer"
    bad.mkdir(parents=True)
    (bad / "leak.cpp").write_text('#include <vulkan/vulkan.h>\n#include "volk.h"\n')
    good = root / "core" / "rhi" / "vulkan"
    good.mkdir(parents=True)
    (good / "vk_ok.cpp").write_text("#include <volk.h>\n#include <vk_mem_alloc.h>\n")
    shader_leak = root / "cli" / "verbs"
    shader_leak.mkdir(parents=True)
    (shader_leak / "oops.cpp").write_text('#include <glslang/Public/ShaderLang.h>\n')
    metal_leak = root / "core" / "renderer"
    (metal_leak / "mtl_leak.mm").write_text('#import <Metal/Metal.h>\n')
    metal_ok = root / "core" / "rhi" / "metal"
    metal_ok.mkdir(parents=True)
    (metal_ok / "mtl_ok.mm").write_text('#import <Metal/Metal.h>\n')

    report = scan_rhi(root)
    found = {(v["file"], v["rule"]) for v in report["violations"]}
    expect = {("core/renderer/leak.cpp", "vulkan"), ("cli/verbs/oops.cpp", "shadercomp"),
              ("core/renderer/mtl_leak.mm", "metal")}
    return (not report["ok"] and expect <= found and len(report["violations"]) == 4
            and all(not v["file"].startswith("core/rhi/") for v in report["violations"]))


def _self_test_core_stderr(root: pathlib.Path) -> bool:
    scripts_dir = root / "scripts"
    scripts_dir.mkdir(exist_ok=True)
    # allowlisted.cpp:3 is fprintf (matched, suppressed); a SECOND allowlist
    # entry (line 99) never matches anything in the file — the stale-entry
    # falsification.
    (scripts_dir / "core_stderr_allowlist.json").write_text(json.dumps({
        "core/reviewed/allowlisted.cpp": [
            {"line": 3, "reason": "reviewed loud-abort site"},
            {"line": 99, "reason": "stale: this line does not exist"},
        ]
    }))

    reviewed = root / "core" / "reviewed"
    reviewed.mkdir(parents=True)
    (reviewed / "allowlisted.cpp").write_text(
        "void fatal() {\n"
        "    // fprintf(stderr, \"in a comment — NOT a hit\\n\");\n"
        "    std::fprintf(stderr, \"reviewed\\n\");\n"
        "    std::abort();\n"
        "}\n")

    stray = root / "core" / "ecs"
    stray.mkdir(parents=True)
    (stray / "leak.cpp").write_text(
        "#include <iostream>\n"
        "void debug() {\n"
        "    printf(\"stray\\n\");\n"
        "    std::cout << \"stray\\n\";\n"
        "    std::cerr << \"stray\\n\";\n"
        "}\n")

    outside = root / "cli" / "verbs"
    outside.mkdir(parents=True)
    (outside / "fine.cpp").write_text('printf("cli may print freely — out of core/ scope\\n");\n')

    report = scan_core_stderr(root)
    found = {(v["file"], v["rule"], v["line"]) for v in report["violations"]}
    expect = {
        ("core/ecs/leak.cpp", "iostream", 1),
        ("core/ecs/leak.cpp", "printf", 3),
        ("core/ecs/leak.cpp", "cout", 4),
        ("core/ecs/leak.cpp", "cerr", 5),
    }
    return (not report["ok"]
            and found == expect
            and report["stale_allowlist"] == ["core/reviewed/allowlisted.cpp:99"])


def _self_test_journal_binary_mode(root: pathlib.Path) -> bool:
    journal = root / "core" / "journal"
    journal.mkdir(parents=True)
    (journal / "writer.cpp").write_text(
        "#include \"core/base/file_io.h\"\n"
        "void open_clean() {\n"
        "    auto* f = base::open_file(path, \"wb\");\n"
        "}\n"
        "void open_text_mode() {\n"
        "    auto* f = base::open_file(path, \"w\");\n"
        "}\n"
        "void open_direct() {\n"
        "    FILE* f = std::fopen(path.c_str(), \"wb\");\n"
        "}\n"
        "// auto* f = std::fopen(path.c_str(), \"wb\"); — in a comment, NOT a hit\n")
    outside = root / "core" / "base"
    outside.mkdir(parents=True)
    (outside / "file_io.h").write_text(
        "// the sanctioned fopen call site — OUT of core/journal/ scope\n"
        "inline FILE* open_file() { return std::fopen(\"x\", \"wb\"); }\n")

    report = scan_journal_binary_mode(root)
    found = {(v["file"], v["rule"], v["line"]) for v in report["violations"]}
    expect = {
        ("core/journal/writer.cpp", "text_mode_open_file", 6),
        ("core/journal/writer.cpp", "direct_fopen", 9),
    }
    return not report["ok"] and found == expect and report["files_scanned"] == 1


def self_test() -> int:
    """Each of the three scanners must FIND its planted violation(s) and PASS
    its clean/sanctioned fixture (falsifiability guard, license_scan
    negative-fixture precedent). Independent temp trees per scanner so one
    scanner's fixtures can never leak into another's scope."""
    with tempfile.TemporaryDirectory() as tmp_rhi, \
         tempfile.TemporaryDirectory() as tmp_stderr, \
         tempfile.TemporaryDirectory() as tmp_journal:
        rhi_ok = _self_test_rhi(pathlib.Path(tmp_rhi))
        stderr_ok = _self_test_core_stderr(pathlib.Path(tmp_stderr))
        journal_ok = _self_test_journal_binary_mode(pathlib.Path(tmp_journal))
        ok = rhi_ok and stderr_ok and journal_ok
        print(json.dumps({
            "self_test_ok": ok,
            "rhi_boundaries": rhi_ok,
            "core_stderr_purity": stderr_ok,
            "journal_binary_mode": journal_ok,
        }, indent=2))
        return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    report = full_scan(pathlib.Path(args.root))
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

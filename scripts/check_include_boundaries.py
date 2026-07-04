#!/usr/bin/env python3
"""check_include_boundaries.py — the RHI seam is a MECHANICAL boundary.

Spec section 5: all engine code targets the RHI, never a GPU API directly.
This scan fails (exit 1) when a first-party source file includes a backend
header outside the backend directory that owns it:

  Vulkan family  (vulkan/..., vulkan.h, volk.h, vk_mem_alloc.h, vk_video/...)
      -> only under core/rhi/vulkan/
  shader toolchain (glslang/..., SPIRV/..., spirv_cross/...)
      -> only under core/rhi/shadercomp/
  Metal family   (#import/#include <Metal/...>, <MetalKit/...>, <QuartzCore/...>)
      -> only under core/rhi/metal/

Also enforced: the seam itself stays clean — core/rhi/*.h|cpp OUTSIDE the
backend subdirectories must match the same rules (they do: they own no
backend prefix). Vendored code (third_party/) is out of scope; it IS the
backend implementation detail being quarantined.

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

# First-party scopes (mirrors check_file_sizes.py; examples/ additionally —
# authored corpus files must not reach around the seam either).
SCOPES = ["core", "cli", "api", "ts", "formats", "testkit", "replay", "model",
          "editor", "tools", "examples"]
SUFFIXES = {".cpp", ".h", ".mm", ".m"}

INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^">]+)[">]')

# (rule name, include-path regex, allowed directory prefix)
RULES = [
    ("vulkan", re.compile(r"^(vulkan/|vk_video/|volk\.h$|vk_mem_alloc\.h$|vulkan\.h$)"),
     "core/rhi/vulkan/"),
    ("shadercomp", re.compile(r"^(glslang/|SPIRV/|spirv_cross/|spirv\.hpp)"),
     "core/rhi/shadercomp/"),
    ("metal", re.compile(r"^(Metal/|MetalKit/|QuartzCore/|Foundation/NSObjCRuntime)"),
     "core/rhi/metal/"),
]


def scan(root: pathlib.Path) -> dict:
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
            for line_number, line in enumerate(
                    path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                match = INCLUDE_RE.match(line)
                if not match:
                    continue
                include = match.group(1)
                for rule, pattern, allowed in RULES:
                    if pattern.match(include) and not rel.startswith(allowed):
                        violations.append({
                            "rule": rule,
                            "file": rel,
                            "line": line_number,
                            "include": include,
                            "allowed_under": allowed,
                        })
    return {"ok": not violations, "files_scanned": files_scanned, "violations": violations}


def self_test() -> int:
    """The scanner must FIND a planted violation and PASS a clean backend file
    (falsifiability guard, license_scan negative-fixture precedent)."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        bad = root / "core" / "renderer"
        bad.mkdir(parents=True)
        (bad / "leak.cpp").write_text('#include <vulkan/vulkan.h>\n#include "volk.h"\n')
        good = root / "core" / "rhi" / "vulkan"
        good.mkdir(parents=True)
        (good / "vk_ok.cpp").write_text("#include <volk.h>\n#include <vk_mem_alloc.h>\n")
        shader_leak = root / "cli" / "verbs"
        shader_leak.mkdir(parents=True)
        (shader_leak / "oops.cpp").write_text('#include <glslang/Public/ShaderLang.h>\n')
        report = scan(root)
        found = {(v["file"], v["rule"]) for v in report["violations"]}
        expect = {("core/renderer/leak.cpp", "vulkan"), ("cli/verbs/oops.cpp", "shadercomp")}
        ok = (not report["ok"] and expect <= found and len(report["violations"]) == 3
              and all(not v["file"].startswith("core/rhi/vulkan/")
                      for v in report["violations"]))
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

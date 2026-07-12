#!/usr/bin/env bash
# Midday Engine — the PINNED lint toolchain + the first-party lint surface,
# shared by scripts/verify.sh (the local gate, run verbatim by CI
# verify-linux) AND the tidy-libstdcxx CI lane (.github/workflows/ci.yml):
# one pin set and one TU list, never two copies that drift — the
# verify_behavioral.sh precedent (M1-exit Phase 1).
#
# Why a second consumer exists at all (CONCERNS #11): clang-tidy's dataflow
# models the stdlib headers it parses. On macOS the gate analyzes against
# libc++ (Apple ships no libstdc++ headers), so libstdc++-only findings —
# bugprone-unchecked-optional-access at m1-prefab-spawn's spawn.cpp:119 is
# the canonical escape — can never surface locally. The SAME pinned tidy
# version must therefore run against libstdc++ in CI; if the pins or the TU
# set ever forked between consumers, a lane-vs-local difference would stop
# meaning "stdlib-sensitive finding" and start meaning "toolchain drift".

CLANG_FORMAT_PIN="clang-format==19.1.7"
CLANG_TIDY_PIN="clang-tidy==19.1.0.1"
JSCPD_PIN="jscpd@5.0.11"
VENV=.venv-tools

# Idempotent: a venv already carrying both pinned binaries is reused.
bootstrap_lint_venv() {
    if [ ! -x "$VENV/bin/clang-format" ] || [ ! -x "$VENV/bin/clang-tidy" ]; then
        python3 -m venv "$VENV"
        "$VENV/bin/pip" install --quiet "$CLANG_FORMAT_PIN" "$CLANG_TIDY_PIN"
    fi
}

# The first-party C++ surface (headers + TUs). Vendored code is lint-exempt.
first_party_cxx() {
    find core cli api ts formats testkit replay model editor tools \
        \( -name '*.cpp' -o -name '*.h' \) -not -path '*/third_party/*' 2>/dev/null || true
}

# clang-tidy over every first-party TU against <build_dir>'s
# compile_commands.json. One process per TU, all cores: the serial
# invocation crossed 30 minutes on 2-core CI runners once the tree passed
# 150 TUs. xargs -P preserves the exit contract (any failing TU fails the
# step); output interleaving is acceptable — findings carry file:line.
clang_tidy_first_party() { # <build_dir>
    local build_dir="$1"
    local tus
    tus=$(first_party_cxx | grep '\.cpp$' || true)
    [ -n "$tus" ] || return 0
    local extra=()
    if [ "$(uname)" = "Darwin" ]; then
        # The pinned (non-Apple) clang-tidy has no implicit macOS sysroot.
        extra=(--extra-arg="-isysroot$(xcrun --show-sdk-path)")
    fi
    local jobs
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    echo "$tus" | xargs -n 4 -P "$jobs" \
        "$VENV/bin/clang-tidy" -p "$build_dir" --quiet "${extra[@]}"
}

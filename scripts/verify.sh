#!/usr/bin/env bash
# Midday Engine — THE gate. Green before done, green before push. Mirrors CI build-linux.
# Every step echoes; first failure stops the run (set -e).
set -euo pipefail
cd "$(dirname "$0")/.."

step() { printf '\n== verify: %s ==\n' "$1"; }

# Pinned lint toolchain (same versions locally and in CI — no formatter drift).
CLANG_FORMAT_PIN="clang-format==19.1.7"
CLANG_TIDY_PIN="clang-tidy==19.1.0.1"
JSCPD_PIN="jscpd@5.0.11"
VENV=.venv-tools
if [ ! -x "$VENV/bin/clang-format" ] || [ ! -x "$VENV/bin/clang-tidy" ]; then
    step "bootstrap pinned lint tools ($CLANG_FORMAT_PIN, $CLANG_TIDY_PIN)"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --quiet "$CLANG_FORMAT_PIN" "$CLANG_TIDY_PIN"
fi

FIRST_PARTY_CXX=$(find core cli api formats testkit replay model editor tools \
    \( -name '*.cpp' -o -name '*.h' \) -not -path '*/third_party/*' 2>/dev/null || true)

step "configure + build (dev preset)"
cmake --preset dev >/dev/null
ninja -C build/dev

if [ -n "$FIRST_PARTY_CXX" ]; then
    step "clang-format --dry-run --Werror"
    # shellcheck disable=SC2086
    "$VENV/bin/clang-format" --dry-run --Werror $FIRST_PARTY_CXX

    step "clang-tidy (compile_commands, first-party TUs)"
    FIRST_PARTY_TUS=$(echo "$FIRST_PARTY_CXX" | grep '\.cpp$' || true)
    TIDY_EXTRA=()
    if [ "$(uname)" = "Darwin" ]; then
        # The pinned (non-Apple) clang-tidy has no implicit macOS sysroot.
        TIDY_EXTRA=(--extra-arg="-isysroot$(xcrun --show-sdk-path)")
    fi
    # shellcheck disable=SC2086
    "$VENV/bin/clang-tidy" -p build/dev --quiet "${TIDY_EXTRA[@]}" $FIRST_PARTY_TUS
fi

step "selftest"
build/dev/midday selftest

step "cli envelope schema"
build/dev/midday version --json | scripts/validate_envelope.py >/dev/null

step "license scan (+ negative fixture)"
scripts/license_scan.py >/dev/null
scripts/test_license_scan.py >/dev/null

step "deterministic-FP flag scan"
scripts/check_fp_flags.py build/dev/compile_commands.json >/dev/null

step "duplication ratchet (jscpd)"
npx --yes "$JSCPD_PIN" --config .jscpd.json . >/dev/null

step "file-size ratchet (soft limit 500 lines)"
scripts/check_file_sizes.py

printf '\nverify: ALL GREEN\n'

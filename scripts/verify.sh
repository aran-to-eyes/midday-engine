#!/usr/bin/env bash
# Midday Engine — THE gate. Green before done, green before push. CI's
# verify-linux job runs this file VERBATIM. NOTE the one intended local/CI
# difference: on macOS clang-tidy parses libc++ (Apple ships no libstdc++),
# so stdlib-sensitive findings are CI-authoritative — verify-linux plus the
# fast tidy-libstdcxx lane (CONCERNS #11). A local green does not preempt a
# Linux tidy red.
# Every step echoes; first failure stops the run (set -e).
#
# Behavioral steps (everything driven through the built binary — selftest
# through golden-compare) live in scripts/lib/verify_behavioral.sh, sourced
# below. That file is the SAME behavioral core `.github/workflows/ci.yml`'s
# `build-windows` job sources under Git-Bash for its D-9 subset (M1-exit
# Phase 1, "asymmetric gate geography" cure) — one shared core, not two
# hand-kept copies that drift. Everything that stays directly in THIS file
# is Linux/macOS-authoritative: the pinned build + clang-format + clang-tidy,
# the license/FP/boundary scans, jscpd, and the file-size ratchet.
set -euo pipefail
cd "$(dirname "$0")/.."

step() { printf '\n== verify: %s ==\n' "$1"; }

# Pinned lint toolchain + first-party lint surface: scripts/lib/verify_lint.sh
# is the single source, shared with the tidy-libstdcxx CI lane (same
# verify_behavioral.sh discipline — one core, no drifting copies).
# shellcheck source=scripts/lib/verify_lint.sh
source scripts/lib/verify_lint.sh
if [ ! -x "$VENV/bin/clang-format" ] || [ ! -x "$VENV/bin/clang-tidy" ]; then
    step "bootstrap pinned lint tools ($CLANG_FORMAT_PIN, $CLANG_TIDY_PIN)"
fi
bootstrap_lint_venv

FIRST_PARTY_CXX=$(first_party_cxx)

step "configure + build (dev preset)"
cmake --preset dev >/dev/null
ninja -C build/dev

if [ -n "$FIRST_PARTY_CXX" ]; then
    step "clang-format --dry-run --Werror"
    # shellcheck disable=SC2086
    "$VENV/bin/clang-format" --dry-run --Werror $FIRST_PARTY_CXX

    step "clang-tidy (compile_commands, first-party TUs, parallel)"
    clang_tidy_first_party build/dev
fi

# shellcheck source=scripts/lib/verify_behavioral.sh
source scripts/lib/verify_behavioral.sh
BIN=build/dev/midday
BUILD_DIR=build/dev

step "selftest"
behavioral_selftest "$BIN"

step "cli envelope schema"
behavioral_envelope "$BIN"

step "journal fixture (byte-pinned bundle + real zstdcat greppability)"
behavioral_journal_fixture "$BIN" "$BUILD_DIR"

step "engine_api drift (two dumps byte-compared + committed artifact + meta-schema)"
# Determinism is proven by two independent dumps diffed (never a self-diff),
# then the committed artifact is byte-compared against a regeneration and
# schema-validated; `api diff` against the committed file must report
# identical (exit 0). Any drift = regenerate + commit, or you broke the API.
behavioral_api_dump_self_consistency "$BIN" "$BUILD_DIR"
behavioral_api_drift_vs_committed "$BIN" "$BUILD_DIR"

step "codegen drift (selfhost authoritative: dual runs + committed artifacts + meta-schema)"
behavioral_codegen_drift "$BIN" "$BUILD_DIR"

step "codegen byte-equivalence (selfhost vs TEMPORARY bootstrap, standing gate until retirement)"
behavioral_codegen_equivalence "$BIN" "$BUILD_DIR"

step "script toolchain (fixtures through the real CLI: exit classes, cache, lint)"
behavioral_script_toolchain "$BIN" "$BUILD_DIR"

step "ts components (m1-ts-components: warden components typecheck; @component/@field extract WITHOUT executing)"
behavioral_ts_components "$BIN" "$BUILD_DIR"

step "batch-binding budgets (1k/10k/100k sweep + naive ratio, m0-batch-bindings exit tests)"
behavioral_batch_budgets_sweep "$BIN" "$BUILD_DIR"

step "yaml loader + run (boss corpus: flight journal from run 1, same-seed dual-run diff)"
behavioral_boss_corpus "$BIN" "$BUILD_DIR"

step "schema validate + fmt (m1-strict-yaml: mutation corpus + fmt idempotence)"
behavioral_schema_validate_fmt "$BIN" "$BUILD_DIR"

step "events format (m1-events-format: project-wide namespace, collisions, undefined refs, wrong-payload halt)"
behavioral_events_format "$BIN" "$BUILD_DIR"

step "input actions (m1-input-actions: synthetic injection journaled at the injected tick, get_vector numeric fixture, project-wide conflict validation)"
behavioral_input_actions "$BIN" "$BUILD_DIR"

step "uid system (m1-uid-system: check --fix repairs drift + mints; a hand-minted uid refuses; the cache regenerates byte-identical)"
behavioral_uid_system "$BIN" "$BUILD_DIR"

step "project scaffold (m1-project-new: midday new + validate + no-editor-truth)"
behavioral_project_scaffold "$BIN" "$BUILD_DIR"

step "scene format (m1-scene-format: on:->Transition round-trip, override-by-name, Warden parse+report, formats[])"
behavioral_scene_format "$BIN" "$BUILD_DIR"

step "warden contract audit (m1-warden-contract-audit: known-completion manifest, exact-set assertion)"
behavioral_warden_audit "$BIN" "$BUILD_DIR"

step "prefab spawn (m1-prefab-spawn: 100-prefab mid-tick spawn + enter-chain trace, "\
"despawn-mid-query, alive-after-phase-8, world.spawn/despawn TS boundary)"
behavioral_prefab_spawn "$BIN" "$BUILD_DIR"

step "appendix A golden (3200-tick assert pack + independent dual-run diff)"
# m0-appendix-a-determinism exit tests: the flagship golden — the authored
# A.3 corpus driven to tick 3200 with the assertion pack; the five item-21
# verdicts hold; two INDEPENDENT runs diff identical (never a self-diff).
# The Linux CI determinism lane adds the dual-host x3 sha256 byte-compare;
# on this (possibly non-Linux) host the diff is the semantic gate. SHARED
# with Windows's D-9 lane (ci.yml build-windows) via the same function.
behavioral_appendix_a_golden "$BIN" "$BUILD_DIR"

step "determinism kata (600-tick exercised asserts + dual-run compare + tainted lint gate)"
# m0-determinism-spike exit tests (MILESTONE_0 item 25, Zenith D024): the
# kata must move TS GC churn + Jolt stepping + statechart cascades +
# sequence spans SIMULTANEOUSLY. The semantic core (asserts + journal diff)
# is SHARED with Windows's D-9 lane; the raw zstdcat byte-compare and the
# wall-clock-taint lint gate are extra Linux/macOS-only reinforcement.
behavioral_determinism_kata_semantic "$BIN" "$BUILD_DIR"
behavioral_determinism_kata_byte_reinforce "$BUILD_DIR"
behavioral_tainted_lint_gate "$BIN" "$BUILD_DIR"

step "license scan (+ negative fixture)"
scripts/license_scan.py >/dev/null
scripts/test_license_scan.py >/dev/null

step "deterministic-FP flag scan"
scripts/check_fp_flags.py build/dev/compile_commands.json >/dev/null

step "boundary scanners (self-test + scan: RHI seam, core/ logging purity, journal binary-mode)"
# Three mechanical, self-testing boundary rules live in one scanner (spec
# section 5's RHI seam; #6's core/ logging-purity allowlist,
# scripts/core_stderr_allowlist.json; the structural CRLF guard banning
# text-mode fopen in the journal writer/reader). Each self-test proves its
# rule still catches a planted violation before the clean scan is trusted
# (license_scan negative-fixture precedent).
scripts/check_include_boundaries.py --self-test >/dev/null
scripts/check_include_boundaries.py >/dev/null

step "entity-API boundary (self-test + scan: no code-assembled entities, spec section 7)"
# Entities are born from data (the loader is the sole spawn path); nothing above
# the runtime may call World::spawn/queue_spawn/emplace directly. The self-test
# proves the scanner still catches a planted assembler before the clean scan is
# trusted (same falsifiability contract as the include boundary above).
scripts/check_entity_api.py --self-test >/dev/null
scripts/check_entity_api.py >/dev/null

step "golden compare (fixture regen byte-compare + two-tier exit tests via the CLI)"
behavioral_golden_compare "$BIN" "$BUILD_DIR"

step "duplication ratchet (jscpd)"
npx --yes "$JSCPD_PIN" --config .jscpd.json . >/dev/null

step "file-size ratchet (soft limit 500 lines)"
scripts/check_file_sizes.py

printf '\nverify: ALL GREEN\n'

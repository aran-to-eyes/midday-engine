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

FIRST_PARTY_CXX=$(find core cli api ts formats testkit replay model editor tools \
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

step "journal fixture (byte-pinned bundle + real zstdcat greppability)"
# Regenerate the greppable bundle from scratch and byte-compare it against the
# committed fixture (journal writer determinism, pinned across platforms),
# then run the actual zstdcat|grep the spec promises (zstd is a declared host
# tool, Aurora D-22).
rm -rf build/dev/journal_fixture.mrj
MIDDAY_JOURNAL_FIXTURE_DIR=build/dev/journal_fixture.mrj \
    build/dev/midday selftest --filter 'journal.greppability' >/dev/null
for f in header.json index.json journal.jsonl.zst; do
    cmp "testkit/fixtures/journal/greppable.mrj/$f" "build/dev/journal_fixture.mrj/$f"
done
zstdcat testkit/fixtures/journal/greppable.mrj/journal.jsonl.zst | grep -q known_event

step "engine_api drift (two dumps byte-compared + committed artifact + meta-schema)"
# Determinism is proven by two independent dumps diffed (never a self-diff),
# then the committed artifact is byte-compared against a regeneration and
# schema-validated; `api diff` against the committed file must report
# identical (exit 0). Any drift = regenerate + commit, or you broke the API.
build/dev/midday api dump --out build/dev/engine_api.json >/dev/null
build/dev/midday api dump --out build/dev/engine_api.rerun.json >/dev/null
cmp build/dev/engine_api.json build/dev/engine_api.rerun.json
cmp api/engine_api.json build/dev/engine_api.json
scripts/validate_envelope.py formats/engine_api.schema.json <api/engine_api.json >/dev/null
build/dev/midday api diff api/engine_api.json >/dev/null

step "codegen drift (selfhost authoritative: dual runs + committed artifacts + meta-schema)"
# The SELF-HOSTED generator (m0-codegen-selfhost) is authoritative: two
# independent `midday api codegen` runs are cmp'd (never a self-diff), then
# the four committed api/ artifacts are byte-compared against a selfhost
# regeneration and schema_manifest.json is validated against its meta-schema.
# Any drift = rerun `build/dev/midday api codegen` from the repo root and
# commit, or you broke the generator contract (api/CODEGEN.md).
rm -rf build/dev/codegen build/dev/codegen.rerun
build/dev/midday api codegen api/engine_api.json --out-dir build/dev/codegen \
    --cache-dir build/dev/ts-cache.codegen >/dev/null
build/dev/midday api codegen api/engine_api.json --out-dir build/dev/codegen.rerun \
    --cache-dir build/dev/ts-cache.codegen >/dev/null
for f in engine.d.ts schema_manifest.json api_docs.md bindings_spec.json; do
    cmp "build/dev/codegen/$f" "build/dev/codegen.rerun/$f"
    cmp "api/$f" "build/dev/codegen/$f"
done
scripts/validate_envelope.py formats/schema_manifest.schema.json <api/schema_manifest.json >/dev/null

step "codegen byte-equivalence (selfhost vs TEMPORARY bootstrap, standing gate until retirement)"
# MILESTONE_0 "Codegen Bootstrap": the self-hosted generator became
# authoritative only because it byte-matches the native bootstrap; this gate
# keeps that claim live on the full corpus (doctests cover synthetic +
# number-edge + live; this covers the committed document through the CLI).
build/dev/midday api codegen --verify-equivalence --cache-dir build/dev/ts-cache.codegen >/dev/null

step "script toolchain (fixtures through the real CLI: exit classes, cache, lint)"
# ts_hello: clean check (exit 0); dual INDEPENDENT builds byte-compared
# (never a self-diff); second run reports zero re-transpiles. Cache dirs are
# regenerable build output — never drift-gated (.midday-cache/ is gitignored
# for ad-hoc runs; verify uses build/dev).
rm -rf build/dev/ts-cache.a build/dev/ts-cache.b
build/dev/midday script check testkit/fixtures/ts/hello.ts --cache-dir build/dev/ts-cache.a --json \
    | jq -e '.ok and .diagnostics == []' >/dev/null
build/dev/midday script build testkit/fixtures/ts/hello.ts --cache-dir build/dev/ts-cache.a --stats --json \
    | jq -e '.ok and .cache_hit == false and .stats.transpiled == 1' >/dev/null
build/dev/midday script build testkit/fixtures/ts/hello.ts --cache-dir build/dev/ts-cache.a --stats --json \
    | jq -e '.ok and .cache_hit == true and .stats == {"transpiled":0,"cache_hits":1}' >/dev/null
build/dev/midday script build testkit/fixtures/ts/hello.ts --cache-dir build/dev/ts-cache.b --json >/dev/null
cmp build/dev/ts-cache.a/*.js build/dev/ts-cache.b/*.js
# type_error: exit 3 with a structured file:line diagnostic
SCRIPT_STATUS=0
SCRIPT_OUT=$(build/dev/midday script check testkit/fixtures/ts/type_error.ts \
    --cache-dir build/dev/ts-cache.a --json) || SCRIPT_STATUS=$?
[ "$SCRIPT_STATUS" -eq 3 ]
echo "$SCRIPT_OUT" | jq -e '.error.code == "script.type_error"
    and .diagnostics[0].line > 0 and (.diagnostics[0].file | endswith("type_error.ts"))' >/dev/null
# lint pack: exit 3; all three rule families, each hit located at file:line
SCRIPT_STATUS=0
SCRIPT_OUT=$(build/dev/midday script check testkit/fixtures/ts/lint_violations.ts \
    --cache-dir build/dev/ts-cache.a --json) || SCRIPT_STATUS=$?
[ "$SCRIPT_STATUS" -eq 3 ]
echo "$SCRIPT_OUT" | jq -e '.error.code == "script.lint"
    and ([.diagnostics[].code] | unique) == ["no-timer","no-unseeded-random","no-wall-clock"]
    and ([.diagnostics[] | select(.line > 0 and .col > 0)] | length) == 6' >/dev/null

step "batch-binding budgets (1k/10k/100k sweep + naive ratio, m0-batch-bindings exit tests)"
# Crossings stay <= 8 * pool_count AND constant across the sweep (O(pools),
# never O(entities)); steady-state pooled-math ticks allocate ZERO GC bytes.
# Naive per-field mode must cross >= 10x more (per tick, tick-invariant).
BATCH_CROSSINGS=""
for N in 1000 10000 100000; do
    BENCH_OUT=$(build/dev/midday script bench --entities "$N" --ticks 60 \
        --cache-dir build/dev/ts-cache.a --json)
    echo "$BENCH_OUT" | jq -e '.ok and .mode=="batched" and .crossings_constant
        and .boundary_crossings_per_tick <= 8*.pool_count
        and .gc_alloc_bytes_per_tick==0' >/dev/null
    N_CROSSINGS=$(echo "$BENCH_OUT" | jq '.boundary_crossings_per_tick')
    if [ -z "$BATCH_CROSSINGS" ]; then BATCH_CROSSINGS="$N_CROSSINGS"; fi
    [ "$N_CROSSINGS" -eq "$BATCH_CROSSINGS" ]
done
build/dev/midday script bench --naive --entities 1000 --ticks 10 \
    --cache-dir build/dev/ts-cache.a --json \
    | jq -e ".ok and .mode==\"naive\" and .crossings_constant
        and .boundary_crossings_per_tick >= 10*$BATCH_CROSSINGS" >/dev/null

step "yaml loader + run (boss corpus: flight journal from run 1, same-seed dual-run diff)"
# m0-yaml-loader-run exit tests: the authored Appendix A corpus loads and
# runs headless with a FLIGHT bundle; two INDEPENDENT same-seed runs diff
# identical through `midday journal diff` (never a self-diff). The full
# A.3 assertion suite arrives at m0-appendix-a-determinism.
rm -rf build/m0
build/dev/midday run examples/appendix_a/boss.scene.yaml --to-tick 100 --seed 7 \
    --record build/m0/a.mrj --cache-dir build/dev/ts-cache.a --json \
    | jq -e '.ok and .ticks==100 and .recorded_tier=="flight"' >/dev/null
build/dev/midday run examples/appendix_a/boss.scene.yaml --to-tick 100 --seed 7 \
    --record build/m0/b.mrj --cache-dir build/dev/ts-cache.a --json >/dev/null
build/dev/midday journal diff build/m0/a.mrj build/m0/b.mrj --json \
    | jq -e '.first_divergent_tick==null and .identical' >/dev/null
# Strictness is the product: an unknown YAML key exits 3 with file/line.
LOADER_STATUS=0
LOADER_OUT=$(printf 'format: 1\nscene: s\nentitiez: []\n' > build/m0/bad.scene.yaml \
    && build/dev/midday run build/m0/bad.scene.yaml --json) || LOADER_STATUS=$?
[ "$LOADER_STATUS" -eq 3 ]
echo "$LOADER_OUT" | jq -e '.error.code == "loader.unknown_key"
    and .error.details.line == 3 and (.error.message | contains("entitiez"))' >/dev/null

step "appendix A golden (3200-tick assert pack + independent dual-run diff)"
# m0-appendix-a-determinism exit tests: the flagship golden — the authored
# A.3 corpus driven to tick 3200 with the assertion pack; the five item-21
# verdicts hold; two INDEPENDENT runs diff identical (never a self-diff).
# The Linux CI determinism lane adds the dual-host x3 sha256 byte-compare;
# on this (possibly non-Linux) host the diff is the semantic gate.
rm -rf build/golden
build/dev/midday run examples/appendix_a/boss.scene.yaml --to-tick 3200 --seed 7 \
    --record build/golden/a1.mrj --cache-dir build/dev/ts-cache.a \
    --assert case=appendix_a_golden --json \
    | jq -e '.ok and .assertions.combat_transitions_at_3200==1
        and .assertions.hurtbox_inactive_before_dead_enter
        and .assertions.voided_stagger
        and .assertions.locomotion_still_chasing
        and .assertions.cause_chain_complete' >/dev/null
build/dev/midday run examples/appendix_a/boss.scene.yaml --to-tick 3200 --seed 7 \
    --record build/golden/a2.mrj --cache-dir build/dev/ts-cache.a \
    --assert case=appendix_a_golden --json >/dev/null
build/dev/midday journal diff build/golden/a1.mrj build/golden/a2.mrj --json \
    | jq -e '.first_divergent_tick==null and .identical' >/dev/null

step "license scan (+ negative fixture)"
scripts/license_scan.py >/dev/null
scripts/test_license_scan.py >/dev/null

step "deterministic-FP flag scan"
scripts/check_fp_flags.py build/dev/compile_commands.json >/dev/null

step "RHI include boundaries (self-test + scan)"
# The seam is mechanical (spec section 5): no vulkan/volk/VMA include outside
# core/rhi/vulkan/, no glslang/SPIRV outside core/rhi/shadercomp/, no Metal
# outside core/rhi/metal/. The self-test proves the scanner still catches
# planted violations before the clean scan is trusted (license_scan ethos).
scripts/check_include_boundaries.py --self-test >/dev/null
scripts/check_include_boundaries.py >/dev/null

step "golden compare (fixture regen byte-compare + two-tier exit tests via the CLI)"
# m0-golden-compare exit tests: the committed triplet under
# testkit/fixtures/goldens/ is regenerated from scratch and byte-compared
# (generator + encoder determinism, journal greppable.mrj precedent), then
# all three pairs run through the REAL `midday shot compare`.
rm -rf build/dev/compare_fixtures
MIDDAY_COMPARE_FIXTURE_DIR="$PWD/build/dev/compare_fixtures" \
    build/dev/midday selftest --filter 'compare.fixtures' >/dev/null
for f in base.png identical.png noise.png shifted.png; do
    cmp "testkit/fixtures/goldens/$f" "build/dev/compare_fixtures/$f"
done
# identical pair: different FILE bytes, same pixels (Aurora D-14 in committed
# form) -> hash pass + tolerance pass, exit 0.
if cmp -s testkit/fixtures/goldens/base.png testkit/fixtures/goldens/identical.png; then
    echo "identical.png must differ from base.png at the byte level (D-14 pin)"; exit 1
fi
build/dev/midday shot compare testkit/fixtures/goldens/base.png \
    testkit/fixtures/goldens/identical.png --json \
    | jq -e '.ok and .hash_equal and .tolerance.pass and .pass
        and .pixel_hash_a == .pixel_hash_b' >/dev/null
# 1-LSB noise: hash FAIL + tolerance PASS -> exit 0 with hash_equal:false
# reported (the caller chooses which tier gates).
build/dev/midday shot compare testkit/fixtures/goldens/base.png \
    testkit/fixtures/goldens/noise.png --json \
    | jq -e '.ok and (.hash_equal | not) and .tolerance.pass and .pass
        and .tolerance.max_channel_delta == 1' >/dev/null
# structural: both tiers FAIL -> exit 1, diff.png emitted, verdicts in JSON.
SHOT_STATUS=0
SHOT_OUT=$(build/dev/midday shot compare testkit/fixtures/goldens/base.png \
    testkit/fixtures/goldens/shifted.png --diff build/dev/compare_diff.png --json) \
    || SHOT_STATUS=$?
[ "$SHOT_STATUS" -eq 1 ]
echo "$SHOT_OUT" | jq -e '(.ok | not) and .error.code == "shot.mismatch"
    and (.hash_equal | not) and (.tolerance.pass | not) and (.pass | not)
    and .tolerance.pct_pixels_over > 0' >/dev/null
test -s build/dev/compare_diff.png

step "duplication ratchet (jscpd)"
npx --yes "$JSCPD_PIN" --config .jscpd.json . >/dev/null

step "file-size ratchet (soft limit 500 lines)"
scripts/check_file_sizes.py

printf '\nverify: ALL GREEN\n'

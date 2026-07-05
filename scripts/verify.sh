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

    step "clang-tidy (compile_commands, first-party TUs, parallel)"
    FIRST_PARTY_TUS=$(echo "$FIRST_PARTY_CXX" | grep '\.cpp$' || true)
    TIDY_EXTRA=()
    if [ "$(uname)" = "Darwin" ]; then
        # The pinned (non-Apple) clang-tidy has no implicit macOS sysroot.
        TIDY_EXTRA=(--extra-arg="-isysroot$(xcrun --show-sdk-path)")
    fi
    # One process per TU, all cores: the serial invocation crossed 30 minutes
    # on 2-core CI runners once the tree passed 150 TUs. xargs -P preserves
    # the exit contract (any failing TU fails the step); output interleaving
    # is acceptable — findings carry file:line.
    JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    # shellcheck disable=SC2086
    echo "$FIRST_PARTY_TUS" | xargs -n 4 -P "$JOBS" \
        "$VENV/bin/clang-tidy" -p build/dev --quiet "${TIDY_EXTRA[@]}"
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

step "ts components (m1-ts-components: warden components typecheck; @component/@field extract WITHOUT executing)"
# Exit-test #1: the two authored Warden components typecheck against the
# generated engine.d.ts (incl. TriggerEntered). Exit-test #2: their schema
# extracts STATICALLY from the AST into a PROJECT manifest (never
# api/schema_manifest.json — that stays engine-only). The extract-throws
# fixture proves the walk never runs the code: its top-level detonate() would
# emit+throw if executed, yet Dangerous extracts cleanly. (Exit-test #3 —
# this.emit own-key + stale-ref despawn tick/site — is the component_host
# doctests, run by selftest above.)
rm -f build/dev/health.components.json build/dev/dangerous.components.json
build/dev/midday script check examples/warden/components/health.ts \
    --cache-dir build/dev/ts-cache.components --json | jq -e '.ok' >/dev/null
build/dev/midday script check examples/warden/components/damage_on_touch.ts \
    --cache-dir build/dev/ts-cache.components --json | jq -e '.ok' >/dev/null
build/dev/midday script extract examples/warden/components/health.ts \
    --out build/dev/health.components.json --cache-dir build/dev/ts-cache.components --json \
    | jq -e '.ok and .components == 1' >/dev/null
jq -e '.format_version == 1 and .components[0].name == "Health"
    and (.components[0].fields | map(.name)) == ["max","value"]
    and .components[0].fields[0].type == "float" and .components[0].fields[0].min == 0
    and (.components[0].methods[0].params | map(.type)) == ["float","entity_ref"]' \
    build/dev/health.components.json >/dev/null
build/dev/midday script extract testkit/fixtures/ts/component_extract_throws.ts \
    --out build/dev/dangerous.components.json --cache-dir build/dev/ts-cache.components --json \
    | jq -e '.ok and .components == 1' >/dev/null
jq -e '.components[0].name == "Dangerous"' build/dev/dangerous.components.json >/dev/null

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

step "schema validate + fmt (m1-strict-yaml: mutation corpus + fmt idempotence)"
# m1-strict-yaml exit tests: the generic schema engine (core/loader/
# format_schema.h) refuses six mutation classes at exit 3 with a
# file:line[:col] diagnostic — four through the engine itself (wrong type,
# unknown key, future format, bad enum) and two it only SURFACES (duplicate
# key, alias: the strict parser's own yaml.strict refusal, reused rather
# than reimplemented, over the self-contained testkit/fixtures/schema
# widget fixture — NOT a scene/machine schema, m1-scene-format's job).
WIDGET_SCHEMA=testkit/fixtures/schema/widget.schema.json
build/dev/midday validate testkit/fixtures/schema/valid_v2.widget.yaml \
    --schema-file "$WIDGET_SCHEMA" --json \
    | jq -e '.ok and .format_version.authored==2 and (.format_version.migrated|not)' >/dev/null
# The migration registry: a v1 document (count:) renames forward to v2's
# amount: and validates clean against the CURRENT schema.
build/dev/midday validate testkit/fixtures/schema/valid_v1.widget.yaml \
    --schema-file "$WIDGET_SCHEMA" --json \
    | jq -e '.ok and .format_version.authored==1 and .format_version.current==2
        and .format_version.migrated' >/dev/null

assert_widget_refusal() { # <fixture-stem> <expected error.code>
    STATUS=0
    OUT=$(build/dev/midday validate "testkit/fixtures/schema/$1.widget.yaml" \
        --schema-file "$WIDGET_SCHEMA" --json) || STATUS=$?
    [ "$STATUS" -eq 3 ]
    echo "$OUT" | jq -e --arg code "$2" \
        '(.ok | not) and .error.code == $code and (.error.message | test(":[0-9]+:[0-9]+: "))' \
        >/dev/null
}
assert_widget_refusal mutation_wrong_type loader.bad_value
assert_widget_refusal mutation_unknown_key loader.unknown_key
assert_widget_refusal mutation_future_format loader.bad_format
assert_widget_refusal mutation_bad_enum schema.bad_enum
assert_widget_refusal mutation_duplicate_key yaml.strict
assert_widget_refusal mutation_alias yaml.strict
# midday fmt refuses the SAME malformed YAML identically (schema-agnostic).
FMT_STATUS=0
FMT_OUT=$(build/dev/midday fmt testkit/fixtures/schema/mutation_alias.widget.yaml --json) \
    || FMT_STATUS=$?
[ "$FMT_STATUS" -eq 3 ]
echo "$FMT_OUT" | jq -e '.error.code == "yaml.strict"' >/dev/null

# fmt(fmt(x)) == fmt(x) for every strict-YAML format fixture in the tree —
# scene/machine/events (real content, not invented for this node) plus the
# schema engine's own widget fixtures. The canonical emitter
# (core/loader/yaml_emit.h) never consults a schema.
rm -rf build/dev/fmt_idem
mkdir -p build/dev/fmt_idem
FMT_FIXTURES=(
    examples/appendix_a/boss.scene.yaml
    examples/appendix_a/boss.machine.yaml
    examples/appendix_a/boss.events.yaml
    examples/spikes/determinism.scene.yaml
    examples/spikes/kata.machine.yaml
    examples/spikes/kata.events.yaml
    examples/spikes/tainted/tainted.scene.yaml
    examples/spikes/tainted/tainted.machine.yaml
    examples/warden/events/combat.events.yaml
    testkit/fixtures/events/wrong_payload.events.yaml
    testkit/fixtures/events/wrong_payload.machine.yaml
    testkit/fixtures/events/wrong_payload.scene.yaml
    testkit/fixtures/schema/valid_v1.widget.yaml
    testkit/fixtures/schema/valid_v2.widget.yaml
    testkit/fixtures/input/clean/a.input.yaml
    testkit/fixtures/input/clean/b.input.yaml
    testkit/fixtures/input/conflict/a.input.yaml
    testkit/fixtures/input/conflict/b.input.yaml
    testkit/fixtures/input/profile/base.input.yaml
    testkit/fixtures/input/profile/rebind.input_profile.yaml
    testkit/fixtures/input/profile/rebind_conflict.input_profile.yaml
)
for f in "${FMT_FIXTURES[@]}"; do
    name=$(basename "$f")
    build/dev/midday fmt "$f" >"build/dev/fmt_idem/$name.once"
    build/dev/midday fmt "build/dev/fmt_idem/$name.once" >"build/dev/fmt_idem/$name.twice"
    cmp "build/dev/fmt_idem/$name.once" "build/dev/fmt_idem/$name.twice"
done
# --write / --check round-trip on a scratch copy (not the committed fixture).
cp testkit/fixtures/schema/valid_v2.widget.yaml build/dev/fmt_idem/scratch.widget.yaml
build/dev/midday fmt build/dev/fmt_idem/scratch.widget.yaml --write --json \
    | jq -e '.ok and .changed' >/dev/null
build/dev/midday fmt build/dev/fmt_idem/scratch.widget.yaml --check --json \
    | jq -e '.ok and .canonical' >/dev/null

step "events format (m1-events-format: project-wide namespace, collisions, undefined refs, wrong-payload halt)"
# m1-events-format exit tests, extension-dispatch validate (no --schema flag:
# core/loader/loader.h load_project_events walks the target file's own
# directory for every *.events.yaml under it — the project-wide namespace).
# 1) the Warden corpus file validates (brought into canonical-type
#    conformance: entity_ref/vec3/float + format: 1, this node's real edit).
build/dev/midday validate examples/warden/events/combat.events.yaml --json \
    | jq -e '.ok and .schema=="events" and .events==8' >/dev/null
# Cross-file collision: two files under ONE root declaring the SAME event
# name refuse (loader.duplicate) even though neither lists the other —
# the deliverable's "collision checks" made real and CLI-observable.
rm -rf build/dev/events_probe
mkdir -p build/dev/events_probe/dup build/dev/events_probe/clean
printf 'format: 1\nevents: {dup.name: {}}\n' >build/dev/events_probe/dup/a.events.yaml
printf 'format: 1\nevents: {dup.name: {}}\n' >build/dev/events_probe/dup/b.events.yaml
EVENTS_STATUS=0
EVENTS_OUT=$(build/dev/midday validate build/dev/events_probe/dup/a.events.yaml --json) \
    || EVENTS_STATUS=$?
[ "$EVENTS_STATUS" -eq 3 ]
echo "$EVENTS_OUT" | jq -e '.error.code == "loader.duplicate"
    and (.error.message | contains("dup.name")) and (.error.message | test(":[0-9]+:[0-9]+: "))' \
    >/dev/null
# The happy multi-file path: two DIFFERENT-named files under one root merge
# into a single reported vocabulary (positive proof the namespace spans
# files, not just a single one).
printf 'format: 1\nevents: {a.one: {}}\nkeys: [squad]\n' >build/dev/events_probe/clean/a.events.yaml
printf 'format: 1\nevents: {b.two: {payload: {x: float}}}\n' >build/dev/events_probe/clean/b.events.yaml
build/dev/midday validate build/dev/events_probe/clean/a.events.yaml --json \
    | jq -e '.ok and (.files | length)==2 and .events==2 and .groups==1' >/dev/null
# 2) an undefined event reference in a machine's pairs exits 3 with a
#    structured file:line diagnostic (loader.bad_ref).
printf 'format: 1\nmachine: bad\nregions:\n  main:\n    initial: Idle\n    states:\n      Idle:\n        on:\n          - {event: nope.undefined, goto: Idle}\n' \
    >build/dev/events_probe/bad.machine.yaml
printf 'format: 1\nscene: bad\nentities:\n  - entity: E\n    machines:\n      - {instance: {path: bad.machine.yaml}}\n' \
    >build/dev/events_probe/bad.scene.yaml
RUN_STATUS=0
RUN_OUT=$(build/dev/midday run build/dev/events_probe/bad.scene.yaml --ticks 1 --json) \
    || RUN_STATUS=$?
[ "$RUN_STATUS" -eq 3 ]
echo "$RUN_OUT" | jq -e '.error.code == "loader.bad_ref"
    and (.error.message | contains("nope.undefined")) and .error.details.line > 0' >/dev/null
# 3) a dev build triggering the declared event's WRONG PAYLOAD TYPE halts
#    at the offending tick with a structured error: the bus refuses the
#    mistyped trigger (bus.payload_invalid), the refusal throws into the
#    state script (this.emit sugar's __midday_emit seam), and the run host
#    surfaces it tick-annotated (script.exception, exit 1 — a runtime
#    halt, not an authored-text refusal).
rm -rf build/dev/ts-cache.events
HALT_STATUS=0
HALT_OUT=$(build/dev/midday run testkit/fixtures/events/wrong_payload.scene.yaml --ticks 5 \
    --cache-dir build/dev/ts-cache.events --json) || HALT_STATUS=$?
[ "$HALT_STATUS" -eq 1 ]
echo "$HALT_OUT" | jq -e '.error.code == "script.exception"
    and (.error.message | contains("bus.payload_invalid"))
    and (.error.details.tick // -1) >= 0' >/dev/null

step "input actions (m1-input-actions: synthetic injection journaled at the injected tick, get_vector numeric fixture, project-wide conflict validation)"
# Exit-test #1 (a synthetic input at tick 42 triggers action.pressed at tick
# 42, journaled as a root record — core/input/inject_test.cpp) and exit-test
# #2 (the virtual-stick get_vector numeric fixture, hand-computed against the
# Godot InputMap::get_vector formula — core/input/action_state_test.cpp) are
# pure doctests; named here explicitly (they already ran once, unfiltered,
# in the "selftest" step above).
build/dev/midday selftest --filter 'input.*' >/dev/null

# Exit-test #3: two DIFFERENT actions anywhere under a project root binding
# the SAME (device, control) refuse "input.conflict", exit 3 — the cross-file
# case (testkit/fixtures/input/conflict/{a,b}.input.yaml, neither lists the
# other; the collision check runs from either file's own directory,
# load_project_input, the load_project_events precedent).
build/dev/midday validate testkit/fixtures/input/clean/a.input.yaml --json \
    | jq -e '.ok and .actions==7 and .sticks==1 and (.files|length)==2' >/dev/null
INPUT_STATUS=0
INPUT_OUT=$(build/dev/midday validate testkit/fixtures/input/conflict/a.input.yaml --json) \
    || INPUT_STATUS=$?
[ "$INPUT_STATUS" -eq 3 ]
echo "$INPUT_OUT" | jq -e '.error.code == "input.conflict"
    and (.error.message | contains("jump")) and (.error.message | contains("crouch"))' >/dev/null

# The runtime rebinding overlay (spec section 13 "a user-profile overlay"):
# a clean rebind validates; an overlay whose OWN two rebinds collide with
# EACH OTHER refuses identically, independent of any base map.
build/dev/midday validate testkit/fixtures/input/profile/rebind.input_profile.yaml --json \
    | jq -e '.ok and .actions==1' >/dev/null
PROFILE_STATUS=0
PROFILE_OUT=$(build/dev/midday validate \
    testkit/fixtures/input/profile/rebind_conflict.input_profile.yaml --json) || PROFILE_STATUS=$?
[ "$PROFILE_STATUS" -eq 3 ]
echo "$PROFILE_OUT" | jq -e '.error.code == "input.conflict"' >/dev/null

step "uid system (m1-uid-system: check --fix repairs drift + mints; a hand-minted uid refuses; the cache regenerates byte-identical)"
# m1-uid-system exit tests, all three over testkit/fixtures/uid (self-
# contained fixtures — NOT a real scene/machine, m1-scene-format's job; see
# testkit/fixtures/uid/README.md, the same "self-contained corpus" call
# m1-strict-yaml made for widget.schema.json). Every scenario runs against a
# SCRATCH copy: `check`/`mv` write a regenerable .midday-cache/uid/ cache
# next to whatever root they scan, and exit-test #1 physically moves files,
# so the committed fixtures are never touched in place (fmt_idem precedent).
rm -rf build/dev/uid_probe
mkdir -p build/dev/uid_probe
cp -R testkit/fixtures/uid/clean build/dev/uid_probe/move
cp -R testkit/fixtures/uid/clean build/dev/uid_probe/cache_a
cp -R testkit/fixtures/uid/clean build/dev/uid_probe/cache_b
cp -R testkit/fixtures/uid/clean build/dev/uid_probe/mv_verb
cp -R testkit/fixtures/uid/hand_minted build/dev/uid_probe/hand_minted

# 1) move a fixture asset (+ its sidecar, together — the natural way to
#    relocate a uid-tracked asset without going through `midday mv`) and
#    confirm `check --fix` repairs the now-stale path while the uid stays
#    byte-identical.
UID_BEFORE=$(grep -o 'uid://[0-9a-z]*' build/dev/uid_probe/move/fixture.demo.yaml)
mkdir -p build/dev/uid_probe/move/assets/moved
mv build/dev/uid_probe/move/assets/widget.asset build/dev/uid_probe/move/assets/moved/widget.asset
mv build/dev/uid_probe/move/assets/widget.asset.uid build/dev/uid_probe/move/assets/moved/widget.asset.uid
build/dev/midday check build/dev/uid_probe/move --fix --json \
    | jq -e '.ok and .counts.drift==1 and .counts.fixed==1' >/dev/null
grep -q 'path: assets/moved/widget.asset' build/dev/uid_probe/move/fixture.demo.yaml
UID_AFTER=$(grep -o 'uid://[0-9a-z]*' build/dev/uid_probe/move/fixture.demo.yaml)
[ "$UID_BEFORE" = "$UID_AFTER" ]

# 2) a hand-minted uid (well-formed uid:// text, but no .uid sidecar
#    anywhere backs it) is a validation ERROR (exit 3), never a silent pass.
UID_STATUS=0
UID_OUT=$(build/dev/midday check build/dev/uid_probe/hand_minted --json) || UID_STATUS=$?
[ "$UID_STATUS" -eq 3 ]
echo "$UID_OUT" | jq -e '.error.code == "check.invalid_ref" and .counts.invalid==1
    and .findings[0].status=="invalid"' >/dev/null

# 3) deleting the cache and regenerating it reproduces the SAME bytes — two
#    INDEPENDENT scratch copies (never a self-diff), each scanning the same
#    committed sidecar from scratch.
build/dev/midday check build/dev/uid_probe/cache_a --json >/dev/null
build/dev/midday check build/dev/uid_probe/cache_b --json >/dev/null
rm -rf build/dev/uid_probe/cache_a/.midday-cache build/dev/uid_probe/cache_b/.midday-cache
build/dev/midday check build/dev/uid_probe/cache_a --json >/dev/null
build/dev/midday check build/dev/uid_probe/cache_b --json >/dev/null
cmp build/dev/uid_probe/cache_a/.midday-cache/uid/registry.json \
    build/dev/uid_probe/cache_b/.midday-cache/uid/registry.json

# `midday mv`: moves the asset + sidecar together and rewrites the
# referencing path ITSELF (no manual `mv` needed) — the uid still never
# changes; --root defaults to the current directory.
MIDDAY_BIN="$PWD/build/dev/midday"
UID_BEFORE_MV=$(grep -o 'uid://[0-9a-z]*' build/dev/uid_probe/mv_verb/fixture.demo.yaml)
(cd build/dev/uid_probe/mv_verb && "$MIDDAY_BIN" mv assets/widget.asset assets/renamed.asset --json) \
    | jq -e '.ok and (.files_updated | length)==1' >/dev/null
grep -q 'path: assets/renamed.asset' build/dev/uid_probe/mv_verb/fixture.demo.yaml
test -f build/dev/uid_probe/mv_verb/assets/renamed.asset.uid
UID_AFTER_MV=$(grep -o 'uid://[0-9a-z]*' build/dev/uid_probe/mv_verb/fixture.demo.yaml)
[ "$UID_BEFORE_MV" = "$UID_AFTER_MV" ]

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

step "determinism kata (600-tick exercised asserts + dual-run compare + tainted lint gate)"
# m0-determinism-spike exit tests (MILESTONE_0 item 25, Zenith D024): the
# kata must move TS GC churn + Jolt stepping + statechart cascades +
# sequence spans SIMULTANEOUSLY — the `.exercised.*` asserts run BEFORE any
# compare so an empty-scene byte-compare can never vacuously pass.
# Bit-identity is two INDEPENDENT runs (never a self-diff): the diff verb
# names divergences, then the normalized record streams are byte-compared
# (zstdcat per D-BUILD-080 — framing is transport, record content is the run).
rm -rf build/spike
build/dev/midday run examples/spikes/determinism.scene.yaml --ticks 600 --seed 123 \
    --record build/spike/ka.mrj --cache-dir build/dev/ts-cache.a \
    --assert case=determinism_kata --json \
    | jq -e '.ok and .exercised.ts_gc_churn and .exercised.jolt_step
        and .exercised.statechart_transitions and .exercised.sequence_spans' >/dev/null
build/dev/midday run examples/spikes/determinism.scene.yaml --ticks 600 --seed 123 \
    --record build/spike/kb.mrj --cache-dir build/dev/ts-cache.a \
    --assert case=determinism_kata --json >/dev/null
build/dev/midday journal diff build/spike/ka.mrj build/spike/kb.mrj --json \
    | jq -e '.first_divergent_tick==null and .identical' >/dev/null
zstdcat build/spike/ka.mrj/journal.jsonl.zst > build/spike/ka.jsonl
zstdcat build/spike/kb.mrj/journal.jsonl.zst > build/spike/kb.jsonl
cmp build/spike/ka.jsonl build/spike/kb.jsonl
# wall-clock taint: Date.now() must die at the LINT GATE (exit 3,
# script.lint, no-wall-clock at file:line) through the REAL run path,
# before a single tick executes.
TAINT_STATUS=0
TAINT_OUT=$(build/dev/midday run examples/spikes/tainted/tainted.scene.yaml --ticks 1 \
    --cache-dir build/dev/ts-cache.a --json) || TAINT_STATUS=$?
[ "$TAINT_STATUS" -eq 3 ]
echo "$TAINT_OUT" | jq -e '.error.code == "script.lint"
    and .error.details.diagnostics[0].code == "no-wall-clock"
    and .error.details.diagnostics[0].line > 0' >/dev/null

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

step "entity-API boundary (self-test + scan: no code-assembled entities, spec section 7)"
# Entities are born from data (the loader is the sole spawn path); nothing above
# the runtime may call World::spawn/queue_spawn/emplace directly. The self-test
# proves the scanner still catches a planted assembler before the clean scan is
# trusted (same falsifiability contract as the include boundary above).
scripts/check_entity_api.py --self-test >/dev/null
scripts/check_entity_api.py >/dev/null

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

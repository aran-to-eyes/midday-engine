#!/usr/bin/env bash
# scripts/lib/verify_behavioral.sh — the PLATFORM-AGNOSTIC behavioral core of
# the gate (M1-exit Phase 1, "asymmetric gate geography" cure).
#
# Every function here is driven entirely through the BUILT BINARY plus the
# repo-relative fixture tree (examples/, testkit/) — no toolchain, no
# compiler, no lint. That is what makes it shareable: `scripts/verify.sh`
# sources this file and runs the full sequence (Linux/macOS local + CI,
# authoritative); `.github/workflows/ci.yml`'s `build-windows` job sources
# the SAME file under Git-Bash and calls a curated D-9 subset (Aurora D-9 —
# see the ci.yml comment at build-windows for exactly which functions and
# why). One behavioral core, not two hand-kept copies that drift.
#
# Contract: this file is SOURCED, never executed directly. It does not set
# `-e`/`-u`/`-o pipefail` itself (that would clobber the caller's shell
# options) — verify.sh sets them; GitHub Actions' `shell: bash` already runs
# `bash -e -o pipefail` by default. Every public function takes the midday
# binary path as its first argument (`bin`) and, where it needs scratch
# space, a build directory as its second (`build_dir`) — so the same
# function runs against `build/dev/midday` (local) or `build/ci/midday.exe`
# (CI) without modification. Functions prefixed `_` are private helpers.
#
# Portability: must run under bash 3.2 (macOS system bash) AND Git-Bash on
# windows-2022 (jq/python3 already present there; no zstd dependency — the
# functions Windows actually calls use `midday journal diff`, never
# `zstdcat`). No bashisms bash 3.2 lacks (no associative arrays, no
# `${var,,}`, no `mapfile`); indexed arrays are fine (already used below).

# ---------------------------------------------------------------------------
# selftest / envelope / journal fixture
# ---------------------------------------------------------------------------

behavioral_selftest() { # <bin>
    "$1" selftest
}

behavioral_envelope() { # <bin>
    "$1" version --json | scripts/validate_envelope.py >/dev/null
}

behavioral_journal_fixture() { # <bin> <build_dir>
    # Regenerate the greppable bundle from scratch and byte-compare it
    # against the committed fixture (journal writer determinism, pinned
    # across platforms), then run the actual zstdcat|grep the spec promises
    # (zstd is a declared host tool, Aurora D-22).
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/journal_fixture.mrj"
    MIDDAY_JOURNAL_FIXTURE_DIR="$build_dir/journal_fixture.mrj" \
        "$bin" selftest --filter 'journal.greppability' >/dev/null
    local f
    for f in header.json index.json journal.jsonl.zst; do
        cmp "testkit/fixtures/journal/greppable.mrj/$f" "$build_dir/journal_fixture.mrj/$f"
    done
    zstdcat "testkit/fixtures/journal/greppable.mrj/journal.jsonl.zst" | grep -q known_event
}

# ---------------------------------------------------------------------------
# engine_api / codegen drift
# ---------------------------------------------------------------------------

# Determinism is proven by two independent dumps diffed (never a self-diff).
# This is the D-9 self-consistency core Windows shares: it does NOT touch
# api/engine_api.json, so MSVC float-formatting drift can never permanent-red
# the Windows lane — that committed artifact stays Linux's contract.
behavioral_api_dump_self_consistency() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    "$bin" api dump --out "$build_dir/engine_api.json" >/dev/null
    "$bin" api dump --out "$build_dir/engine_api.rerun.json" >/dev/null
    cmp "$build_dir/engine_api.json" "$build_dir/engine_api.rerun.json"
}

# The committed-artifact contract: byte-compare against api/engine_api.json,
# schema-validate, and confirm `api diff` reports clean. Requires
# behavioral_api_dump_self_consistency to have already populated
# $build_dir/engine_api.json. Linux/macOS only — never called from Windows.
behavioral_api_drift_vs_committed() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    cmp "api/engine_api.json" "$build_dir/engine_api.json"
    scripts/validate_envelope.py formats/engine_api.schema.json <api/engine_api.json >/dev/null
    "$bin" api diff api/engine_api.json >/dev/null
}

# The SELF-HOSTED generator (m0-codegen-selfhost) is authoritative: two
# independent `midday api codegen` runs are cmp'd (never a self-diff), then
# the four committed api/ artifacts are byte-compared against a selfhost
# regeneration and schema_manifest.json is validated against its meta-schema.
behavioral_codegen_drift() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/codegen" "$build_dir/codegen.rerun"
    "$bin" api codegen "api/engine_api.json" --out-dir "$build_dir/codegen" \
        --cache-dir "$build_dir/ts-cache.codegen" >/dev/null
    "$bin" api codegen "api/engine_api.json" --out-dir "$build_dir/codegen.rerun" \
        --cache-dir "$build_dir/ts-cache.codegen" >/dev/null
    local f
    for f in engine.d.ts schema_manifest.json api_docs.md bindings_spec.json; do
        cmp "$build_dir/codegen/$f" "$build_dir/codegen.rerun/$f"
        cmp "api/$f" "$build_dir/codegen/$f"
    done
    scripts/validate_envelope.py formats/schema_manifest.schema.json <api/schema_manifest.json >/dev/null
}

# MILESTONE_0 "Codegen Bootstrap": the self-hosted generator became
# authoritative only because it byte-matches the native bootstrap; this gate
# keeps that claim live on the full corpus. Requires behavioral_codegen_drift
# to have already populated $build_dir/ts-cache.codegen.
behavioral_codegen_equivalence() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    "$bin" api codegen --verify-equivalence --cache-dir "$build_dir/ts-cache.codegen" >/dev/null
}

# ---------------------------------------------------------------------------
# script toolchain / ts components
# ---------------------------------------------------------------------------

# ts_hello: clean check (exit 0); dual INDEPENDENT builds byte-compared
# (never a self-diff); second run reports zero re-transpiles. type_error:
# exit 3 with a structured file:line diagnostic. lint pack: exit 3, all three
# rule families, each hit located at file:line.
behavioral_script_toolchain() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/ts-cache.a" "$build_dir/ts-cache.b"
    "$bin" script check "testkit/fixtures/ts/hello.ts" --cache-dir "$build_dir/ts-cache.a" --json \
        | jq -e '.ok and .diagnostics == []' >/dev/null
    "$bin" script build "testkit/fixtures/ts/hello.ts" --cache-dir "$build_dir/ts-cache.a" --stats --json \
        | jq -e '.ok and .cache_hit == false and .stats.transpiled == 1' >/dev/null
    "$bin" script build "testkit/fixtures/ts/hello.ts" --cache-dir "$build_dir/ts-cache.a" --stats --json \
        | jq -e '.ok and .cache_hit == true and .stats == {"transpiled":0,"cache_hits":1}' >/dev/null
    "$bin" script build "testkit/fixtures/ts/hello.ts" --cache-dir "$build_dir/ts-cache.b" --json >/dev/null
    cmp "$build_dir/ts-cache.a"/*.js "$build_dir/ts-cache.b"/*.js

    local script_status=0 script_out
    script_out=$("$bin" script check "testkit/fixtures/ts/type_error.ts" \
        --cache-dir "$build_dir/ts-cache.a" --json) || script_status=$?
    [ "$script_status" -eq 3 ]
    echo "$script_out" | jq -e '.error.code == "script.type_error"
        and .diagnostics[0].line > 0 and (.diagnostics[0].file | endswith("type_error.ts"))' >/dev/null

    script_status=0
    script_out=$("$bin" script check "testkit/fixtures/ts/lint_violations.ts" \
        --cache-dir "$build_dir/ts-cache.a" --json) || script_status=$?
    [ "$script_status" -eq 3 ]
    echo "$script_out" | jq -e '.error.code == "script.lint"
        and ([.diagnostics[].code] | unique) == ["no-timer","no-unseeded-random","no-wall-clock"]
        and ([.diagnostics[] | select(.line > 0 and .col > 0)] | length) == 6' >/dev/null
}

# Exit-test #1: the authored Warden components AND dead.ts (despawn-linger
# authoring surface, M2 0B #12b) typecheck against the generated
# engine.d.ts. Exit-test #2: their schema extracts STATICALLY from the AST
# into a PROJECT manifest (format 2: event_bindings). The extract-throws
# fixture proves the walk never runs the code.
behavioral_ts_components() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -f "$build_dir/health.components.json" "$build_dir/dangerous.components.json" \
        "$build_dir/damage.components.json"
    "$bin" script check "examples/warden/components/health.ts" \
        --cache-dir "$build_dir/ts-cache.components" --json | jq -e '.ok' >/dev/null
    "$bin" script check "examples/warden/components/damage_on_touch.ts" \
        --cache-dir "$build_dir/ts-cache.components" --json | jq -e '.ok' >/dev/null
    # dead.ts joined the typecheck corpus at M2 0B: world.despawn(ref,
    # {after}) is authored surface now (the old standing TS2554 died with
    # the despawn opts regeneration).
    "$bin" script check "examples/warden/states/dead.ts" \
        --cache-dir "$build_dir/ts-cache.components" --json | jq -e '.ok' >/dev/null
    "$bin" script extract "examples/warden/components/health.ts" \
        --out "$build_dir/health.components.json" --cache-dir "$build_dir/ts-cache.components" --json \
        | jq -e '.ok and .components == 1' >/dev/null
    jq -e '.format_version == 2 and .components[0].name == "Health"
        and (.components[0].fields | map(.name)) == ["max","value"]
        and .components[0].fields[0].type == "float" and .components[0].fields[0].min == 0
        and (.components[0].methods[0].params | map(.type)) == ["float","entity_ref"]
        and .components[0].event_bindings == []' \
        "$build_dir/health.components.json" >/dev/null
    # The spec-literal two-param listener (D1, #12b): DamageOnTouch's
    # onEvent overload extracts to the exact {event, payload_compat_hash}
    # pair — the hash pinned literally against bindings_spec.json's
    # generated bijection, and onEvent NEVER doubles as an ordinary method.
    "$bin" script extract "examples/warden/components/damage_on_touch.ts" \
        --out "$build_dir/damage.components.json" --cache-dir "$build_dir/ts-cache.components" --json \
        | jq -e '.ok and .components == 1' >/dev/null
    jq -e '.components[0].name == "DamageOnTouch"
        and .components[0].event_bindings ==
            [{"event":"trigger.entered","payload_compat_hash":"d9d4b0d4f4ce21a0"}]
        and ([.components[0].methods[].name] | index("onEvent")) == null' \
        "$build_dir/damage.components.json" >/dev/null
    "$bin" script extract "testkit/fixtures/ts/component_extract_throws.ts" \
        --out "$build_dir/dangerous.components.json" --cache-dir "$build_dir/ts-cache.components" --json \
        | jq -e '.ok and .components == 1' >/dev/null
    jq -e '.components[0].name == "Dangerous"' "$build_dir/dangerous.components.json" >/dev/null

    # m1-exit Phase 3 (CONCERNS #12a): 'midday'-qualified annotations.
    # Positive — import('midday').EntityRef and midday.Vec3 both extract
    # (resolved against engine.d.ts, never the checker, which types the
    # import() spelling as `any`).
    rm -f "$build_dir/qualified.components.json" "$build_dir/unresolved.components.json" \
        "$build_dir/unknown_member.components.json" "$build_dir/module_surface.components.json"
    "$bin" script extract "testkit/fixtures/ts/component_extract_qualified.ts" \
        --out "$build_dir/qualified.components.json" --cache-dir "$build_dir/ts-cache.components" --json \
        | jq -e '.ok and .components == 1' >/dev/null
    jq -e '.components[0].name == "Seeker"
        and (.components[0].fields | map(.type)) == ["float","vec3"]
        and (.components[0].methods[0].params | map(.type)) == ["entity_ref","vec3"]
        and .components[0].methods[1].returns == "vec3"' \
        "$build_dir/qualified.components.json" >/dev/null
    # Negatives — every refusal class, all exit 3, all validate-before-
    # write (no manifest file may appear).
    # SCHEMA-owned: in the d.ts but not a field type (midday.EventPayloads,
    # the lookup-map interface — retargeted at #12b when the old
    # TriggerEnteredEvent annotation became the payload-position refusal
    # below) -> schema.unresolved_type from the extraction walk.
    local rc=0 out
    out=$("$bin" script extract "testkit/fixtures/ts/component_extract_unresolved.ts" \
        --out "$build_dir/unresolved.components.json" \
        --cache-dir "$build_dir/ts-cache.components" --json) || rc=$?
    [ "$rc" -eq 3 ]
    echo "$out" | jq -e '(.ok | not) and .error.code == "script.schema_error"
        and (([.diagnostics[].code] | index("schema.unresolved_type")) != null)' >/dev/null
    [ ! -f "$build_dir/unresolved.components.json" ]
    # TYPE-owned: not declared anywhere -> the checker refuses (TS2694)
    # BEFORE extraction; pinned so a module-binding change can never demote
    # this to `any` and reopen the fabrication window silently.
    rc=0
    out=$("$bin" script extract "testkit/fixtures/ts/component_extract_unknown_member.ts" \
        --out "$build_dir/unknown_member.components.json" \
        --cache-dir "$build_dir/ts-cache.components" --json) || rc=$?
    [ "$rc" -eq 3 ]
    echo "$out" | jq -e '(.ok | not) and .error.code == "script.type_error"
        and (([.diagnostics[].code] | index("TS2694")) != null)' >/dev/null
    [ ! -f "$build_dir/unknown_member.components.json" ]
    # MODULE-surface, consciously FLIPPED at M2 0B (#12b): the module now
    # exports the ...Event-suffixed payload aliases, so the m1-exit TS2694
    # pin became the POSITIVE vocabulary pin — the spec-literal two-param
    # onEvent extracts the exact event_bindings pair. Module-exports ⊂
    # namespace still holds; the boundary moved by exactly these aliases
    # (unknown_member above still pins the checker's module gate).
    rm -f "$build_dir/module_surface.components.json"
    "$bin" script extract "testkit/fixtures/ts/component_extract_module_surface.ts" \
        --out "$build_dir/module_surface.components.json" \
        --cache-dir "$build_dir/ts-cache.components" --json \
        | jq -e '.ok and .components == 1' >/dev/null
    jq -e '.format_version == 2 and .components[0].name == "Eavesdropper"
        and .components[0].event_bindings ==
            [{"event":"trigger.entered","payload_compat_hash":"d9d4b0d4f4ce21a0"}]
        and (.components[0].fields | map(.name)) == ["sensitivity"]
        and .components[0].methods == []' \
        "$build_dir/module_surface.components.json" >/dev/null

    # M2 0B (#12b) event-vocabulary negatives: six DISTINCT structured
    # refusals, each exit 3 + script.schema_error + its own diagnostic code,
    # each validate-before-write (no manifest file may appear). The sixth
    # (council fix C3): onEvent authored as a class PROPERTY refuses as the
    # listener-shape violation instead of extracting zero bindings silently.
    local stem code
    for stem in \
        "event_field:schema.event_payload_field" \
        "event_param:schema.event_payload_param" \
        "event_mismatch:schema.event_mismatch" \
        "event_duplicate:schema.event_duplicate" \
        "event_union:schema.event_union_only" \
        "event_property:schema.event_listener_shape"; do
        code="${stem#*:}"
        stem="${stem%%:*}"
        rm -f "$build_dir/$stem.components.json"
        rc=0
        out=$("$bin" script extract "testkit/fixtures/ts/component_extract_$stem.ts" \
            --out "$build_dir/$stem.components.json" \
            --cache-dir "$build_dir/ts-cache.components" --json) || rc=$?
        [ "$rc" -eq 3 ]
        echo "$out" | jq -e --arg code "$code" '(.ok | not)
            and .error.code == "script.schema_error"
            and (([.diagnostics[].code] | index($code)) != null)' >/dev/null
        [ ! -f "$build_dir/$stem.components.json" ]
    done

    # `midday run --components` (M2 0B, #12b): flag registered + VALIDATED
    # only — host wiring lands with the component-host track. A missing
    # manifest refuses structured (exit 3) before any tick; a valid format-2
    # manifest passes through with run behavior unchanged.
    rc=0
    out=$("$bin" run "testkit/smoke/smoke.scene.yaml" --ticks 1 \
        --cache-dir "$build_dir/ts-cache.components" \
        --components "$build_dir/no_such.components.json" --json) || rc=$?
    [ "$rc" -eq 3 ]
    echo "$out" | jq -e '(.ok | not) and .error.code == "loader.io"' >/dev/null
    "$bin" run "testkit/smoke/smoke.scene.yaml" --ticks 1 \
        --cache-dir "$build_dir/ts-cache.components" \
        --components "$build_dir/health.components.json" --json \
        | jq -e '.ok and .ticks == 1' >/dev/null
}

# ---------------------------------------------------------------------------
# batch-binding budgets
# ---------------------------------------------------------------------------

# One-size budget check, shared by verify.sh's full sweep AND Windows's D-9
# lite check (1k only): crossings stay <= 8 * pool_count and steady-state
# pooled-math ticks allocate ZERO GC bytes (semantic counts, never bytes —
# these are the properties MSVC codegen cannot make flaky). Prints
# boundary_crossings_per_tick on stdout so a caller sweeping multiple sizes
# can compare them for constancy.
behavioral_batch_budget_check() { # <bin> <build_dir> <entities> -> prints crossings/tick
    local bin="$1" build_dir="$2" n="$3" bench_out
    bench_out=$("$bin" script bench --entities "$n" --ticks 60 \
        --cache-dir "$build_dir/ts-cache.a" --json)
    echo "$bench_out" | jq -e '.ok and .mode=="batched" and .crossings_constant
        and .boundary_crossings_per_tick <= 8*.pool_count
        and .gc_alloc_bytes_per_tick==0' >/dev/null
    echo "$bench_out" | jq '.boundary_crossings_per_tick'
}

# The 1k/10k/100k sweep (crossings O(pools), never O(entities)) plus the
# naive per-field mode must-cross->=10x check. Linux/macOS only — Windows
# runs only behavioral_batch_budget_check at 1k (see ci.yml build-windows).
behavioral_batch_budgets_sweep() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    local batch_crossings="" n n_crossings
    for n in 1000 10000 100000; do
        n_crossings=$(behavioral_batch_budget_check "$bin" "$build_dir" "$n")
        if [ -z "$batch_crossings" ]; then batch_crossings="$n_crossings"; fi
        [ "$n_crossings" -eq "$batch_crossings" ]
    done
    "$bin" script bench --naive --entities 1000 --ticks 10 \
        --cache-dir "$build_dir/ts-cache.a" --json \
        | jq -e ".ok and .mode==\"naive\" and .crossings_constant
            and .boundary_crossings_per_tick >= 10*$batch_crossings" >/dev/null
}

# ---------------------------------------------------------------------------
# yaml loader + run (boss corpus smoke, m0-yaml-loader-run) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_boss_corpus() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/m0"
    "$bin" run "examples/appendix_a/boss.scene.yaml" --to-tick 100 --seed 7 \
        --record "$build_dir/m0/a.mrj" --cache-dir "$build_dir/ts-cache.a" --json \
        | jq -e '.ok and .ticks==100 and .recorded_tier=="flight"' >/dev/null
    "$bin" run "examples/appendix_a/boss.scene.yaml" --to-tick 100 --seed 7 \
        --record "$build_dir/m0/b.mrj" --cache-dir "$build_dir/ts-cache.a" --json >/dev/null
    "$bin" journal diff "$build_dir/m0/a.mrj" "$build_dir/m0/b.mrj" --json \
        | jq -e '.first_divergent_tick==null and .identical' >/dev/null
    # Strictness is the product: an unknown YAML key exits 3 with file/line.
    local loader_status=0 loader_out
    loader_out=$(printf 'format: 1\nscene: s\nentitiez: []\n' >"$build_dir/m0/bad.scene.yaml" \
        && "$bin" run "$build_dir/m0/bad.scene.yaml" --json) || loader_status=$?
    [ "$loader_status" -eq 3 ]
    echo "$loader_out" | jq -e '.error.code == "loader.unknown_key"
        and .error.details.line == 3 and (.error.message | contains("entitiez"))' >/dev/null
}

# ---------------------------------------------------------------------------
# schema validate + fmt (m1-strict-yaml) — Linux/macOS only
# ---------------------------------------------------------------------------

# Internal helper for behavioral_schema_validate_fmt.
_assert_widget_refusal() { # <bin> <schema-file> <fixture-stem> <expected error.code>
    local bin="$1" schema="$2" stem="$3" code="$4" status=0 out
    out=$("$bin" validate "testkit/fixtures/schema/$stem.widget.yaml" \
        --schema-file "$schema" --json) || status=$?
    [ "$status" -eq 3 ]
    echo "$out" | jq -e --arg code "$code" \
        '(.ok | not) and .error.code == $code and (.error.message | test(":[0-9]+:[0-9]+: "))' \
        >/dev/null
}

# The generic schema engine (core/loader/format_schema.h) refuses six
# mutation classes at exit 3 with a file:line[:col] diagnostic, then
# fmt(fmt(x)) == fmt(x) idempotence over every strict-YAML fixture in the
# tree, plus the migration registry (v1 -> v2 forward rename) and the
# --write/--check round trip.
behavioral_schema_validate_fmt() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    local widget_schema="testkit/fixtures/schema/widget.schema.json"
    "$bin" validate "testkit/fixtures/schema/valid_v2.widget.yaml" \
        --schema-file "$widget_schema" --json \
        | jq -e '.ok and .format_version.authored==2 and (.format_version.migrated|not)' >/dev/null
    "$bin" validate "testkit/fixtures/schema/valid_v1.widget.yaml" \
        --schema-file "$widget_schema" --json \
        | jq -e '.ok and .format_version.authored==1 and .format_version.current==2
            and .format_version.migrated' >/dev/null

    _assert_widget_refusal "$bin" "$widget_schema" mutation_wrong_type loader.bad_value
    _assert_widget_refusal "$bin" "$widget_schema" mutation_unknown_key loader.unknown_key
    _assert_widget_refusal "$bin" "$widget_schema" mutation_future_format loader.bad_format
    _assert_widget_refusal "$bin" "$widget_schema" mutation_bad_enum schema.bad_enum
    _assert_widget_refusal "$bin" "$widget_schema" mutation_duplicate_key yaml.strict
    _assert_widget_refusal "$bin" "$widget_schema" mutation_alias yaml.strict

    local fmt_status=0 fmt_out
    fmt_out=$("$bin" fmt "testkit/fixtures/schema/mutation_alias.widget.yaml" --json) || fmt_status=$?
    [ "$fmt_status" -eq 3 ]
    echo "$fmt_out" | jq -e '.error.code == "yaml.strict"' >/dev/null

    rm -rf "$build_dir/fmt_idem"
    mkdir -p "$build_dir/fmt_idem"
    local fmt_fixtures=(
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
    local f name
    for f in "${fmt_fixtures[@]}"; do
        name=$(basename "$f")
        "$bin" fmt "$f" >"$build_dir/fmt_idem/$name.once"
        "$bin" fmt "$build_dir/fmt_idem/$name.once" >"$build_dir/fmt_idem/$name.twice"
        cmp "$build_dir/fmt_idem/$name.once" "$build_dir/fmt_idem/$name.twice"
    done
    cp "testkit/fixtures/schema/valid_v2.widget.yaml" "$build_dir/fmt_idem/scratch.widget.yaml"
    "$bin" fmt "$build_dir/fmt_idem/scratch.widget.yaml" --write --json \
        | jq -e '.ok and .changed' >/dev/null
    "$bin" fmt "$build_dir/fmt_idem/scratch.widget.yaml" --check --json \
        | jq -e '.ok and .canonical' >/dev/null
}

# ---------------------------------------------------------------------------
# events format (m1-events-format) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_events_format() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    "$bin" validate "examples/warden/events/combat.events.yaml" --json \
        | jq -e '.ok and .schema=="events" and .events==8' >/dev/null

    rm -rf "$build_dir/events_probe"
    mkdir -p "$build_dir/events_probe/dup" "$build_dir/events_probe/clean"
    printf 'format: 1\nevents: {dup.name: {}}\n' >"$build_dir/events_probe/dup/a.events.yaml"
    printf 'format: 1\nevents: {dup.name: {}}\n' >"$build_dir/events_probe/dup/b.events.yaml"
    local events_status=0 events_out
    events_out=$("$bin" validate "$build_dir/events_probe/dup/a.events.yaml" --json) \
        || events_status=$?
    [ "$events_status" -eq 3 ]
    echo "$events_out" | jq -e '.error.code == "loader.duplicate"
        and (.error.message | contains("dup.name")) and (.error.message | test(":[0-9]+:[0-9]+: "))' \
        >/dev/null

    printf 'format: 1\nevents: {a.one: {}}\nkeys: [squad]\n' >"$build_dir/events_probe/clean/a.events.yaml"
    printf 'format: 1\nevents: {b.two: {payload: {x: float}}}\n' >"$build_dir/events_probe/clean/b.events.yaml"
    "$bin" validate "$build_dir/events_probe/clean/a.events.yaml" --json \
        | jq -e '.ok and (.files | length)==2 and .events==2 and .groups==1' >/dev/null

    printf 'format: 1\nmachine: bad\nregions:\n  main:\n    initial: Idle\n    states:\n      Idle:\n        on:\n          - {event: nope.undefined, goto: Idle}\n' \
        >"$build_dir/events_probe/bad.machine.yaml"
    printf 'format: 1\nscene: bad\nentities:\n  - entity: E\n    machines:\n      - {instance: {path: bad.machine.yaml}}\n' \
        >"$build_dir/events_probe/bad.scene.yaml"
    local run_status=0 run_out
    run_out=$("$bin" run "$build_dir/events_probe/bad.scene.yaml" --ticks 1 --json) \
        || run_status=$?
    [ "$run_status" -eq 3 ]
    echo "$run_out" | jq -e '.error.code == "loader.bad_ref"
        and (.error.message | contains("nope.undefined")) and .error.details.line > 0' >/dev/null

    rm -rf "$build_dir/ts-cache.events"
    local halt_status=0 halt_out
    halt_out=$("$bin" run "testkit/fixtures/events/wrong_payload.scene.yaml" --ticks 5 \
        --cache-dir "$build_dir/ts-cache.events" --json) || halt_status=$?
    [ "$halt_status" -eq 1 ]
    echo "$halt_out" | jq -e '.error.code == "script.exception"
        and (.error.message | contains("bus.payload_invalid"))
        and (.error.details.tick // -1) >= 0' >/dev/null
}

# ---------------------------------------------------------------------------
# input actions (m1-input-actions) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_input_actions() { # <bin> <build_dir>
    local bin="$1"
    "$bin" selftest --filter 'input.*' >/dev/null

    "$bin" validate "testkit/fixtures/input/clean/a.input.yaml" --json \
        | jq -e '.ok and .actions==7 and .sticks==1 and (.files|length)==2' >/dev/null
    local input_status=0 input_out
    input_out=$("$bin" validate "testkit/fixtures/input/conflict/a.input.yaml" --json) \
        || input_status=$?
    [ "$input_status" -eq 3 ]
    echo "$input_out" | jq -e '.error.code == "input.conflict"
        and (.error.message | contains("jump")) and (.error.message | contains("crouch"))' >/dev/null

    "$bin" validate "testkit/fixtures/input/profile/rebind.input_profile.yaml" --json \
        | jq -e '.ok and .actions==1' >/dev/null
    local profile_status=0 profile_out
    profile_out=$("$bin" validate \
        "testkit/fixtures/input/profile/rebind_conflict.input_profile.yaml" --json) || profile_status=$?
    [ "$profile_status" -eq 3 ]
    echo "$profile_out" | jq -e '.error.code == "input.conflict"' >/dev/null
}

# ---------------------------------------------------------------------------
# uid system (m1-uid-system) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_uid_system() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/uid_probe"
    mkdir -p "$build_dir/uid_probe"
    cp -R testkit/fixtures/uid/clean "$build_dir/uid_probe/move"
    cp -R testkit/fixtures/uid/clean "$build_dir/uid_probe/cache_a"
    cp -R testkit/fixtures/uid/clean "$build_dir/uid_probe/cache_b"
    cp -R testkit/fixtures/uid/clean "$build_dir/uid_probe/mv_verb"
    cp -R testkit/fixtures/uid/hand_minted "$build_dir/uid_probe/hand_minted"

    local uid_before uid_after
    uid_before=$(grep -o 'uid://[0-9a-z]*' "$build_dir/uid_probe/move/fixture.demo.yaml")
    mkdir -p "$build_dir/uid_probe/move/assets/moved"
    mv "$build_dir/uid_probe/move/assets/widget.asset" "$build_dir/uid_probe/move/assets/moved/widget.asset"
    mv "$build_dir/uid_probe/move/assets/widget.asset.uid" "$build_dir/uid_probe/move/assets/moved/widget.asset.uid"
    "$bin" check "$build_dir/uid_probe/move" --fix --json \
        | jq -e '.ok and .counts.drift==1 and .counts.fixed==1' >/dev/null
    grep -q 'path: assets/moved/widget.asset' "$build_dir/uid_probe/move/fixture.demo.yaml"
    uid_after=$(grep -o 'uid://[0-9a-z]*' "$build_dir/uid_probe/move/fixture.demo.yaml")
    [ "$uid_before" = "$uid_after" ]

    local uid_status=0 uid_out
    uid_out=$("$bin" check "$build_dir/uid_probe/hand_minted" --json) || uid_status=$?
    [ "$uid_status" -eq 3 ]
    echo "$uid_out" | jq -e '.error.code == "check.invalid_ref" and .counts.invalid==1
        and .findings[0].status=="invalid"' >/dev/null

    "$bin" check "$build_dir/uid_probe/cache_a" --json >/dev/null
    "$bin" check "$build_dir/uid_probe/cache_b" --json >/dev/null
    rm -rf "$build_dir/uid_probe/cache_a/.midday-cache" "$build_dir/uid_probe/cache_b/.midday-cache"
    "$bin" check "$build_dir/uid_probe/cache_a" --json >/dev/null
    "$bin" check "$build_dir/uid_probe/cache_b" --json >/dev/null
    cmp "$build_dir/uid_probe/cache_a/.midday-cache/uid/registry.json" \
        "$build_dir/uid_probe/cache_b/.midday-cache/uid/registry.json"

    local bin_abs="$PWD/$bin"
    local uid_before_mv uid_after_mv
    uid_before_mv=$(grep -o 'uid://[0-9a-z]*' "$build_dir/uid_probe/mv_verb/fixture.demo.yaml")
    (cd "$build_dir/uid_probe/mv_verb" && "$bin_abs" mv assets/widget.asset assets/renamed.asset --json) \
        | jq -e '.ok and (.files_updated | length)==1' >/dev/null
    grep -q 'path: assets/renamed.asset' "$build_dir/uid_probe/mv_verb/fixture.demo.yaml"
    test -f "$build_dir/uid_probe/mv_verb/assets/renamed.asset.uid"
    uid_after_mv=$(grep -o 'uid://[0-9a-z]*' "$build_dir/uid_probe/mv_verb/fixture.demo.yaml")
    [ "$uid_before_mv" = "$uid_after_mv" ]
}

# ---------------------------------------------------------------------------
# project scaffold (m1-project-new) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_project_scaffold() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/new_fixture"
    "$bin" new "$build_dir/new_fixture" --json \
        | jq -e '.ok and (.files | length)==5 and .name=="new_fixture"' >/dev/null
    "$bin" validate "$build_dir/new_fixture/midday.project.yaml" --json \
        | jq -e '.ok and .schema=="project" and .format_version.authored==1' >/dev/null
    "$bin" validate "$build_dir/new_fixture/midday.import.yaml" --json \
        | jq -e '.ok and .schema=="import_policy" and .format_version.authored==1' >/dev/null
    "$bin" validate "$build_dir/new_fixture/default.input.yaml" --json \
        | jq -e '.ok and .schema=="input" and .actions==5 and .sticks==1' >/dev/null
    "$bin" run "$build_dir/new_fixture/scenes/main.scene.yaml" --json \
        | jq -e '.ok and .entities==0 and .ticks==0' >/dev/null

    local f
    for f in midday.project.yaml midday.import.yaml default.input.yaml scenes/main.scene.yaml; do
        "$bin" fmt "$build_dir/new_fixture/$f" --check --json | jq -e '.ok and .canonical' >/dev/null
    done

    local new_status=0 new_out
    new_out=$("$bin" new "$build_dir/new_fixture" --json) || new_status=$?
    [ "$new_status" -eq 1 ]
    echo "$new_out" | jq -e '.error.code == "new.target_exists"' >/dev/null

    [ "$(find "$build_dir/new_fixture" -type f | wc -l | tr -d ' ')" -eq 5 ]
    ! grep -rq "$PWD" "$build_dir/new_fixture"
    [ ! -d "$build_dir/new_fixture/.midday-cache" ]
}

# ---------------------------------------------------------------------------
# scene format (m1-scene-format) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_scene_format() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    "$bin" scene print "examples/appendix_a/boss.machine.yaml" --full --json \
        | jq -e '.ok and (.yaml | contains("Transition:")) and ((.yaml | contains("\non:")) | not)' \
        >/dev/null

    rm -rf "$build_dir/m1_scene_roundtrip"
    mkdir -p "$build_dir/m1_scene_roundtrip/scripts"
    cp "examples/appendix_a/boss.events.yaml" "$build_dir/m1_scene_roundtrip/"
    cp "examples/appendix_a/scripts/slash_attack.ts" "examples/appendix_a/scripts/dead.ts" \
        "$build_dir/m1_scene_roundtrip/scripts/"
    "$bin" scene print "examples/appendix_a/boss.machine.yaml" --full --json \
        | jq -r '.yaml' >"$build_dir/m1_scene_roundtrip/once.machine.yaml"
    "$bin" scene print "$build_dir/m1_scene_roundtrip/once.machine.yaml" --full --json \
        | jq -r '.yaml' >"$build_dir/m1_scene_roundtrip/twice.machine.yaml"
    cmp "$build_dir/m1_scene_roundtrip/once.machine.yaml" "$build_dir/m1_scene_roundtrip/twice.machine.yaml"

    "$bin" scene print "examples/warden/scenes/arena.scene.yaml" --full --json \
        >"$build_dir/m1_scene_roundtrip/arena_full.json"
    local arena="$build_dir/m1_scene_roundtrip/arena_full.json"
    jq -e '.ok and (.gaps | length) > 0' "$arena" >/dev/null
    jq -e '[.gaps[] | select(.kind=="component" and .what=="Perception")] | length == 1' "$arena" \
        >/dev/null
    jq -e '[.gaps[] | select(.kind=="prefab" and (.what|contains("player.entity.yaml")))] | length == 1' \
        "$arena" >/dev/null
    local warden_machines='[.prefab_entities[] | select(.entity=="Warden")][0].machines[0]'
    jq -e "${warden_machines}.effective_yaml | contains(\"duration: 1\")" "$arena" >/dev/null
    jq -e "${warden_machines}.effective_yaml | contains(\"amount: 55\")" "$arena" >/dev/null
    jq -e '[.prefab_entities[] | select(.entity=="Player")][0].resolved == false' "$arena" >/dev/null

    "$bin" scene print "examples/warden/brains/warden.machine.yaml" --full --json \
        | jq -e '.ok and ([.gaps[] | select(.kind=="script" and (.what|contains("chase.ts")))]
            | length == 1) and ([.gaps[] | select(.kind=="statechart")] | length == 1)' >/dev/null
    "$bin" scene print "examples/warden/prefabs/warden.entity.yaml" --full --json \
        | jq -e '.ok and ([.gaps[] | select(.what=="warden_mace.entity.yaml")] | length == 1)' \
        >/dev/null

    local f
    for f in examples/appendix_a/boss.scene.yaml examples/spikes/determinism.scene.yaml \
             examples/spikes/tainted/tainted.scene.yaml; do
        "$bin" scene print "$f" --json | jq -e '.ok and (.gaps == [])' >/dev/null
    done

    "$bin" validate "examples/appendix_a/boss.scene.yaml" --schema scene --json \
        | jq -e '.ok' >/dev/null
    "$bin" validate "examples/appendix_a/boss.machine.yaml" --schema machine --json \
        | jq -e '.ok' >/dev/null
    "$bin" validate "examples/warden/prefabs/warden.entity.yaml" --schema entity --json \
        | jq -e '.ok' >/dev/null
}

# ---------------------------------------------------------------------------
# warden contract audit (m1-warden-contract-audit) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_warden_audit() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    local audit="$build_dir/warden_audit.json"
    local audit_status=0
    "$bin" validate examples/warden --audit-missing --json >"$audit" || audit_status=$?
    [ "$audit_status" -eq 3 ]
    jq -e '.error.code == "validate.audit_missing" and .missing_count == 11' "$audit" >/dev/null
    jq -e '(.missing.files | map(.path) | sort) == [
        "goldens/warden_dead.png",
        "models/arena_floor.model.yaml",
        "models/warden_body.model.yaml",
        "prefabs/player.entity.yaml",
        "prefabs/warden_mace.entity.yaml",
        "states/chase.ts",
        "states/staggered.ts"
      ]' "$audit" >/dev/null
    jq -e '(.missing.components | map(.name) | sort) == ["NavFollow","Perception","StaggerTimer"]' \
        "$audit" >/dev/null
    jq -e '(.missing.wiring | length) == 1 and .missing.wiring[0].kind == "animator_rig"
        and (.missing.wiring[0].implied_by == "models/warden_body.model.yaml")' "$audit" >/dev/null
    jq -e '[.missing.components[].name] | inside(["NavFollow","Perception","StaggerTimer"])
        and (index("Health") == null) and (index("DamageOnTouch") == null)' "$audit" >/dev/null
    jq -e '([.out_of_scope[].what] | unique | sort) ==
        ["MeshRenderer","SlashAttack.initial","Spline","VirtualCamera","self.recovered"]' \
        "$audit" >/dev/null
    jq -e '(.present_files | length) == 5 and (.present_scripts | length) == 5' "$audit" >/dev/null
}

# ---------------------------------------------------------------------------
# prefab spawn (m1-prefab-spawn) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_prefab_spawn() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    "$bin" selftest --filter 'loader.prefab_spawn*' >/dev/null
    "$bin" selftest --filter 'script.world_host*' >/dev/null

    rm -rf "$build_dir/prefab_probe"
    mkdir -p "$build_dir/prefab_probe"
    printf 'format: 1\nmachine: goblin\nregions:\n  main:\n    initial: Idle\n    states:\n'\
'      Idle: {}\n' >"$build_dir/prefab_probe/goblin.machine.yaml"
    printf 'format: 1\nentity: Goblin\nmachines:\n  - instance: {path: goblin.machine.yaml}\n' \
        >"$build_dir/prefab_probe/goblin.entity.yaml"
    printf 'format: 1\nscene: arena\nentities:\n  - entity: Grunt\n'\
'    prefab: {path: goblin.entity.yaml}\n    at: [1, 0, 0]\n' \
        >"$build_dir/prefab_probe/arena.scene.yaml"
    "$bin" run "$build_dir/prefab_probe/arena.scene.yaml" --ticks 1 --json \
        | jq -e '.ok and .entities==1 and .machines==1' >/dev/null
}

# ---------------------------------------------------------------------------
# M2 smoke corpus (node 0A; grown at m2-testkit-core) — SHARED with Windows D-9
# ---------------------------------------------------------------------------

# The milestone-long smoke seed (testkit/smoke/): the minimal-complete
# non-warden scene — static ground + dynamic body + one machine on the
# builtin input-action route — runs headless to the tick count
# smoke.meta.json declares. That meta file is a CHECKED deferral contract,
# not a comment: this gate consumes ticks/seed from it AND refuses if the
# named owner nodes drift (input injection lands at m2-testkit-core,
# snapshot cadence at m2-snapshots). smoke.input.yaml is validated as
# corpus content only — the run verb does NOT load it in 0A (wiring it
# needs a new run flag, and verb argv schemas are part of api_compat_hash).
behavioral_smoke_corpus() { # <bin> <build_dir>
    local bin="$1" build_dir="$2" meta="testkit/smoke/smoke.meta.json"
    # .actions==1 is a PROJECT-WIDE walk of testkit/smoke/ (validate merges
    # every input map under the root) — a second input map under the smoke
    # dir MUST red this gate (pinned corpus, conscious change only).
    "$bin" validate "testkit/smoke/smoke.input.yaml" --json \
        | jq -e '.ok and .schema=="input" and .actions==1' >/dev/null
    jq -e '.input_action=="jump"
        and .input_activation_node=="m2-testkit-core"
        and .snapshot_every_ticks==256
        and .snapshot_activation_node=="m2-snapshots"' "$meta" >/dev/null
    local smoke_ticks smoke_seed smoke_action
    smoke_ticks=$(jq -er '.ticks' "$meta")
    smoke_seed=$(jq -er '.seed' "$meta")
    # The input map must DECLARE the meta-named action (two-space indent per
    # the canonical yaml) — meta and corpus cannot drift apart silently.
    smoke_action=$(jq -er '.input_action' "$meta")
    grep -q "^  ${smoke_action}:" "testkit/smoke/smoke.input.yaml"
    rm -rf "$build_dir/smoke"
    # `.ticks==300` deliberately DUPLICATES smoke.meta.json's ticks (the
    # corpus-identity pin): meta feeds --ticks, so without the literal a
    # meta tick drift would be self-consistently green (probe-proven,
    # council 0A). Changing the smoke run length is a conscious two-file
    # edit — the drifted meta reds HERE as an envelope tick mismatch.
    "$bin" run "testkit/smoke/smoke.scene.yaml" --ticks "$smoke_ticks" --seed "$smoke_seed" \
        --record "$build_dir/smoke/run.mrj" --cache-dir "$build_dir/smoke/ts-cache" --json \
        | jq -e --argjson ticks "$smoke_ticks" '.ok and .ticks==$ticks and .ticks==300
            and .entities==3 and .machines==1 and .bodies==2
            and .recorded_tier=="flight"' >/dev/null
}

# ---------------------------------------------------------------------------
# appendix A golden — SHARED with Windows D-9 (ci.yml build-windows)
# ---------------------------------------------------------------------------

# The flagship golden: the authored A.3 corpus driven to tick 3200 with the
# assertion pack; the five item-21 verdicts hold; two INDEPENDENT runs diff
# identical (never a self-diff) via the engine's own `journal diff` — no
# byte-cmp, so this is safe to run on any platform including MSVC (Aurora
# D-9: cross-host BYTE equality stays the Linux determinism lane's contract).
behavioral_appendix_a_golden() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/golden"
    "$bin" run "examples/appendix_a/boss.scene.yaml" --to-tick 3200 --seed 7 \
        --record "$build_dir/golden/a1.mrj" --cache-dir "$build_dir/ts-cache.a" \
        --assert case=appendix_a_golden --json \
        | jq -e '.ok and .assertions.combat_transitions_at_3200==1
            and .assertions.hurtbox_inactive_before_dead_enter
            and .assertions.voided_stagger
            and .assertions.locomotion_still_chasing
            and .assertions.cause_chain_complete' >/dev/null
    "$bin" run "examples/appendix_a/boss.scene.yaml" --to-tick 3200 --seed 7 \
        --record "$build_dir/golden/a2.mrj" --cache-dir "$build_dir/ts-cache.a" \
        --assert case=appendix_a_golden --json >/dev/null
    "$bin" journal diff "$build_dir/golden/a1.mrj" "$build_dir/golden/a2.mrj" --json \
        | jq -e '.first_divergent_tick==null and .identical' >/dev/null
}

# ---------------------------------------------------------------------------
# determinism kata — semantic core SHARED with Windows D-9; byte-reinforce
# and the tainted lint gate are Linux/macOS-only additions verify.sh layers
# on top (all three ran as one step historically; split here so the D-9
# subset is independently callable without dragging the other two along).
# ---------------------------------------------------------------------------

# The kata must move TS GC churn + Jolt stepping + statechart cascades +
# sequence spans SIMULTANEOUSLY — the `.exercised.*` asserts run BEFORE any
# compare so an empty-scene diff can never vacuously pass. Two INDEPENDENT
# runs diffed via `journal diff` (never a self-diff, never a byte-cmp) — the
# exact D-9 shape Windows needs (catches Windows-side nondeterminism/uninit
# without ever requiring cross-host byte equality).
behavioral_determinism_kata_semantic() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/spike"
    "$bin" run "examples/spikes/determinism.scene.yaml" --ticks 600 --seed 123 \
        --record "$build_dir/spike/ka.mrj" --cache-dir "$build_dir/ts-cache.a" \
        --assert case=determinism_kata --json \
        | jq -e '.ok and .exercised.ts_gc_churn and .exercised.jolt_step
            and .exercised.statechart_transitions and .exercised.sequence_spans' >/dev/null
    "$bin" run "examples/spikes/determinism.scene.yaml" --ticks 600 --seed 123 \
        --record "$build_dir/spike/kb.mrj" --cache-dir "$build_dir/ts-cache.a" \
        --assert case=determinism_kata --json >/dev/null
    "$bin" journal diff "$build_dir/spike/ka.mrj" "$build_dir/spike/kb.mrj" --json \
        | jq -e '.first_divergent_tick==null and .identical' >/dev/null
}

# Extra LOCAL reinforcement beyond the D-9 semantic core: the normalized
# record streams (zstdcat per D-BUILD-080 — framing is transport, record
# content is the run) are byte-compared too. Requires
# behavioral_determinism_kata_semantic to have already populated
# $build_dir/spike/{ka,kb}.mrj. Linux/macOS only (needs zstdcat).
behavioral_determinism_kata_byte_reinforce() { # <build_dir>
    local build_dir="$1"
    zstdcat "$build_dir/spike/ka.mrj/journal.jsonl.zst" >"$build_dir/spike/ka.jsonl"
    zstdcat "$build_dir/spike/kb.mrj/journal.jsonl.zst" >"$build_dir/spike/kb.jsonl"
    cmp "$build_dir/spike/ka.jsonl" "$build_dir/spike/kb.jsonl"
}

# ---------------------------------------------------------------------------
# component_event_lifecycle golden (M2 node 0B, FUSED-SPEC D6) — Linux/macOS
# ---------------------------------------------------------------------------

# The milestone's THIRD golden: the authored examples/lifecycle corpus —
# typed two-param onEvent hydration (distinct EntityRefs, Vec3, signed-zero
# normalization), the canonical payload-byte envelope pinned literally, the
# 7-line A.2.1 exit chain, the REAL warden dead.ts, and the exact-tick
# despawn reap at 1 + ceil(4.0 * 60) = 241 — driven through the real run
# verb with --components wired. The committed manifest is REGENERATED from
# the TS source and byte-compared FIRST (extract drift reds loudly); then
# the twelve named verdicts must hold and two INDEPENDENT runs must diff
# identical via the engine's own `journal diff` (never a self-diff).
behavioral_component_event_lifecycle() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/lifecycle"
    mkdir -p "$build_dir/lifecycle"
    "$bin" script extract "examples/lifecycle/components/lifecycle_components.ts" \
        --out "$build_dir/lifecycle/components.json" \
        --cache-dir "$build_dir/ts-cache.lifecycle" --json \
        | jq -e '.ok and .components == 5' >/dev/null
    cmp "examples/lifecycle/lifecycle.components.json" "$build_dir/lifecycle/components.json"
    "$bin" run "examples/lifecycle/lifecycle.scene.yaml" --ticks 241 --seed 7 \
        --components "examples/lifecycle/lifecycle.components.json" \
        --record "$build_dir/lifecycle/l1.mrj" --cache-dir "$build_dir/ts-cache.lifecycle" \
        --assert case=component_event_lifecycle --json \
        | jq -e '.ok and .ticks==241
            and .assertions.initial_entry_seated_order
            and .assertions.exit_chain_seven_lines
            and .assertions.contact_payload_bytes_pinned
            and .assertions.canonical_payloads_verified
            and .assertions.typed_hydration_verified
            and .assertions.signed_zero_normalized
            and .assertions.base_transform_mirror_read
            and .assertions.kill_cause_chain
            and .assertions.boss_died_at_enter_once
            and .assertions.despawn_scheduled_due_241
            and .assertions.despawn_exit_order
            and .assertions.reaped_at_exactly_241' >/dev/null
    "$bin" run "examples/lifecycle/lifecycle.scene.yaml" --ticks 241 --seed 7 \
        --components "examples/lifecycle/lifecycle.components.json" \
        --record "$build_dir/lifecycle/l2.mrj" --cache-dir "$build_dir/ts-cache.lifecycle" \
        --assert case=component_event_lifecycle --json >/dev/null
    "$bin" journal diff "$build_dir/lifecycle/l1.mrj" "$build_dir/lifecycle/l2.mrj" --json \
        | jq -e '.first_divergent_tick==null and .identical' >/dev/null
}

# wall-clock taint: Date.now() must die at the LINT GATE (exit 3,
# script.lint, no-wall-clock at file:line) through the REAL run path, before
# a single tick executes. Linux/macOS only (not part of Windows's D-9 list).
behavioral_tainted_lint_gate() { # <bin> <build_dir>
    local bin="$1" build_dir="$2" taint_status=0 taint_out
    taint_out=$("$bin" run "examples/spikes/tainted/tainted.scene.yaml" --ticks 1 \
        --cache-dir "$build_dir/ts-cache.a" --json) || taint_status=$?
    [ "$taint_status" -eq 3 ]
    echo "$taint_out" | jq -e '.error.code == "script.lint"
        and .error.details.diagnostics[0].code == "no-wall-clock"
        and .error.details.diagnostics[0].line > 0' >/dev/null
}

# ---------------------------------------------------------------------------
# golden compare (m0-golden-compare) — Linux/macOS only
# ---------------------------------------------------------------------------

behavioral_golden_compare() { # <bin> <build_dir>
    local bin="$1" build_dir="$2"
    rm -rf "$build_dir/compare_fixtures"
    MIDDAY_COMPARE_FIXTURE_DIR="$PWD/$build_dir/compare_fixtures" \
        "$bin" selftest --filter 'compare.fixtures' >/dev/null
    local f
    for f in base.png identical.png noise.png shifted.png; do
        cmp "testkit/fixtures/goldens/$f" "$build_dir/compare_fixtures/$f"
    done
    if cmp -s "testkit/fixtures/goldens/base.png" "testkit/fixtures/goldens/identical.png"; then
        echo "identical.png must differ from base.png at the byte level (D-14 pin)"; exit 1
    fi
    "$bin" shot compare "testkit/fixtures/goldens/base.png" \
        "testkit/fixtures/goldens/identical.png" --json \
        | jq -e '.ok and .hash_equal and .tolerance.pass and .pass
            and .pixel_hash_a == .pixel_hash_b' >/dev/null
    "$bin" shot compare "testkit/fixtures/goldens/base.png" \
        "testkit/fixtures/goldens/noise.png" --json \
        | jq -e '.ok and (.hash_equal | not) and .tolerance.pass and .pass
            and .tolerance.max_channel_delta == 1' >/dev/null
    local shot_status=0 shot_out
    shot_out=$("$bin" shot compare "testkit/fixtures/goldens/base.png" \
        "testkit/fixtures/goldens/shifted.png" --diff "$build_dir/compare_diff.png" --json) \
        || shot_status=$?
    [ "$shot_status" -eq 1 ]
    echo "$shot_out" | jq -e '(.ok | not) and .error.code == "shot.mismatch"
        and (.hash_equal | not) and (.tolerance.pass | not) and (.pass | not)
        and .tolerance.pct_pixels_over > 0' >/dev/null
    test -s "$build_dir/compare_diff.png"
}

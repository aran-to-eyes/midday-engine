# Midday Engine — Load-Bearing Contracts (Milestone 0)

The authoritative index of the engine's load-bearing contracts as of milestone 0: what a
clean-context coding agent may rely on, which gate falsifies each guarantee, who consumes it
first, and — honestly — what is NOT promised. One section per contract: **Guarantee / Enforced by
/ First consumer / Limits**.

Enforcement lives in two mirrored places: the local gate `scripts/verify.sh` (steps named below
exactly as it prints them) and `.github/workflows/ci.yml` (jobs: `verify-linux` — runs verify.sh
verbatim — `build-linux`, `sanitizer-linux`, `drift`, `determinism` (two-host matrix `a`/`b`),
`determinism-compare`, `build-macos`, `boundaries`, `golden-software`, `build-windows`). "Enforced
by selftest" means a doctest case compiled into the `midday` binary, run by every lane that
executes `midday selftest` (all five build/verify lanes, plus the `rhi.*` filter on
`golden-software`).

**Change policy:** a contract here may change only in a commit that updates its enforcing gate in
the same commit. If a gate and this document disagree, the gate is the truth and this document has
a bug.

---

## 1. Deterministic tick + statechart execution (Appendix A.1 / A.2 / A.2.1)

**Guarantee.** Every fixed tick runs Appendix A.1's nine phases in contractual order; transitions
execute inline per the A.2 algorithm (priority, declaration-order tie-break, any-state-first, one
transition per region per tick, losers journaled as voided); enter/exit hooks run in the exact
A.2.1 order (exit: state script → spans/substates deepest-first → components reverse-attach →
subtree deactivate; enter is the mirror). Same build + seed + inputs ⇒ byte-identical journal
record streams across independent runs. Core semantics are proven C++-first (no QuickJS in the
process); TS hook parity through the generated bindings is a separately gated claim.

**Enforced by.** `midday selftest --filter statechart.*` / `sequence.*` / `tick.*`; verify.sh
steps "appendix A golden (3200-tick assert pack + independent dual-run diff)" and "yaml loader +
run (boss corpus: flight journal from run 1, same-seed dual-run diff)"; CI `determinism` job steps
"Appendix A x3 (assert pack + normalized sha256)" and "Within-host independent dual-run diff (both
corpora)", then `determinism-compare` step "Cross-host normalized byte-compare (3x sha256 each,
both corpora)" across two independent Linux x86_64 hosts. Bit-identity is always two independent
runs diffed, never a self-diff.

**First consumer.** The Appendix A.3 boss golden (`examples/appendix_a/`) driven by `midday run` —
the flagship falsifier for the whole sim stack.

**Limits.** Cross-host BYTE identity is gated on the pinned platform class only (Linux x86_64,
`ci` preset). macOS arm64 gates the same SEMANTIC assertions plus a same-host dual-run diff — its
journal hash is reported, non-gating (`build-macos` step "Appendix A golden — semantic report
(bytes NON-GATING here, Aurora D-9)" and the kata twin). Windows runs selftest-level dual-run
compares only. The sim is single-threaded in M0. Frame-packet extraction exists as a seam but has
no consumer yet.

## 2. The `run.mrj` journal: FLIGHT always-on, cause-id chains, byte determinism

**Guarantee.** Every run-capable verb records a `run.mrj` bundle from tick 0 — FLIGHT recording is
on by construction, no off switch exists (`recorded_tier=="flight"` in the `midday run` payload).
Format (`formats/run_mrj.md`): `header.json` (identity/info split + replay-identity hash; readers
refuse drifted expectations with `journal.replay_refusal`), `journal.jsonl.zst` (one JSON object
per line in standard zstd frames — `zstdcat | grep` works anywhere), `index.json` (tick →
byte-offset seek). Records carry `{tick, tier, kind, cause_id, id, payload}`; every submission
consumes an id even when tier-filtered, so FLIGHT bytes never vary with snapshot/trace config;
cause chains reconstruct mechanically end to end (gated by the A.3 `cause_chain_complete`
assertion). Bundle bytes are a pure function of (submissions, config, flush sequence); zstd is
vendored, pinned, single-threaded.

**Enforced by.** verify.sh step "journal fixture (byte-pinned bundle + real zstdcat greppability)"
(regenerates and byte-compares `testkit/fixtures/journal/greppable.mrj`, greps via the real
zstdcat); `midday selftest --filter journal.*`; the CI `determinism` lane hashes the DECOMPRESSED
record stream — compression framing is transport, record content is the run.

**First consumer.** `core/bus` (every accepted trigger writes a FLIGHT record before dispatch and
hands its id out as the cause id); `midday run --record` and `midday journal diff`
(first-divergent-tick over two bundles).

**Limits.** `snapshots/` is an empty sidecar slot until snapshot content lands (M2); the SNAPSHOT
and TRACE tiers exist as vocabulary but have no gated content in M0. Compare decompressed streams,
never assume frame layout. `info.created_at` is the single sanctioned wall-clock slot, excluded
from every hash and compare. Id gaps in a journal are normal (filtered records).

## 3. Event bus dispatch semantics

**Guarantee.** The bus is the game's only transition mechanism: keyed channels (keys are
capabilities), IMMEDIATE registration-order dispatch on the same call stack, re-entrant cascades
capped at depth 32 (`bus.cascade_depth` structured error, no unwinding), subscribe/unsubscribe
deferred to dispatch end while dispatching, typed payload validation against the reflected event
vocabulary, and a FLIGHT `event.trigger` record written before dispatch whose id is the cause id
for everything the listeners do. Component subscriptions auto-unsubscribe on despawn (rows are
re-fetched, never pointer-cached); dormant components hear nothing by default.

**Enforced by.** `midday selftest --filter bus.*` — registration order, key isolation, the
depth-33 structured error with the journaled chain, byte-pinned trigger records, cause-chain
walks, dual-run record identity. Every `determinism`-lane byte-compare re-proves dispatch order
downstream.

**First consumer.** `core/statechart` transition evaluation (one subscription per key);
`core/physics` (`contact.began`/`contact.ended` in phase-6 order sorted by body-pair id); the tick
loop drives `set_tick`.

**Limits.** Dispatch is synchronous and immediate — no queued/async delivery tier and no
cross-thread story in M0. The depth cap is shared with statechart cascades. Unregistered event
names cannot be triggered.

## 4. JSON byte-determinism (the tree-wide writer/parser)

**Guarantee.** One JSON implementation (`core/base/json.h`) serves the whole tree:
insertion-ordered objects, strict RFC 8259 parse with structured `origin:line:col` errors and
never exceptions. Double emission goes through vendored dragonbox (shortest round-trip,
shorter-of-fixed/scientific, ties to fixed, exact integer expansion) and double parsing through
vendored fast_float — never the standard library's FP conversions. Same double, same JSON bytes,
on every supported platform and toolchain. Integers use `std::to_chars`/`from_chars`
(locale-free); non-finite doubles serialize as `null`.

**Enforced by.** The `core.json` known-answer corpus in selftest (validated against a 64.6M-double
`std::to_chars` cross-check at introduction — any change to the conversion stack surfaces there as
a diff), running on all three OS lanes; every byte-compare gate in the tree sits on this guarantee
and re-falsifies it daily.

**First consumer.** The CLI envelope, journal records, `engine_api.json` emission, and both code
generators (`ts/codegen/json.ts` reproduces the writer byte-for-byte, pinned by the number-edge
corpus).

**Limits.** Output is compact only (no pretty-printer contract). Integer tokens beyond int64 range
degrade to double; `-0` parses as double and dumps as `-0`. Parse depth is capped. The byte
guarantee covers this writer's output, not third-party JSON.

## 5. Name / hash identity

**Guarantee.** Interned `Name` ids are XXH3-64 content hashes of the string — identical across
runs, platforms, and intern order (never sequential handout); id 0 is reserved for empty; a
detected 64-bit collision aborts loudly. Wherever a 64-bit hash reaches JSON output or a CI
byte-compare it is spelled as 16-digit lowercase hex (`core/base/hex.h`) — one canonical spelling
tree-wide (compat hashes, golden pixel hashes, `math_fixture_hash`, the journal replay-identity
hash).

**Enforced by.** `midday selftest --filter core.*`; indirectly by every hash-bearing byte-compare:
the `drift` job's compat-hash checks, `golden-software`'s pixel-hash compare, the determinism
lane's journal hashes.

**First consumer.** `core/reflect` (registry lookup by interned Name), `core/bus` (EventKey), the
statechart's Name-referenced machine descriptors.

**Limits.** Collisions abort rather than resolve — Name interning is not an open-world hashing
service. Ids are content-derived: renaming a thing changes its identity everywhere, by design.

## 6. Reflection registry + `engine_api.json` + compat hashes

**Guarantee.** Every reflected component class, event payload schema, and expression function —
plus every CLI verb schema — is enumerated deterministically (init-level-major, registration order
within a level, never pointer/hash order) and dumped as `api/engine_api.json`: generated,
committed, canonical. Each entry carries a signature-only XXH3-64 compat hash (docs excluded),
plus one top-level `api_compat_hash`; `midday api diff` exits 1 with a structured
added/removed/changed report on any signature change. Two independent dumps are byte-identical.

**Enforced by.** verify.sh step "engine_api drift (two dumps byte-compared + committed artifact +
meta-schema)"; CI `drift` job steps "engine_api.json drift (dump twice, byte-compare, committed
diff)" and "engine_api.json meta-schema + envelope shape"; `midday selftest --filter reflect.*`
(pinned compat-hash known answers).

**First consumer.** The code generator (contract 7) — and any agent deciding what the engine
exposes: `engine_api.json` is the machine-readable source of truth for the API surface.

**Limits.** Compat hashes cover signatures only — doc edits never change a hash; `engine_version`
sits outside every hash. The registry holds descriptors, not invocation thunks (dispatch tables
live in the binding layer). An unregistered type is invisible to the whole reflection/codegen
chain by construction.

## 7. Codegen: self-hosted, byte-equivalent, drift-gated

**Guarantee.** All four derived artifacts — `api/engine.d.ts`, `api/schema_manifest.json`,
`api/api_docs.md`, `api/bindings_spec.json` — are generated from `engine_api.json` by the
SELF-HOSTED TS-on-QuickJS generator (`midday api codegen`, authoritative) as pure functions of the
input bytes (no timestamps, no absolute paths), byte-identical across platforms and across BOTH
generators (the temporary native bootstrap remains solely as the byte-equivalence pin). Every
formatting rule is written in `api/CODEGEN.md`; an unwritten rule is a doc defect. No subsystem
ever gets hand-written bindings.

**Enforced by.** verify.sh steps "codegen drift (selfhost authoritative: dual runs + committed
artifacts + meta-schema)" and "codegen byte-equivalence (selfhost vs TEMPORARY bootstrap, standing
gate until retirement)"; CI `drift` job steps "codegen drift (selfhost dual runs byte-compared +
committed artifacts)", "codegen byte-equivalence (selfhost vs bootstrap, standing gate)", and
"schema_manifest.json meta-schema"; `midday selftest --filter codegen.*`.

**First consumer.** `ts/toolchain` typechecks every script against the generated `engine.d.ts`;
`ts/runtime` implements `bindings_spec.json` (batch envelope and state-script hook seam — the
seam's global names are drift-gated generated data); agents read `api_docs.md`.

**Limits.** Two `bindings_spec.json` members are self-host-only glue excluded from the bootstrap
equivalence view (`batch_envelope` compared nulled, `state_script_hooks` dropped); everything else
is full-byte equivalent. The bootstrap tool is scheduled for deletion post-M0, retiring the
equivalence gate with it. `schema_manifest.json`'s `formats` array is empty until scene/machine
schemas join (M1).

## 8. CLI envelope + exit classes

**Guarantee.** Every `midday` verb emits the JSON envelope defined by
`formats/cli_envelope.schema.json`: `{"ok":true,...}` on success, `{"ok":false,
"error":{code,message,details}}` on failure, machine-parseable on stdout. Exit codes are classed
tree-wide: 0 ok · 1 failure (the tool's/infrastructure's fault) · 2 usage (argv-level, refused by
the framework before the verb runs) · 3 validation (your input's fault — type errors, lint hits,
unknown YAML keys, malformed documents, with structured file/line diagnostics). Verb schemas ship
in `engine_api.json`; help is generated from the same metadata.

**Enforced by.** verify.sh step "cli envelope schema"; exit-class assertions threaded through the
verify.sh steps "script toolchain (fixtures through the real CLI: exit classes, cache, lint)" and
"yaml loader + run (…)" (exit-3 fixtures asserted by exact status and error code); CI "Envelope
smoke (version --json, schema-validated)" steps on `build-linux`, `build-macos`, and
`build-windows`; `midday selftest --filter cli.*`.

**First consumer.** Every CI gate that pipes `midday … --json | jq -e` — the envelope is what
makes the gate architecture scriptable — and every agent driving the engine.

**Limits.** Only the JSON envelope is stable; human-readable output has no byte contract. Error
`details` shapes are per-code, documented at their producers, and may grow fields additively.

## 9. TS toolchain: cache, lint pack, gas

**Guarantee.** The vendored TypeScript compiler runs ON the embedded QuickJS — no Node anywhere.
`midday script check` typechecks against the generated `engine.d.ts` (skipLibCheck OFF) plus the
engine lint pack; `build` adds transpile through a content-addressed XXH3-128 cache (fingerprint
covers compiler, driver, options, `engine.d.ts`, and every `ts/lib/*.ts` — a hit soundly skips
compile AND check; the second build reports zero re-transpiles). The lint pack is an AST walk,
bypass policy none: no-wall-clock, no-unseeded-random, no-timer, each at file:line:col, exit 3
before a single tick executes. Script execution is gas-metered: the interrupt budget counts VM
poll points — a pure function of the executed program, never wall-clock — and the SIM runtime
profile poisons Date/performance/Math.random at the prelude (`script.nondeterminism`).

**Enforced by.** verify.sh step "script toolchain (fixtures through the real CLI: exit classes,
cache, lint)" (clean check, cache stats, cross-cache-dir byte-compare of emitted JS,
`script.type_error` and the three-family lint corpus by exact code and location); the
tainted-scene gates — verify.sh "determinism kata (…)" and the CI `determinism` step
"Wall-clock-tainted variant dies at the lint gate (exit 3, pre-tick)"; gas determinism via the
`script.runtime` selftest cases (identical programs consume identical gas in two independent
runtimes).

**First consumer.** State scripts seated on statechart states (the Appendix A corpus builds
through this cache on every `midday run`); the self-hosted code generator is itself a toolchain
consumer (TOOL profile).

**Limits.** Cache directories are regenerable build output — never drift-gated, never committed.
Determinism guarantees apply to the SIM profile; the TOOL profile sets `deterministic = false`.
Lint is a determinism tripwire, not a sandbox; the sandbox is the poisoned prelude plus the
absence of quickjs-libc (no timers exist).

## 10. Batch-binding budgets

**Guarantee.** Scripts touch component data through batch views (`midday/batch`): the engine
publishes SoA typed-array views of the active join per phase, scripts compute over arrays, and one
`commit()` scatters writable columns back at a deterministic point. Boundary crossings per tick
are O(exposed buffers), never O(entities): `boundary_crossings_per_tick <= 8 * pool_count`,
CONSTANT across a 1k/10k/100k entity sweep, ZERO steady-state GC allocation per tick
(`gc_alloc_bytes_per_tick == 0` with the pooled `midday/math` types), and naive per-field mode
measured at >= 10x more crossings. Capacity growth detaches old ArrayBuffers; rows vanishing
mid-phase refuse the commit (`bindings.stale_view`).

**Enforced by.** verify.sh step "batch-binding budgets (1k/10k/100k sweep + naive ratio,
m0-batch-bindings exit tests)" driving `midday script bench --json` with jq assertions on every
budget; the `bindings.*` selftest fixtures.

**First consumer.** The determinism kata's agent-style TS workload and the Appendix A state
scripts — every scripted sim path crosses this boundary.

**Limits.** Only scalar/vector columns batch (`bool`/`int`/`float`/vec2-4/quat/color); strings,
names, refs, arrays, and maps stay on the structured JSON seam by design. `int` travels in f64
buffers (2^53-exact). The `<= 8 * pool_count` bound is the contract; the observed constant (16
crossings for 3 pools) is not. Read-only columns never scatter back.

## 11. Strict YAML loader + `midday run`

**Guarantee.** The PERMANENT loader subset (`core/loader`, grammar contract
`formats/loader_yaml.md`) takes authored `*.scene.yaml` / `*.machine.yaml` / `*.events.yaml` to
live World/Hierarchy instantiation — the exact agent path, no fixture loader exists anywhere, and
no public path assembles entities in code. Strictness is the product: unknown keys, bad state
refs, anchors/aliases/tags, duplicate keys, and multi-document streams are refused with structured
errors at file/line, exit 3. `midday run <scene> --ticks|--to-tick --seed --record <mrj>` is the
headless drive verb.

**Enforced by.** verify.sh step "yaml loader + run (boss corpus: flight journal from run 1,
same-seed dual-run diff)" (boss corpus to tick 100, `recorded_tier=="flight"`, dual-run `journal
diff` clean, unknown-key fixture asserted as exit 3 / `loader.unknown_key` / line 3); `midday
selftest --filter loader.*`; every `determinism`-lane run re-drives the full path from authored
text.

**First consumer.** The Appendix A golden corpus (`examples/appendix_a/`) and the determinism kata
— both exist only as authored YAML + TS.

**Limits.** M0 subset only: base components, regions/states/substates/sequences, transition pairs
(any-state/priority/if), `on:`/`then:` sugar, symbolic keys, children under states, state-script
refs. Schema-manifest validation, uid dual-write, and prefab overrides extend this loader IN PLACE
at M1, with every M0 fixture still loading unchanged.

## 12. The RHI seam, two-backend conformance, and golden classes

**Guarantee.** All render code targets the RHI (`core/rhi/rhi.h`), never a GPU API. The boundary
is mechanical: vulkan/volk/VMA headers only under `core/rhi/vulkan/`, glslang/SPIRV only under
`core/rhi/shadercomp/`, Metal only under `core/rhi/metal/`. Handles are generational and typed;
stale/null lookups are structured refusals, never UB. The coordinate contract is pinned: the seam
is Vulkan-convention y-DOWN clip space; the ONE adaptation is a vertex-stage clip-space y negation
emitted in the MSL translation — never a viewport flip, never a readback flip. Failures are
structured error spellings, not crashes: `rhi.unavailable`, `rhi.device_fault` (contained
backend/ObjC escapes), `rhi.golden_missing`, `rhi.golden_mismatch`, `rhi.golden_driver_mismatch`.
Headless render → PNG is a launch requirement: every path works with no window, no display server,
no X11/Wayland sockets. Comparison semantics are two-tier: tier 1 hash-equality (XXH3-64 over
DECODED pixels — never encoded file bytes) is valid only within a pinned driver class; tier 2
per-pixel tolerance + diff-image emission is the cross-machine/cross-backend verdict (`midday shot
compare` reports both; the caller chooses which gates). Software-Vulkan goldens are minted only
from inspected images on the pinned lavapipe container and carry
`testkit/goldens/m0/DRIVER_PIN.txt` — the exact driver string, enforced at compare time, so a
silently drifting runner can never corrupt goldens. The RHI demonstrably spans two dissimilar
APIs: the SAME scenes render through native Metal and Vulkan and match within tier-2 tolerance.

**Enforced by.** verify.sh step "RHI include boundaries (self-test + scan)" and the CI
`boundaries` job (steps "Boundary scanner self-test" then "RHI include boundaries" — the scanner
proves it still catches planted violations before the clean scan is trusted); the conformance
corpus (`core/rhi/conformance_test.cpp`) — ONE corpus, every backend — via `midday selftest`
(NullDevice everywhere; native Metal on `build-macos`, where Metal skips are forbidden; lavapipe
on `golden-software` step "rhi selftests against lavapipe (skips are impossible here)");
`golden-software` steps "Probe — a SOFTWARE device must exist, display-less" and "Render M0 scenes
+ compare DECODED-PIXEL hashes against goldens" (hard exit on `rhi.golden_missing` — a stubbed
green is forbidden); verify.sh step "golden compare (fixture regen byte-compare + two-tier exit
tests via the CLI)" driving the identical/noise/shifted triplet through the real verb (identical =
different file bytes, same pixels — the committed proof that hashes cover decoded pixels);
`build-macos` steps "Metal probe — the hosted runner must expose a Metal device" and
"Cross-backend compare — SAME scenes through native Metal AND Vulkan, tier 2".

**First consumer.** The Vulkan and Metal backends themselves, `midday rhi render`, and the
`golden-software` lane; every future render feature declares its required runner class rather than
pretending lavapipe has the hardware.

**Limits.** M0 surface is offscreen-only: clear/triangle/textured-quad scenes, no swapchain, no
render graph yet; two backends, D3D12 future scope; the NullDevice is protocol truth, not a
rasterizer. Hash equality is pinned-driver-class only — across GPUs/drivers only tier 2 applies;
sim determinism stays bit-exact, pixel determinism is scoped honestly. On the hosted macOS runner
the cross-backend compare degrades LOUDLY (warning + Metal-only render) when MoltenVK cannot
create a device on the paravirtual GPU; the native-Metal conformance gate stays hard, and the
standing cross-backend evidence is a real-hardware run (Apple M4: tier-1 hash-identical, all three
scenes). Lavapipe proves headless correctness, never performance or hardware-feature claims.

## 13. Deterministic floating-point policy (BIT-PORTABLE vs LIBM-BOUND)

**Guarantee.** Every sim target compiles under pinned FP flags (`cmake/DeterministicFP.cmake`): no
fast-math, no FMA contraction (`-fno-fast-math -ffp-contract=off`; MSVC `/fp:precise`); 32-bit x87
targets hard-fail configure. Result: `+ - * /`, `sqrt`, comparisons, and every integer op produce
identical bits across runs, hosts, and toolchains. Operations are classed honestly (full table:
`core/math/README.md`): BIT-PORTABLE (the above, plus Philox RNG, all distributions including
`normal()` via the controlled `det_log` polynomial, noise, splines, polynomial easings,
vec/mat/xform/intersect kernels — all known-answer bit-pinned) versus LIBM-BOUND
(`sin/cos/acos/exp2/pow/log` and dependents — deterministic within one build, NOT bit-identical
across platforms/libcs). The expression language and the `midday/math` script library admit
BIT-PORTABLE operations only; a needed transcendental lands as a controlled polynomial first,
never raw libm. Jolt builds cross-platform-deterministic under the same contract, thread config
locked in deterministic mode.

**Enforced by.** Configure-time hard-fail on any fast-math opt-in; verify.sh step
"deterministic-FP flag scan" and the `build-linux` step "FP flag scan"
(`scripts/check_fp_flags.py` re-checks the emitted `compile_commands.json` per TU); `midday
selftest --filter math.*` (known-answer bit pins incl. the 1,000,000-op fixture and the official
Random123 vectors); ultimately every journal byte-compare in the `determinism` lane.

**First consumer.** `core/math` kernels, `core/expr` evaluation, the physics step — every number
the sim produces.

**Limits.** LIBM-BOUND results are pinned per build only — exactly what the M0 determinism
contract gates. The falsifiable per-TU flag scan runs on the Linux `compile_commands.json` (CI)
and wherever verify.sh runs locally; on `build-macos`/`build-windows` the flags are enforced at
the CMake level without an independent scan step. Cross-platform bit-identity beyond the pinned
lane remains a contract-level stretch goal, though every BIT-PORTABLE-class operation already
meets it.

## 14. The determinism kata: an exercised gate, never a vacuous one

**Guarantee.** The determinism byte-compare is only accepted from a run that provably exercised
the risky machinery SIMULTANEOUSLY: agent-style TS allocation/GC churn, Jolt stepping, event
cascades + statechart transitions, sequence spans, and seeded RNG. `midday run` on the kata scene
reports `.exercised.{ts_gc_churn, jolt_step, statechart_transitions, sequence_spans}` and every
gate asserts all four BEFORE any compare — an empty-scene byte-match can never pass as determinism
evidence. A wall-clock-tainted variant of the kata must die at the lint gate (exit 3,
`script.lint`, `no-wall-clock`) before a single tick executes.

**Enforced by.** verify.sh step "determinism kata (600-tick exercised asserts + dual-run compare +
tainted lint gate)" (asserts, dual-run `journal diff`, then a raw `cmp` of the two decompressed
record streams); CI `determinism` job steps "Determinism kata x3 (exercised asserts + normalized
sha256)" and "Wall-clock-tainted variant dies at the lint gate (exit 3, pre-tick)", cross-host via
`determinism-compare`; macOS semantic twin on `build-macos` (non-gating bytes, gating asserts).

**First consumer.** The determinism contract itself — the kata is the standing regression tripwire
that keeps contracts 1, 2, 9, 10, and 13 falsifiable as the engine grows. New risky machinery is
expected to join the kata scene, not get its own unexercised compare.

**Limits.** The exercised flags prove the kata moved each subsystem, not that it covers every code
path within them — subsystem-level determinism pins live in the subsystems' own selftests. Byte
scope is the same as contract 1: pinned platform class gates bytes; macOS gates semantics.

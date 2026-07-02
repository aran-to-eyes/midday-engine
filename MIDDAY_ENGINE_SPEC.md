# Midday Engine — Full Specification Sheet

**An AI-first, open-source, full-3D game engine.** AI-first means: coding agents are first-class
game developers. The engine's canonical interface is text, CLI, and images; its human face is
**Midday** — an embodied agent living in an infinite-canvas editor — and everything works fully
without any AI at all.

Status: consultation complete (2026-07-02). Companion doc: `GODOT_REFERENCE_SPEC.md` (the analyzed
Godot 4.8 baseline this spec inherits from and deviates from deliberately).

---

## 1. Mission & success criterion

- **Thesis**: an engine designed from scratch for AI agents as developers — text-native formats,
  fully headless operation, deterministic replay, machine-readable everything, and built-in
  perception channels so agents can see and verify their own work.
- **V1 outcome**: the engine plus **one complete showcase game built by an agent through the
  engine's intended workflow**.
- **Success bar ("nailed it")**: a one-paragraph game idea goes in; an agent using only the
  engine's tools and docs delivers a playable, reasonably fun 3D game; **zero human-written code**.

## 2. Locked decisions (consultation record)

| Decision | Choice |
|---|---|
| AI-first meaning | AI as the developer (runtime AI explicitly out of v1) |
| Relationship to Godot | Greenfield build; Godot spec is the blueprint, not the base |
| Game scope | Full 3D capabilities |
| Core stack | C++ + Vulkan (MoltenVK on macOS; RHI abstraction like Godot's RenderingDevice) |
| Script tier | Embedded TypeScript (QuickJS first, V8 as later swap) — game logic never recompiles the core |
| Physics | Jolt Physics (MIT) — deterministic mode for replay, threaded mode for play |
| Rendering bar | **Full Godot-4 3D parity = v1 definition of done**, phased M1→M3 (showcase gates on M1) |
| Modeling module | Parametric + CSG op-lists as source of truth; meshes are compiled output |
| Modeling perception | Coordinate-gridded ortho views + arbitrary cross-section slices + turntable renders (visual-primary); structured geometry queries live in the testing pillar |
| AI pillars in v1 | All five: game-testing layer, golden-frame verification, schema + validate-before-write, query/explain over recorded runs, modeling module |
| Licensing | Everything open source — engine and all dependencies (no proprietary SDKs, ever) |
| Entity model | Pure data-oriented ECS storage; **every entity is a statechart by default** — base components (always live) + states owning component sets (§4.1) |
| State memory | Components persist for entity lifetime (toggle active/inactive with their state); resets are explicit in lifecycle hooks |
| State structure | Substates + parallel states + sequenced states (dope sheet: trigger + span tracks), arbitrarily composable |
| Hooks | Per-state script (the state's brain) + per-component hooks: onEnter/onUpdate/onFixedUpdate/onExit |
| Event system | Keyed event bus: EventListener interface, `subscribe(this, key)` / `trigger(key)`, immediate dispatch; keys = capability scoping (entity UUID = private channel) |
| Transitions | **Event-driven only** — no setState API; `Transition` components on states declare `{event, goto, priority, if}` pairs; machine owns only any-state rules; states hold by default |
| States as nodes | States are scene-hierarchy nodes; child objects attach to states (hurtbox under Attack); state active ⇒ subtree live |
| Machine reuse | Machines are prefab subtrees — instanced as assets with per-entity property-diff overrides |
| Conflicts | Priority numbers on pairs (tie → declaration order); one transition per region per tick; voided candidates journaled |
| Entry/history | `initial:` per region; opt-in `history: true`; interrupted sequences close spans deterministically |
| Introspection | Full read-only state queries everywhere (scripts, `if` filters, testkit, replay, live bridge); mutation only via bus |
| TS components | Code-first: TS classes + decorators are the schema source; methods allowed; C++ components typed via engine_api.json → .d.ts |
| TS access | Typed `entity.get(Class)` + `world.query()`; generational handles with `.alive`; stale access = structured error |
| TS lifetime | Prefab-only spawning with overrides; structural changes apply between tick phases; no code-assembled entities |
| TS errors | Dev: halt at tick with structured error + replay bookmark; shipped: quarantine faulty component/state |
| Import trigger | Auto on demand (every verb imports dirty assets first); `midday import` for CI pre-warm |
| Asset refs | Dual-write `{uid, path}` — uid authoritative (engine-assigned, never hand-minted), path for readability; `midday check --fix` repairs drift |
| Import settings | Convention via project policy file + sidecars only where an asset deviates |
| Asset hot-reload | Yes in dev runs (journaled swap tick); never in replay mode |
| Journal format | `run.mrj` bundle: JSONL causality stream (+zstd) + binary snapshot sidecars + hash-pinned header + tick index |
| Record tiers | FLIGHT (always, shipped = rolling window) → SNAPSHOT (dev default, 300-tick cadence) → TRACE (opt-in deltas) |
| Replay queries | All v1: event/transition streams, state timelines, at-tick inspection, causality explain, property curves, run-diff |
| CSG kernel | OpenCASCADE (OCCT, LGPL dynamic) — full B-rep in v1: fillets/chamfers, exact surfaces, STEP interop |
| Model op set | Primitives · booleans · sketches + extrude/revolve/sweep/loft · fillets/chamfers · patterns · hull/offset/shell · anchors (exported sockets) |
| Geo addressing | Op ids + semantic selectors (faces/edges by predicate); no bare indices; failed selectors error with candidates |
| Model params | `params` block + expressions over params and prior-op measurements; TS builder emits canonical YAML |
| View annotations | All default-on: grid/axes/dims, per-op labels + leaders, anchor markers + key dims, slice contour coordinate tables |
| Platforms | Five (macOS/Win/Linux/Android/iOS) architected from day one; desktop @ M1, Android @ M2, iOS gates v1-done |
| GPU backends | RHI hard boundary; Vulkan v1 (MoltenVK on macOS) → native Metal M2 → D3D12 optional; SPIR-V once, translated per backend |
| Shaders | V1: uber-material + raw GLSL→SPIR-V; M2: shader graphs as validated YAML data; no custom DSL ever |
| SRP | Pipeline-as-data (render-graph YAML, dumpable) + TS custom passes; C++ only for new pass types |
| Math stdlib | Full library (vectors/matrices/quats, splines, easing, intersections, seeded RNG streams, noise, distributions) identical in C++ and TS |
| UI system | HTML/CSS via RmlUi (MIT), rendered through our pipeline; .rml/.css validated; data bindings; in-world screens |
| Viewport vision | All four channels v1: screenshots + flycam, entity-ID pick buffer/segmentation, debug-draw overlays, G-buffer taps |
| Procgen | Deterministic seeded stdlib: noise, Poisson/scatter, WFC, dungeon/BSP/maze, spline placement, weighted tables |
| Cameras | Cinemachine-class direction system: virtual-camera components, rigs, priority blending, event-driven cuts, shake |
| Director sequences | Scene-owned sequences choreographing many entities + camera + audio (Timeline-class cutscenes) |
| Save/load | Curated snapshots via `@field({save: true})`, slots, versioned migrations |
| Quality/loading | Named per-platform quality tiers selecting pipeline variants; async + additive scene loading |
| Editor | Midday Editor: infinite-canvas workspace, subcanvases instead of windows; built with the engine itself; starts M2, gates v1-done |
| Editor truth | Live document model (transactions, undo/redo) with text persistence — canonical YAML on disk, external edits hot-merge |
| Midday agent | Engine mascot + embodied agent; one `editor_api.json` tool surface shared by human chrome and Midday; shared undo stack/journal |
| Midday brain | Bring-your-own endpoint (Anthropic/OpenAI-compatible/local); harness is OSS; editor fully functional with no AI |

## 3. Architecture overview

```
midday/
  core/        C++20 · Vulkan RHI · Jolt · data-oriented ECS · statechart entities · event bus · deterministic sim loop
  api/         engine_api.json (generated, canonical) → engine.d.ts, docs, schema manifest
  ts/          embedded TS runtime · engine bindings · hot reload
  cli/         midday <verb> — the ONLY interface; every verb: JSON output + exit codes
  model/       CSG kernel · slicer · ortho/turntable renderer · mesh compiler
  testkit/     input playback · tick control · state snapshots/asserts · golden frames · geo queries
  replay/      input log + tick snapshots · bit-identical re-execution · query/explain
  formats/     scene/model/material text formats + validators
  editor/      Midday Editor (a midday app): infinite canvas · doc model · editor_api.json · agent harness
```

**Layering contract** (from Godot's proven pattern): core compiles rarely and is owned by
engine developers; everything an agent touches (scenes, TS scripts, models, materials, tests)
lives in the no-recompile tier. Agent feedback latency is bounded by script reload + headless
run — never by a C++ link.

## 4. Core runtime (C++)

Inherits the Godot reference architecture where it was validated as sound, with a novel entity
model (§4.1) and event system (§4.2) replacing the node-tree/signal design.

### 4.1 Entity model: statechart entities on ECS

**Storage is pure data-oriented ECS** — entities are IDs, components are data in tables, systems
are ordered logic over queries. **Authoring is statecharts**: every entity is a state machine by
default.

- **Entity = base components + state machine(s).** Base components (Transform, Collider,
  RigidBody, Health…) are always live — the entity's invariants. States own their component sets;
  only the active states' components are live (visible to system queries, receiving ticks).
  Scenes are trees of entities (transform hierarchy + instancing per the reference spec); ECS
  tables underneath.
- **States are scene-hierarchy nodes.** A state is a real node in the tree; entities and objects
  can parent under it (a hurtbox entity under the Attack state). State active ⇒ its subtree is
  live; state inactive ⇒ subtree dormant. State activation IS subtree activation — one mechanism,
  and the entity's behavioral structure is visible in the hierarchy itself.
- **Machines are prefab subtrees.** A machine (region → states → components/children/transitions)
  is a subtree, so reuse falls out of scene instancing: save as an asset
  (`brains/melee_enemy.machine.yaml`), instance on any entity, override per-entity via the same
  property-diff mechanism scenes use (retarget a transition, resize a hurtbox, retune a value).
- **Entry semantics**: every region/parent state declares `initial:`; re-entry starts there
  unless the region opts into `history: true` (resume last active substate; sequences resume
  their saved playhead). Interrupted sequences always clean deterministically: exit fires → all
  open spans close (children's onExit in reverse span order) → playhead resets (or saves, under
  history).
- **Persistence**: all components (base and state-owned) are created at entity spawn and persist
  for the entity's lifetime; state changes toggle active/inactive, never construct/destroy.
  Resets are explicit — written in lifecycle hooks — so nothing resets by surprise and nothing
  leaks by surprise.
- **Lifecycle hooks**: each state may carry one **state script** (the state's brain:
  `onEnter(from)`, `onUpdate(dt)`, `onFixedUpdate(dt)`, `onExit(to)`), and every script-component
  receives the same hook set around its own activation. Hook firing order is deterministic:
  exit chain (inside-out) → enter chain (outside-in) → component hooks in attach order.
- **Structure is fully composable**: **substates** (nested states stack their ancestors'
  component sets), **parallel states** (independent regions/layers, each with one active state;
  live set = base + union of all active states), and **sequences**.
- **Sequences (dope-sheet states)**: a state whose body is a timeline with **trigger tracks**
  (fire events at time t) and **span tracks** (child states/components active over [from, to]).
  No property keyframes — value animation belongs to the animation system. Keyframe times resolve
  to sim ticks (deterministic, replay-exact). Timeline end modes: `loop [n]` or `hold`; sequence
  end always emits the built-in `<state>.finished` event, and chaining is an ordinary transition
  pair on it (`then: <state>` is accepted as pure sugar for exactly that pair).
- **Query semantics**: systems match base + active-state components by default; querying inactive
  components is an explicit opt-in flag.

### 4.2 Event bus & transitions

**One keyed, journaled event bus is the game's nervous system — and its only transition
mechanism.**

- **Events are first-class definitions** (named, typed payload schemas, schema-visible to the
  validator and to agents).
- **Mechanics**: components implement the `EventListener` interface (`onEvent(event, payload)`);
  subscribe via `event.subscribe(this, key)`; `event.trigger(key, payload)` synchronously
  iterates all subscribers on that `(event, key)` channel — **immediate dispatch**, subscriber
  order = registration order (deterministic), re-entrancy depth-capped with a structured error.
- **Keys are capability scoping**: entity UUID as key = private entity-local channel (component↔
  component on one entity); any shared key = private group channel; well-known key = global
  broadcast. You hear only what you hold the key for.
- **Public event fields**: entities/components/states expose linkable event fields in their
  definitions; linking an event to a field in the entity file auto-subscribes it. The declarative
  **reaction palette** — call method, run script function, goto state, start/stop sequence, set
  component field, emit another event (chaining) — compiles down to generated listeners on the
  same interface. An entity file is therefore a complete, readable wiring diagram.
- **Transitions are event-driven only, declared as Transition components.** There is no
  `setState()` API, and the machine itself defines no default flow between states — **states hold
  until an event moves them**. Each state optionally carries a `Transition` component declaring
  `{event, goto, priority, if}` pairs (the `if` may include read-only state conditions). The
  machine owns only **any-state transitions** (wildcards like `on death.dealt → Dead`), declared
  the same way at region level. Condition transitions (`when: Health.value < 30`) are
  edge-triggered watchers that emit an internal event; sequence chaining rides `<state>.finished`.
- **Conflict resolution is explicit**: transition pairs carry `priority` (higher wins; tie →
  declaration order; any-state rules compete in the same priority space). At most one transition
  per region per tick; the journal records the winner AND the voided candidates with their events.
- **State introspection is read-only and universal**: scripts query own and other entities
  (`world.inState(boss, 'combat', 'Enraged')`), transition/reaction `if` filters accept
  is-in-state conditions, the testkit asserts on states, replay queries state timelines, the live
  bridge dumps machine status. The only mutation path remains the event bus.
- **Everything journals**: every trigger records (tick, event, key, payload, subscribers, cause
  chain) and every transition records (tick, entity, region, from→to, cause, voided candidates) —
  this journal IS the replay/explain pillar's causality timeline.

**Authoring conventions (fixed by the Warden stress-test, `examples/warden/`):**
- **Event definitions** live in `*.events.yaml` files (project-wide namespace, validated like all
  formats).
- **Key vocabulary at author time**: `key: self` (own entity) · `root` (owning entity of a
  state-subtree child) · `global` · `<group-name>` (named shared channel) — symbolic, resolved at
  spawn. Any holder of an EntityRef may trigger at that entity's key.
- **`on:` on a state is sugar** for a `Transition` component pair list; canonical serialization
  emits the component form.
- **Built-in engine events are enumerated in `engine_api.json`** like everything else:
  `trigger.entered/exited`, `contact.began/ended`, `<state>.finished`, spawn/despawn lifecycle.
  Nothing implicit.
- **Component emit sugar**: `this.emit(name, payload)` ≡ `events.trigger(name, payload,
  {key: this.entity})`; `entity.root()` returns the owning entity from any state-subtree child
  (for e.g. hurtbox damage attribution).
- **One expression language** — deterministic, typed, side-effect-free — shared by transition
  `if:` filters, `when:` watchers, and model `params` expressions; its function inventory is part
  of `engine_api.json`. No per-context mini-DSLs.
- **Override path grammar**: `<machine-name>/<Region>/<State>/<ChildEntity>/<Component>` —
  machines addressed by name, never index; all asset/script paths are project-root-relative.

### 4.3 Runtime services (from the validated reference architecture)

- **Server + handle pattern**: rendering/physics/audio/navigation behind server interfaces with
  opaque handles; headless swaps are first-class, not stubs that lie.
- **Main loop**: fixed sim tick (default 60 Hz, max catch-up steps, render interpolation),
  variable-rate render. `--fixed-fps`-equivalent determinism is a general engine mode.
- **Reflection as data**: every class/method/property/signal registered in a ClassDB-equivalent;
  `engine_api.json` (with docs inlined) is a **shipped, versioned artifact** — it generates the TS
  type definitions, the schema manifest, and the agent-facing docs. Per-method compat hashes.
  **Build-order mandate**: the reflection/registration system and the codegen chain
  (api.json emitter → `.d.ts` generator → schema manifest) are the FIRST infrastructure work
  item, proven on a single component before any subsystem lands — no subsystem ever gets
  hand-written bindings that must later be thrown away (Godot's GDCLASS blob is the cautionary
  tale).
- **Module init levels** CORE → SERVERS → SCENE (→ TOOLS), torn down in reverse.
- **Math**: real_t single-precision default; deterministic math policy documented (pinned FP flags);
  full vector/transform/geometry library per reference spec §1.
- **Input**: event hierarchy + named action maps; **synthetic injection is a public, stable API**
  (the testkit is built on it).

### Determinism contract (non-negotiable, pillar-supporting)
- Same build + same input log + same seed ⇒ **bit-identical simulation**, guaranteed, tested in CI.
- **Validated by a determinism spike before any subsystem work**: the first runnable slice of the
  engine is run repeatedly on two different machines and its sim journals byte-compared —
  Jolt config, QuickJS behavior (GC/iteration order in agent-style TS), and FP flags are proven,
  not assumed. The spike's dual-run byte-compare becomes a permanent CI lane.
- Replay mode: single-threaded or deterministically-reduced parallelism; Jolt deterministic mode;
  all engine RNG seeded and journaled; wall clock banned from sim code (lint-enforced).
- Snapshot/restore at the tick boundary (Godot analysis: between end-sync and step). Snapshots are
  serializable, diffable, and addressable by tick number.

## 5. Rendering (Vulkan)

- **RHI is the hard multi-backend boundary** (RenderingDevice-style, render-graph with automatic
  barriers on top): all engine and shader code targets the RHI, never a GPU API directly.
  Backends by milestone: **Vulkan at v1** (Windows/Linux/Android native; macOS via MoltenVK —
  production-proven) → **native Metal at M2** (macOS + the iOS path) → D3D12 optional later.
  Shaders compile once to SPIR-V; per-backend translation (MSL via SPIRV-Cross).
- **The render pipeline is data (the SRP)**: the frame is a declared render-graph document —
  passes, targets, resources, ordering — inspectable, diffable, validated like every other
  format; `midday render graph --dump` renders the frame structure. Custom passes are TS at
  setup/config level referencing GLSL (later shader-graph) or compute shaders for GPU work;
  C++ only for entirely new pass types. Determinism and golden-frames enforce per pass.
- **Headless GPU rendering is a launch requirement, not a mode**: every render path works without
  a window or display server — offscreen swapchain, `render → PNG/EXR bytes` as a synchronous API.
  This single decision fixes Godot's worst agent gap (headless = zero pixels).
- **Milestones (v1 done = M3 = Godot-4 3D parity per reference spec §3):**
  - **M1 — Lean modern PBR** (showcase game gates on this): forward+ clustered; directional/point/
    spot lights; cascaded shadow maps; HDR + ACES; bloom; MSAA; IBL environment + procedural sky;
    glTF 2.0 PBR materials; mesh LOD; frustum culling; 3D text; particles (GPU).
  - **M2 — Screen-space & fidelity**: SSAO, SSR, SSIL; DoF, auto-exposure; decals; TAA + SMAA;
    volumetric fog; occlusion culling; VRS.
  - **M3 — GI & scale (parity)**: realtime GI (SDFGI-class) + baked GI (lightmapper + probes) +
    voxel GI; FSR-class upscaling (OSS algorithms only); soft-body/vehicle render support;
    the long tail of reference spec §3. **The parity bar, precisely: Godot-4 / Unity-URP-class**
    — exactly where the industry is consolidating (Unity retiring Built-in RP, HDRP in
    maintenance). Hardware ray tracing and HDRP-class extras stay post-parity roadmap.
- **2D**: canvas layer for UI/HUD (batched), not a separate 2D engine in v1.
- **Shaders**: v1 = parameter-driven materials (PBR uber-shader, Godot StandardMaterial3D model)
  + custom GLSL with engine includes, compiled via glslang (BSD) to SPIR-V. **M2 = shader graphs
  as data**: typed node graphs in validated YAML, compiled through the same SPIR-V pipeline,
  previewable via `midday shader view` (graph render + material ball). No custom shading DSL ever
  (Godot's 406k-line parser is the cautionary tale).
- **Golden-frame infrastructure** (pillar): deterministic render mode (fixed seeds, stable
  reductions) → reference-image comparison as CLI verbs. **Comparison semantics are two-tier**
  (GPU output is not bit-identical across GPUs/drivers): across machines, goldens compare
  **per-pixel with tolerance + perceptual diff** (diff images emitted on failure); **hash-equal**
  comparison applies only within a pinned CI GPU class. Sim-side determinism stays bit-exact;
  pixel determinism is scoped honestly.

## 6. Physics & navigation

- **Jolt (MIT)** integrated behind the physics server interface: static/kinematic/rigid/character
  bodies, areas/triggers with override zones, standard shape set, ray/shape/swept queries,
  32-bit layers/masks, joints, vehicles, soft bodies (Jolt features adopted, not reimplemented).
  **Replay-mode threading config is pinned and documented**: deterministic Jolt configuration
  (fixed thread count or single-threaded island solve, stable insertion order) is the same config
  the determinism CI lane runs; play-mode may use full threading, replay-mode never diverges
  from the pinned config.
- Character controller: kinematic move-and-slide with floor snap/max-angle/one-way — the
  reference's most agent-debuggable component, kept.
- **Navigation** (Recast/Detour — zlib): per-agent-type navmesh surfaces (multiple bakes per
  scene, not one global), area types with traversal costs (path cost = distance × area cost),
  off-mesh links with auto-generation at bake, carving obstacles, runtime rebaking (tile-sized
  for memory) + A* + RVO avoidance.
- **Physics introspection is headless-first** (Unity's Physics Debugger, reimagined for agents):
  per-body state queries as JSON (velocities, sleep state, island); contact streams (points,
  impulses, separations) recorded in TRACE-tier replays and queryable; collider/contact/impulse
  overlays in the §12 vision channels. What Unity shows in an editor window, we expose to tests
  and `midday replay explain`.
- **Collision layers are named project data**: layers declared in project config; per-object
  32-bit layer/mask plus a project-level collision-matrix view that validators check (broad-phase
  culling like Unity's Layer Collision Matrix).
- **Ragdolls are a recipe, not a runtime system** (Unity's model, confirmed): a `midday` generator
  builds colliders/bodies/joints from a skeleton into a normal prefab; ships with the M2
  IK/rigging wave.
- Physics runs on the deterministic sim tick; per-tick snapshots feed replay.

## 7. Scripting tier (embedded TypeScript)

- Runtime: QuickJS (MIT) embedded in core for v1 (small, sandboxable, trivially deterministic);
  V8 (BSD) as a swap-in when perf demands. TS → JS transpile handled by the CLI toolchain.
- **Bindings are generated from `engine_api.json`** — one source of truth produces the C++ glue,
  `engine.d.ts`, and docs. Agents get full type checking against the real API.
- **The binding layer is batch-first (hard requirement)**: the classic embedded-scripting cost is
  boundary chatter, not script execution. ECS component data lives in C++ tables; TS query
  iteration receives **batched, typed views** (typed-array-backed), never per-field crossings in
  loops; math types are pooled/reusable to kill per-frame GC churn. The per-frame script budget
  instrumentation reports boundary-crossing counts alongside time, so a chatty pattern is visible
  the moment an agent writes it. Hot loops that outgrow TS get engine-side native primitives
  (systems pass types, math/procgen stdlib) — the script tier itself stays TS.
- Agent-facing code shapes (all TS): **state scripts** (per-state brains with lifecycle hooks),
  **script-components** (data + hooks, implementing EventListener), and **systems** (ordered
  global logic over ECS queries). Event wiring is preferably data in entity files; code drops to
  `subscribe`/`trigger` when logic demands it.
- **Components are code-first**: a component is a TS class with decorated fields
  (`@component()` / `@field({min, max, event: true, …})`); the engine extracts the schema at load,
  so YAML validation, entity-file autocomplete, and the schema manifest derive from the one
  artifact the agent wrote. Engine-native (C++) components arrive pre-typed via
  `engine_api.json` → `.d.ts`. Methods on components are allowed and encouraged
  (`health.damage(40)`).
- **Access is typed**: `entity.get(Health)` (typed, throws structured), `tryGet`/`has` for
  optional paths; systems use `world.query(A, B)` tuple iteration. Entity handles are
  generational refs with `.alive`; stale access is a caught, explained error (entity, despawn
  tick, access site) — never silent corruption.
- **Lifetime**: `world.spawn(prefab, {at, overrides})` instantiates prefab assets (the same
  instancing + override mechanism as scenes/machines) and returns an immediately usable handle;
  `world.despawn(ref)` marks for removal. Structural changes (spawn/despawn/reparent) queue and
  apply at defined points between tick phases — never mid-query — keeping iteration safe and
  replay stable. No code-assembled entities: everything traces to a validated prefab/scene file.
- **Error contract**: an exception in any hook halts dev/headless runs AT the offending tick with
  a structured JSON error (stack, entity, region/state, hook, event cause-chain, tick, replay
  bookmark + nearest snapshot — a ready-to-fix artifact). Shipped builds quarantine the offending
  component/state (deactivate + log) so games degrade instead of crashing. No error is swallowed.
- Hot reload; per-frame budget instrumentation; script errors are structured JSON with stack +
  tick + scene context.
- Gradual-safety model (from GDScript's lesson): code always runs; the analyzer (tsc + engine
  lints) makes it stricter monotonically. Engine-specific lint pack: no wall-clock in sim, no
  unseeded RNG, no frame-rate-dependent logic.

## 8. Text formats (agent-native by construction)

- **Scene format**: strict-YAML, tscn-inspired semantics (typed literals; sections for entities,
  resources, event definitions). An entity definition declares: base components, state machine
  (regions → states → substates/sequences, each with components + optional state script), event
  field links, and reactions — the complete behavioral wiring readable in one file. Deviations
  from Godot, by design:
  - **No opaque IDs without a human-readable companion.** Asset references dual-write
    `{uid, path}` — the engine-assigned uid is authoritative (rename-safe, survives moves), the
    path keeps files self-explanatory and greppable. `midday check --fix` repairs drift in either
    direction; `midday mv` rewrites paths (uids never change); uids live in `.uid` sidecars + a
    regenerable cache and are never hand-minted. Intra-file references (nodes, states, features)
    use names/paths only.
  - **Full-state visibility**: files may elide defaults, but `midday scene print --full` and the
    schema manifest expose complete effective state; validators know every default.
  - **No editor-bookkeeping fields** in agent files; anything regenerable lives in a cache dir.
- **Model format**: parametric + CSG op-list (see §10), same YAML dialect.
- **Validate-before-write** (pillar): `midday validate <file>` checks types, required props,
  allowed children, connection signatures, ID references — against the schema manifest generated
  from `engine_api.json`. Warnings-after-the-fact are a fallback, not the contract.
- **Asset import workflow**: content-hashed cache (regenerable, gitignored; sources + sidecars
  committed). **Auto-import on demand** — every consuming verb (run/test/shot/export) hash-checks
  referenced assets and imports dirty ones before proceeding; `midday import [--all]` pre-warms
  for CI. Settings are **convention + override-only sidecars**: project-level policy file
  (`midday.import.yaml`, glob-scoped defaults); a per-asset sidecar exists only where an asset
  deviates. glTF 2.0 is the mesh/anim interchange. CSG models are assets like any other — scenes
  reference the `.model.yaml` source; the engine resolves to the compiled mesh transparently.
- **Asset hot-reload in dev runs**: changed assets re-import and swap live in running sessions
  (journaled with the swap tick); replay mode never hot-reloads.

## 9. CLI (the entire interface)

Every verb: `--json` structured output, meaningful exit codes, headless-first.

```
midday new|run|build|import|export      project & asset lifecycle
midday validate|check|fmt               schema + TS + format gates
midday test [--golden]                  testkit runner (see §11)
midday shot|record                      one-shot frame → PNG · deterministic capture
midday model build|view|slice|query     modeling module (see §10)
midday replay run|seek|query|explain    recorded-run interrogation (see §12)
midday api dump|diff|docs               engine_api.json · drift detection · agent docs
midday inspect|eval                     live-process bridge (see §12)
```

The CLI is the **engine's** canonical interface — every capability exists here first, headless,
JSON-out. The Midday Editor (§13a) is a *client* of this surface plus the live bridge; nothing is
editor-only.

## 10. Modeling module (the fifth pillar — no industry precedent)

**Kernel**: OpenCASCADE (OCCT, LGPL — dynamically linked) — full B-rep from day one: exact curved
surfaces, true fillets/chamfers, STEP interop. Meshes are tessellated from the B-rep at build
time with per-model quality settings. **OCCT is toolchain-only**: models compile to meshes at
build; exported games never link OCCT — which keeps the LGPL boundary off iOS (where static
linking is the norm) and off every runtime. Export packaging includes a license scan asserting
this.

**Authoring**: declarative parametric op-lists, the source of truth (`midday model build` compiles
to mesh). V1 op set: primitives (box, sphere, cylinder, capsule, cone, torus, wedge) · booleans
(union, cut, intersect) · 2D sketches (polyline/arc/circle profiles on any plane) with extrude,
revolve, sweep-along-path, loft · **fillets & chamfers** (B-rep native) · patterns (linear,
radial, mirror) · transforms, hull, offset/shell · **anchors** (named oriented mount points —
`hand`, `muzzle`, `hinge` — exported as attachment sockets usable in scenes/prefabs).

**Addressing**: every op has an `id`; sub-geometry via semantic selectors scoped to ids —
`turret.faces.top`, `hull.edges(angle>60°, z>1)`, `handle.faces(normal~-y).center`. No bare
indices (regeneration-stable); a selector either resolves or fails with a structured error
listing nearest candidates.

**Parameters**: a `params` block (values with ranges/docs); any numeric field accepts arithmetic
expressions over params and prior-op measurements (`hull.bbox.max.y + turretR/2`). Procedural
generation is TS emitting `.model.yaml` via a builder API (`midday model gen`) — the YAML stays
the canonical, diffable, validated artifact.

**Perception (visual-primary, per consultation).** Default annotations on all renders (each also
individually flaggable): coordinate grid (auto-snapped spacing) + axis labels/orientation gizmo +
bounding-box dimension callouts · per-op labels with leader lines, color-keyed consistently
across all views · anchor markers (oriented, labeled) + per-op key dimensions on the most legible
view · slice contour vertex coordinates as a margin table (exact numbers beside the picture).
- `midday model view` — orthographic top/front/side/iso renders, engineering-drawing style.
- `midday model slice --plane <axis>=<v>` — exact B-rep cross-section at any cutting plane (the
  "CT scan" channel for interiors and wall thickness).
- `midday model turntable` — N shaded perspective orbits for overall shape assessment.

**Verification (lives in the testing pillar, not perception)**: structured geometry queries as
JSON — bounds, volume, center of mass, manifold checks, intersection tests, feature-to-feature
distances, raycast probes — so tests assert on numbers without vision calls.

Materials/UV: box/planar auto-UV + PBR material assignment per feature in v1; hand-tuned UV
unwrapping is post-v1.

## 11. Game-testing layer (pillar)

- Scripted input playback (synthetic event injection on the deterministic tick).
- Tick-stepped test control: run N ticks, pause, assert, continue — from TS test files.
- Scene-state assertions: query any node/property/signal at any tick; snapshot diffing.
- Golden-frame assertions: `expectFrame(camera, reference, tolerance)` in tests.
- Geometry queries (§10) for model regression tests.
- Headless scene playback at faster-than-realtime where the sim allows.
- One runner: `midday test` — TAP/JSON output, CI-native. "Drive player to the door, assert the
  level loads" is the canonical smoke test an agent writes for every game.

## 12. Replay, introspection & query/explain (pillar)

- **Journal format (`run.mrj`, a directory bundle)**: `header.json` (build hash, content hashes of
  all scripts/assets, seed, record tier — replay refuses/warns on mismatch: a replay against
  drifted code is a lie) · `journal.jsonl.zst` (append-only causality stream: one record per
  input/event/transition/spawn, each carrying tick, key, payload, subscribers, and a **parent
  cause-id** — chains reconstruct mechanically) · `snapshots/t<N>.bin` (binary world-state
  sidecars) · `index.json` (tick → offset seeking). Text journal stays greppable; bulk stays
  binary.
- **Recording tiers**: **FLIGHT** (always on, shipped builds as a rolling window → every crash
  report is a reproducible run: inputs + seeds + event/transition journal — enough to re-simulate
  everything) → **SNAPSHOT** (dev default: + world snapshots every 300 ticks for fast seek) →
  **TRACE** (opt-in: + per-tick component deltas for re-sim-free property queries).
- **Replay**: `midday replay run` reproduces bit-identically; `--to-tick N` seeks via nearest
  snapshot + re-sim.
- **Query surface (all v1)**: filtered event/transition streams (by name/key/entity/tick-range);
  state timelines + full at-tick entity inspection; `midday replay explain <tick> <entity|event>`
  — walk the cause-id chain backward (input → event → reaction → transition) and render the tree;
  property-over-time curves (TRACE or windowed re-sim); and **run-diff** — compare two runs to the
  first divergent tick (the determinism-debugging tool).
- **Live bridge** (Godot's remote-debugger lesson, kept and formalized): a documented JSON
  protocol over one socket — scene-tree dump, property inspect/mutate, expression eval, spawn/
  free nodes, frame-step, time-scale, screenshot. The perceive→act→verify loop for a running game.
- Profiling: per-function script timing, engine monitors, frame metadata (draw calls, culled
  counts, pass timings) — all queryable as JSON.
- **Viewport vision (the agent's eyes — all four channels v1)**: interactive screenshots at any
  camera or an agent-positioned flycam, deterministic capture mode · **entity-ID pick buffer** —
  "what entity/state is at pixel (x,y)" + full segmentation maps (entity = color + legend), so any
  screenshot becomes a labeled scene · composable **debug-draw overlays** burned into captures
  (collider wireframes, navmesh, raycasts, event flashes, state labels above entities, camera
  frustums, light bounds) · **G-buffer taps** (depth/normal/motion as EXR) for geometric
  verification. All exposed via `midday shot` flags and the live bridge; the pipeline-as-data
  render graph names these targets, so channels are taps, not forks.

## 13. Gameplay systems: audio, animation, UI, cameras, saves, procgen (v1 scope)

- **Audio**: bus DAG with per-bus effect chains (serializable layout); WAV/Ogg (+ QOA); positional
  3D (attenuation models, doppler); dedicated audio thread; **configurable buffer size from day
  one** (Godot's hardcoded-latency wart, fixed); dummy driver clocked by ticks for deterministic
  capture. **Mixer snapshots**: named bus-state presets (volumes, effect params) blendable at
  runtime (combat ↔ explore ducking) — declared as data, switched via events.
- **Input rebinding is data**: action maps live in project config; runtime rebinding writes a
  user-profile overlay; conflict detection in the validator. (The rebinding *UI* is a game's
  RmlUi concern; the engine owns the data path.)
- **Animation**: Animation-as-pure-data resource (value/transform/method/audio tracks, keyframes
  as text); player with queue/sections/capture-blend; **Tween as the primary agent-facing
  game-feel API**; blend trees + state machines as data (M2 of animation); skeletal + root motion
  for humanoid showcase needs; glTF animation import. **M2 animation state machines must answer
  Mecanim's transition semantics explicitly** (verified against Unity 6 docs): interruption
  scopes (none / by-source / by-destination / ordered combinations) with priority-ordered rules;
  exit-time triggers on normalized state time (<1.0 = every loop, >1.0 = one-shot after N loops);
  blend durations in seconds OR normalized source time; destination phase offsets. All as data,
  all journaled.
- **UI is HTML/CSS via RmlUi (MIT)** — the game-oriented HTML/CSS engine: HTML-like markup + real
  CSS (selectors, flexbox, transitions, animations), rendered through OUR pipeline (HUD, menus,
  render-to-texture in-world screens, custom shaders on panels), data bindings to component
  fields. Agents write `.rml`/`.css`, validated like every other format; deterministic layout.
  MSDF fonts, i18n catalogs, and accessibility semantic roles (AccessKit — MIT/Apache) integrate
  at our layer.
- **Camera direction system** (Cinemachine-class, as data + TS): virtual cameras as components —
  follow/orbit/rail rigs with damping, composition rules, look-at targets — priority-based
  blending, shot cuts driven by bus events, camera shake. The camera has a brain, not just a
  transform.
- **Scene-level director sequences**: the sequence machinery (§4.1) also runs scene-owned, with
  tracks referencing MANY entities — activation spans, targeted event triggers, camera track,
  audio track — Timeline-class cutscenes on the same dope-sheet primitive, same determinism.
- **Save/load**: player-facing save games as curated world-state serialization — opt-in per
  component via `@field({save: true})` — save slots, versioned migration hooks. Implementation is
  a filtered snapshot + manifest (the replay pillar pays for this twice over).
- **Quality tiers**: named quality profiles in a per-platform matrix, selecting pipeline-as-data
  variants + setting bundles. **Async & additive scene loading** with progress events; streaming
  beyond that is roadmap.
- **Splines are a v1 scene primitive**: a Spline component (Bézier/Catmull-Rom, control points as
  YAML) that camera rail rigs ride, entities follow (patrol paths, moving platforms), procgen
  scatters along, and sequences animate position-on-spline — one primitive, four consumers.
- **Runtime rigging/IK lands at animation M2**: constraint components on skeletons — look-at,
  two-bone limb IK, FABRIK chains, foot placement — as data + hooks, alongside blend trees and
  state machines. M1 ships root motion + authored clips.
- **Procgen stdlib** (deterministic, seeded from engine RNG streams — replay always holds): noise
  fields (FastNoiseLite — MIT), sampling & scatter (Poisson disk, jittered grids), Wave Function
  Collapse, graph/dungeon/BSP/maze generators, spline-driven placement, weighted tables. Runs at
  build time (baked scenes) or runtime (journaled seeds); feeds modeling params and prefab
  spawning.

## 13a. The Midday Editor (infinite canvas + the engine's face)

**The editor is an infinite-canvas workspace, not a windowed IDE.** One pannable, zoomable plane;
everything lives on it as **subcanvases** — no docked windows. A new project opens to an empty
canvas and **Midday**, the engine's mascot and embodied agent: you tell Midday what to build, and
it builds — opening viewports, editing the project, arranging the workspace. The editor is also a
complete, fully-functional tool with **no AI configured at all**.

- **Built with the engine itself** (the editor is a Midday app): the canvas is a 2D world
  (pan/zoom = camera), subcanvases are render-to-texture viewports (native engine targets, zero
  streaming), chrome and inspectors are RmlUi, edits flow through the same validation/bridge API.
  Building the editor IS testing the engine. Workspace layout persists as `workspace.yaml`.
- **Subcanvas types (v1 set)**: live scene viewports (editor camera + gizmos) and running-game
  viewports · model sheets (ortho/slice/turntable cards, live-updating) and RmlUi previews at
  device resolutions · statechart diagrams (active states highlighted live), event-wiring graphs,
  render-pipeline graphs, scrubbabale replay/sequence timelines · **agent workspace cards** —
  Midday's artifacts (shots, slices, diffs, test reports) as cards the human annotates and
  arranges; annotations flow back to Midday.
- **Live document model, text persistence**: the editor owns a full in-memory document model —
  selection, transactions (a drag is one transaction), unlimited undo/redo — but persistence is
  the canonical YAML/TS files: saving serializes with stable ordering; external file edits
  (agents, git) hot-merge into the open document. The editor holds no private truth; a GUI
  session's `git diff` is readable YAML.
- **One tool API for humans and Midday**: every editor capability (open/arrange subcanvases,
  create entities, edit properties, wire events, run/test/shot) is a command on the document
  model + shell, exposed simultaneously as human chrome (menus, ⌘K palette, shortcuts) and as
  Midday's generated tool schema (`editor_api.json`, same discipline as `engine_api.json`).
  Midday's actions enter the **same undo stack and journal** as human actions — one ⌘Z undoes
  either. Human/AI parity holds by construction.
- **Midday's perception**: the viewports on the canvas are what Midday sees — the §12 vision
  channels (screenshots, entity-ID buffer, overlays, G-buffer taps) wired to the agent loop.
- **Midday's brain is bring-your-own-endpoint**: the agent harness (persona, tools, vision
  wiring, journaling) is OSS engine code; the model is user-configured — Anthropic/OpenAI-
  compatible APIs or local models (Ollama/llama.cpp) through one interface. No key configured →
  Midday idles; the editor is a complete manual tool.
- **Timing**: the engine and showcase stay CLI/headless through M1; the editor program starts at
  M2 and **gates v1-done** alongside renderer parity and iOS export.
- **M2 entry gate — hot-merge design doc**: "external edits hot-merge into the open document" is
  a concurrent-editing problem, not a feature bullet. Before any editor code: a short design doc
  deciding the merge policy (external diff → synthetic transactions replayed into the doc model),
  conflict semantics (per-section file-wins vs doc-wins), and undo-stack behavior across merges.
  No doc, no editor milestone.

## 14. Platforms, distribution, build

- **Five platforms are the contract**: macOS, Windows, Linux, Android, iOS. Everything (RHI,
  input, filesystem, export pipeline) is architected for all five from the first commit — no
  desktop-only assumptions. Delivery is milestone-gated: desktop ships with renderer M1; Android
  export (native Vulkan) at M2; iOS export (Metal path) gates v1-done at M3. Headless mode on all
  desktop platforms (CI = Linux). Post-v1: WASM/WebGPU investigation.
- `midday export --platform <p> --profile <release|debug>` → signed artifact + JSON report.
- Build: CMake + Ninja; tiered so `core` builds are minutes-cold but agents never need one;
  `compile_commands.json` always on; sanitizer + deterministic-replay CI lanes.
- Export: pack format (content-addressed, per-file hashes, patch-capable — PCK lessons) + platform
  binaries.
- Testing the engine itself: unit tests compiled into the binary (doctest pattern), golden-frame
  suite, determinism suite (replay N ticks twice, byte-compare), API-drift gate (`midday api diff`).
- **CI strategy (pinned, so the pillars never degrade to "runs on the dev Mac")**: one **pinned
  Linux GPU runner class** is the golden-frame reference (hash-equal there; tolerance elsewhere);
  a **determinism lane** (dual-run byte-compare of sim journals) runs on every push from the
  first runnable slice; desktop build lanes from day one; Android/iOS lanes join at their
  milestones (M2/M3); macOS CI optional until Metal M2. The managed-firewall dev Mac is never a
  CI dependency.

## 15. Constraints (hard)

1. **Open source everything** — engine license MIT (proposed; confirm), all dependencies
   OSS-licensed (MIT/BSD/Apache/zlib preferred; LGPL acceptable if dynamically linked; no
   proprietary SDKs). A `LICENSES/` manifest tracks every dependency.
2. **No GUI editor.** The CLI + formats + agent are the editor.
3. **Headless is the default**, windowed is the special case.
4. **Every output machine-readable** (JSON) before it is human-readable.
5. **Determinism is contractual** — a CI-enforced invariant, not a best effort.
6. Dev environment reality: macOS with managed firewall (no incoming connections/LAN broadcast) —
   local sockets/pipes for the live bridge; nothing requires listening on external interfaces.

## 16. Non-goals (v1) & post-v1 roadmap

**Non-goals (v1):**
- Runtime AI features (LLM NPCs, generated content at runtime) — the parked second pillar.
- Multiplayer/networking beyond the local live-bridge socket.
- Console/web export. VR/XR. Custom shading DSL (graphs-as-data are in; a text DSL is not).
- ~~Human-facing GUI editor~~ — **superseded**: the Midday Editor (§13a) is in, starting M2. The
  surviving principle: the editor is never the source of truth and nothing is editor-only — text
  formats + CLI remain canonical, the editor is a client.
- Cloud/live-ops services (ads, analytics, remote build) — Unity treats these as engine surface;
  we explicitly do not.

**Post-v1 roadmap (validated against the Unity 6 research pass):** terrain system (sculpt-as-data
+ procgen-driven) · 2D/sprite toolchain (tilemaps, sprite animation) · VFX-graph-as-data · video
playback · GPU-driven rendering (resident-drawer-style batching, GPU occlusion culling) ·
APV-class GI upgrades (probe streaming for open worlds, lighting-scenario blending, runtime sky
occlusion) · hardware ray tracing (post-parity) · world streaming beyond additive loading ·
WASM/WebGPU export · versioned engine-module/package tier (Unity's "Released packages" delivery
model) · 2D physics (with the 2D toolchain) · Addressables-class remote content delivery ·
cloth simulation.

**iOS constraint (noted for M3)**: the App Store bans JIT — QuickJS (interpreter) ships as-is;
a V8 swap must use JIT-less mode on iOS.

## 17. Risks (acknowledged in consultation)

| Risk | Mitigation |
|---|---|
| Renderer parity (M3) is years of work | Showcase gates on M1; parity is the v1 *done* bar, phased; roadmap explicit |
| C++ core slows engine iteration | Layering contract: agents live entirely in the TS/format tier; core API stabilized early via api.json |
| OCCT kernel weight (heavy API, meshing quality tuning, LGPL care) | Wrap behind the op vocabulary — agents never see OCCT; tessellation-quality golden tests; dynamic linking + LICENSES manifest |
| Determinism across platforms (FP drift) | Contract is per-build determinism first; cross-platform determinism a stretch goal; pinned flags + CI lanes |
| Solo maintainer + huge surface | Agents build the engine too; reference spec + this spec keep every subsystem's "done" definition explicit |
| Editor is a second product (canvas workspace + doc model + agent harness) | It's a Midday app (dogfoods the engine, one stack); starts only after the showcase proves the core; editor_api.json keeps its surface as disciplined as the engine's |

## 18. Open items (to confirm at kickoff)

*(Appendix A below is normative: it fixes the tick order and statechart execution semantics that
§4.1/§4.2 rules imply, and its worked trace ships as a golden engine test.)*

- Engine license: MIT (recommended) vs Apache-2.0 (patent grant) — pick before first public commit.
- Engine name: "Midday Engine" (from the project dir) — working title until confirmed.
- QuickJS vs V8 as the *first* embedded runtime (spec says QuickJS; cheap to revisit at M1).
- Showcase game concept — deliberately chosen AFTER the M1 toolchain exists, via a one-paragraph
  prompt, because producing it from a prompt IS the success test.

---

## Appendix A — Canonical tick order & statechart execution semantics (normative)

This appendix fixes the execution semantics §4.1/§4.2 imply, so they are spec, not C++
implementation accidents. The worked trace (A.3) ships as a golden engine test.

### A.1 The fixed tick, phase by phase

Every fixed tick N runs these phases in order. All iteration orders are deterministic and
defined. Events dispatch **immediately** wherever they trigger (any phase); the phase list
defines when the ENGINE initiates triggers.

1. **tick-begin** — journal tick marker; clear every region's transitioned-this-tick mark.
2. **input** — OS/synthetic input updates input state; input action events trigger on the bus
   (reactions and transitions may run inline here, like anywhere).
3. **watchers** — condition watchers (`when:` transitions) evaluate in deterministic order
   (entity tree order, then declaration order); edge-triggered: each newly-true watcher fires
   its internal event once.
4. **sequences** — active sequences advance playheads to tick N (entity tree order, region
   declaration order). Per sequence, due timeline items fire in timeline order: trigger-track
   keyframes (bus events), span openings (activate children — enter hooks), span closings
   (deactivate — exit hooks). A playhead reaching the end fires `<state>.finished` (or loops /
   holds per the declared end mode).
5. **update** — pre-update systems (declared order), then `onFixedUpdate` hooks: entities in
   scene-tree depth-first order; per entity, regions in declaration order; per active state,
   **state script first, then components in attach order**.
6. **physics** — Jolt steps (pinned deterministic config in replay mode). Contact/trigger events
   collected during the step trigger AFTER it, sorted by body-pair id (deterministic).
7. **post** — post-update systems (declared order).
8. **structural apply** — spawns/despawns/reparents queued during the tick apply in queue order.
   Spawned entities go live: initial states enter (full enter chains). Despawned: full exit
   chains, then handles go `.alive == false`.
9. **tick-end** — journal flush; snapshot if on cadence; render-interpolation state captured.

### A.2 Transition algorithm (runs inline, any time an event triggers)

The bus iterates subscribers of `(event, key)` in registration order. When a subscriber is a
Transition component (or a region's any-state list):

1. **Eligibility**: the owning region must NOT have transitioned this tick. Candidate pairs =
   region any-state rules + the active state's pairs matching the event, whose `if` filters pass.
2. **Winner**: highest `priority`; tie → declaration order, with the any-state list treated as
   declared before the active state's pairs.
3. **Execute inline** — exit chain, then enter chain (A.2.1); journal
   `(tick, entity, region, from→to, cause: event-id)`; mark the region.
4. **Voiding**: losing candidates now, and ALL matching pairs on later events this tick (in this
   region), are journaled as `voided` with their cause. They do not queue; next tick is a fresh
   evaluation.
5. **Cascades**: hooks run by the transition may trigger further events — dispatched immediately,
   nested (depth cap 32 → structured error E-series). These may transition OTHER regions
   (if unmarked) but never the own region again this tick (rule 1). This is the cycle-breaker:
   `onEnter` emissions matching the NEW state's own pairs are voided this tick.

#### A.2.1 Enter/exit hook order (per state S)

**Exit** (brain first — it orchestrates while its parts are still live):
1. S's state script `onExit(to)`
2. S's open sequence spans close / active substates exit — deepest first, reverse activation order
3. S's components `onExit` — reverse attach order
4. S's node subtree (attached child entities) deactivates
5. Sequence playhead resets (or saves, under `history: true`)

**Enter** (mirror — brain last, when its parts are live):
1. S's node subtree activates
2. S's components `onEnter` — attach order
3. S's initial substate enters / sequence playhead starts at 0 (or saved position under history)
4. S's state script `onEnter(from)`

Parallel regions transition independently; a multi-region entity may transition several regions
in one tick (one transition each).

### A.3 Worked trace — the golden test case

**Setup.** Entity `Boss`, base `[Transform, Collider, RigidBody, Health]`, two regions:
- `locomotion`: `Idle` (initial) → pair `{player.spotted → Chasing}`; currently **Chasing**.
- `combat`: `Passive` (initial), `SlashAttack` (sequence, 1.2 s = 72 ticks: trigger
  `attack.swoosh` @0.3 s; span `HitboxLive` [0.4–0.8 s] with child entity `Hurtbox` under it;
  end → `self.finished`), `Staggered`, `Dead`.
  Pairs: Passive `{player.inRange → SlashAttack}`; SlashAttack `{self.finished → Passive}`,
  `{stagger.hit → Staggered, priority 10}`. Region any-state:
  `{death.dealt → Dead, priority 100}`.
  Currently **SlashAttack**, playhead 0.6 s — span open, `Hurtbox` live.

**Tick 3200.** The player's counter-attack landed at tick 3199 (physics contact). The damage
system (update phase) processes the hit:

```
phase 5 (update), damage system:
  boss.get(Health).damage(40)            → Health.value hits 0
  Health emits death.dealt @ key boss    → IMMEDIATE dispatch:
    combat any-state pair {death.dealt → Dead, prio 100} — region unmarked → WINS
      exit SlashAttack:
        1. slash_attack.ts onExit(Dead)
        2. open span HitboxLive closes  → Hurtbox subtree exits/deactivates
        3. SlashAttack components onExit (reverse attach)
        4. playhead reset (no history)
      enter Dead:
        1-2. (no subtree/components)     3. (no substates)
        4. dead.ts onEnter(SlashAttack)  → emits boss.died @ global (depth-2 cascade;
                                           UI/score listeners react; no transition)
      journal: t3200 Boss combat SlashAttack→Dead (anystate, cause: death.dealt#ev88123)
      combat region MARKED
  damage system continues:
  emits stagger.hit @ key boss           → IMMEDIATE dispatch:
    SlashAttack pair {stagger.hit → Staggered} — combat already transitioned this tick
      → journal: t3200 VOIDED stagger.hit→Staggered (region combat already transitioned)
    locomotion: no matching pair          → no-op
```

**Assertions the golden test makes:** exactly one `combat` transition at t3200; `Hurtbox` is
inactive before `Dead.onEnter` runs (span closed inside the exit chain — no zombie hitbox);
`stagger.hit` journaled as voided with cause; `locomotion` remains `Chasing` (parallel region
untouched); cause chain reads `contact → damage → death.dealt → transition → boss.died` end to
end; replay of the tick is bit-identical.

# Godot Reference Spec — Base for the AI-First Engine

Extracted from Godot Engine v4.8-dev source (`godot/`, shallow clone, analyzed 2026-07-02 by 8 parallel
subsystem analyses). This is the "what a full game engine comprises" baseline. Section 12 maps every
subsystem to an AI-first verdict (keep / change / drop / missing) — that's the part we iterate on.

---

## 1. Core architecture

**Object model**
- Root `Object` (manual delete) → `RefCounted` → `Resource` (path-addressable, shareable asset).
- `ClassDB`: full runtime reflection registry — classes, methods (typed args + defaults), properties
  (49 hint types, usage bitmask), signals, enums/constants, inheritance, API-type partition
  (core/editor/extension) with per-API compatibility hashes.
- Fast RTTI without dynamic_cast (single inheritance + ancestry bitfield); `ObjectDB` slot table with
  39-bit validators = safe stale-ID detection, O(1) lookup, ~16.7M live objects max.
- Signals are first-class Variant values (`Signal`, `Callable`); connect flags: deferred, persist
  (serialized into scene), one-shot, reference-counted. `MessageQueue` for deferred calls, flushed
  per tick.

**Memory & containers**
- Copy-on-write core (`CowData`): Vector, String, all Packed*Array types — cheap Variant copies.
- Interned `StringName` (pointer-comparison keys used everywhere), `NodePath`.
- Container zoo: CoW Vector vs LocalVector, HashMap/AHashMap, RB trees, PagedArray, RID pools,
  lock-free lists, command queues.

**Serialization & IO**
- `ResourceLoader/Saver` with pluggable format chain, threaded loading with progress, 5 cache modes.
- `FileAccess` factory + decorators (compressed, encrypted, memory, pack, zip, patched); ConfigFile
  (INI), JSON with Variant↔JSON conversion, binary Variant marshalling with object-injection gates.

**Math**
- `real_t` float/double build switch. Vector2/3/4(i), Basis, Transform2D/3D, Projection, Quaternion,
  Rect2/AABB/Plane, Geometry2D/3D helpers, Delaunay, convex hulls, BVH/DynamicBVH, AStar2D/3D/Grid,
  PCG RNG, runtime `Expression` evaluator.

**OS abstraction**
- Abstract `OS` singleton per platform: process exec with pipes, env, paths, dynamic libraries
  (substrate for extensions), monotonic time, entropy, memory/CPU introspection, stdio with
  pipe-detection.
- Threading: Thread/Mutex/RWLock/Semaphore/SpinLock + `WorkerThreadPool` job system (tasks + data-
  parallel group tasks, sized to CPU count).

**Main loop**
- Phased startup: core types → project settings → servers → scene → editor; extensions/modules
  init at 4 matching levels (CORE/SERVERS/SCENE/EDITOR), torn down in reverse.
- Frame: fixed physics ticks (default 60 Hz, max 8 catch-up steps, jitter-fix 0.5, 12-frame step
  smoothing) + variable-delta process + render sync/draw + audio + debugger + movie writer.
- `--fixed-fps` forces deterministic stepping; `--quit-after N` deterministic exit; `--headless`
  swaps null DisplayServer + dummy audio.

**Input**
- `InputEvent` Resource hierarchy (key/mouse/joy/touch/gesture/action/MIDI), serializable.
- `InputMap`: named actions → event lists, deadzones, per-device. Polling API (`get_vector` etc).
- **Synthetic injection**: `parse_input_event`, `action_press/release` — automation-ready.

## 2. Scene system

- Everything runnable is a `Node` in a single-rooted `SceneTree`. Lifecycle notifications:
  enter_tree (top-down) → ready (bottom-up, once) → process/physics_process (opt-in) → exit_tree.
- Parent vs **owner** distinction: parent = tree position, owner = which scene file a node is saved
  into (keeps instanced sub-scenes collapsed).
- Groups (broadcast call/notify, persistent groups serialized); declarative pause via ProcessMode
  (inherit/pausable/when_paused/always/disabled); process priorities; opt-in threaded subtree
  processing.
- **PackedScene**: scene = flat table (nodes, properties as diffs-over-defaults, connections,
  groups). Instancing = reference to another scene + property overrides. Scene inheritance = base +
  diff. Placeholders for deferred loading.
- **.tscn text format**: line-oriented INI-like, typed literals (`Vector2(0, 50)`,
  `ExtResource("id")`), explicitly VCS/diff-optimized. Sections: gd_scene header, ext_resource,
  sub_resource, node, connection, editable. Only non-default properties stored.
- Node taxonomy: ~28 2D nodes + physics, ~55 3D nodes + physics, 59 Control/GUI nodes, special
  nodes (Viewport, Window, CanvasLayer, Timer, HTTPRequest, MissingNode for forward-compat).
- Resource taxonomy: ~94 resource types (textures, materials, meshes, shapes, curves, themes,
  fonts, animations, tilesets...).
- Viewport model: viewport = render target + world(s); SubViewport for render-to-texture;
  CanvasLayer decouples HUD from camera.
- Validation contract: per-node `get_configuration_warnings()` (after-the-fact, editor-surfaced).

## 3. Rendering

- **Server + RID pattern**: nodes own no GPU state, only opaque RID handles into `RenderingServer`.
  Command-queue thread separation (optional dedicated render thread).
- Backend tiers: `RenderingDevice` (Vulkan-flavored abstraction) → drivers (Vulkan/D3D12/Metal);
  legacy GLES3 path bypasses RD entirely; dummy rasterizer for headless. Render-graph layer
  auto-resolves barriers.
- Renderer split: forward_plus (desktop) / mobile (tiled) / gl_compatibility (web+old HW) / dummy.
- 2D: retained canvas-item tree, aggressive instance batching (batch breaks on texture/shader/blend
  change), 2D lights + shadow occluders, 2D SDF generation, separate 2D MSAA, HDR-2D.
- 3D inventory: 3 light types + clustered culling; 3 GI systems (SDFGI realtime, VoxelGI baked
  volume, lightmaps + probes); shadows/CSM; SSR/SSAO/SSIL; DoF, auto-exposure, volumetric fog,
  decals; tonemappers incl. ACES/AgX; MSAA/TAA/FXAA/SMAA; mesh LOD; software occlusion culling
  (Embree); FSR1/FSR2/MetalFX upscaling; VRS.
- Shader system: custom GLSL-like DSL (largest file in the engine: 406k-line parser) → transpile →
  SPIR-V → per-backend (DXIL/MSL); ShaderMaterial (custom) vs StandardMaterial3D (parameter-driven,
  codegen under the hood); visual shader graphs.
- **Headless truth**: `--headless` ⇒ dummy rasterizer ⇒ **zero pixels**. Screenshots need a real
  display driver (Xvfb pattern on CI). MovieWriter (deterministic frame+audio capture, fixed
  timestep, frame-locked audio) exists but also needs real GPU.
- Windowless compute exists (`create_local_device`), but no windowless scene rendering.

## 4. Physics & navigation

- Same server+RID pattern. 2D and 3D fully separate stacks. Backend factory with priority
  (GodotPhysics2D; GodotPhysics3D default, Jolt opt-in via setting; GDExtension swap-in supported).
- Body taxonomy: static / kinematic (animatable) / rigid / rigid-linear; Area (trigger + gravity/
  damp/wind override zones with priority stacking); CharacterBody with `move_and_slide` (floor
  snap, max angle 45°, one-way platforms, moving platform velocity inheritance).
- Shapes: ~9 per dimension; queries: raycast, point, shape overlap, swept cast (`cast_motion`),
  rest info, `body_test_motion`; 32-bit layer/mask model; CCD modes.
- Joints: 3 types in 2D, 5 in 3D. Soft bodies + raycast vehicles (3D only).
- Island-based sequential-impulse solver, 16 iterations, sleeping. Fixed 60 Hz tick,
  render-side interpolation optional.
- **Determinism: NOT guaranteed** — threaded solver ordering, float, island order. Incidental
  same-machine repeatability only. Jolt is deterministic per-binary but Godot doesn't expose it.
  Clean snapshot point exists between `end_sync()` and `step()`.
- Navigation: maps → regions → navmesh polygons + off-mesh links; A* over polygon graph; RVO2/ORCA
  local avoidance (threaded); runtime navmesh baking; separate 2D and 3D implementations.
- TileMap physics: tiles batched into merged static-body quadrants keyed by layer + one-way flags.

## 5. Scripting

- **Language-agnostic contract**: `Script` (code asset) / `ScriptInstance` (per-object binding) /
  `ScriptLanguage` (driver) + global `ScriptServer` (16 slots). Engine dispatches through `callp`
  only — never knows the language. Placeholder instances keep objects alive with broken scripts.
  Whole contract mirrored through GDExtension so languages can be plugins.
- **GDScript**: gradual typing (untyped always runs; hard type annotations flip the compiler into
  typed/validated opcodes — correctness and perf improve monotonically). Register-based bytecode VM,
  ~150 opcodes with type-specialized tiers. Signals/await/coroutines native in VM. ~25 annotations
  (@export family, @tool, @onready, @rpc). Disassembler included.
- Tooling: tokenizer → parser → analyzer → compiler pipeline, reused by editor + LSP. **40+ warning
  categories** with per-project levels, safe-line info. Real LSP (completion, hover, definition,
  rename, references, signature help) aware of scene tree. `##` doc comments feed the unified doc
  system.
- C#/Mono: same three-interface contract; real .NET assemblies, GC↔refcount bridging, MSBuild
  cycle. Proves the boundary is language-neutral.
- **GDExtension**: stable C ABI as data (179-entry function-pointer table spec'd in JSON);
  `extension_api.json` = the ENTIRE engine API as machine-readable JSON (classes, methods with typed
  args/defaults/hashes, signals, enums, builtin memory layouts, optional inlined docs). Per-method
  compatibility hashes detect version drift. Hot reload supported.

## 6. Debugging & introspection

- `EngineDebugger`: message bus with capture namespaces (core/profiler/scene/servers/...), TCP
  transport, messages = (name, Array).
- Execution control: breakpoints, step/next/out/continue, stack dump, frame locals/members/globals,
  **arbitrary expression evaluation in a stack frame**, script hot-reload.
- Scene introspection over the wire: full live scene-tree dump, inspect any object's complete
  property set by ObjectID, **mutate live properties**, call methods on live nodes, spawn/reparent/
  delete nodes at runtime, serialize a live node back to .tscn.
- Game-view control: suspend, frame-step, time scale, screenshots, camera override, click-to-pick.
- Profiling: pluggable profilers — per-function script timing, engine monitors (fps, memory, draw
  calls), custom user monitors, network bandwidth; Tracy/Perfetto zone instrumentation.
- This protocol is effectively a complete perceive→act→verify loop over one socket.

## 7. Editor & asset pipeline

- Editor = superset build (TOOLS_ENABLED) of the same binary. Import/export live editor-side only.
- **Import pipeline**: `ResourceFormatImporter` intercepts loads; source assets (png/wav/gltf/ttf)
  converted once to engine-native binaries cached in `.godot/imported/`; `.import` INI sidecars
  hold importer name/version, output path, all options; `.import.md5` sidecars drive reimport
  (source hash, dest hash, format version bump). 15+ registered importers; pluggable
  (EditorImportPlugin, scene format importers, post-import chain).
- **Export pipeline**: preset model (`export_presets.cfg`, INI, secrets split out) → feature-tag
  resolution → dependency walk with filter modes → PCK v4 pack (magic GDPC, per-file MD5, optional
  AES, patch/delta packs, embed-in-binary) → merge with per-platform export template. Fully
  scriptable (`PCKPacker`), headless-capable (`--export-*`).
- **UID subsystem**: stable `uid://` identities decoupled from paths (rename-safe references),
  regenerable binary cache + `.uid` sidecars.
- **Doc system**: 813 class XML files; `--doctool` reflects live ClassDB and merges (code owns
  signatures, humans own prose); CI fails on drift.
- Existing CLI agent surface: `--import`, `--export-release/debug/pack/patch`, `--check-only`,
  `--script`, `--headless`, `--quit-after`, `--fixed-fps`, `--doctool`, `--dump-extension-api`,
  `--validate-extension-api`, `--lsp-port`, `--dap-port`, `--benchmark-file` (JSON).

## 8. Audio

- Bus DAG (not fixed mixer): N buses route to Master; per-bus volume/solo/mute/bypass + ordered
  effect chain; whole layout is a serializable resource.
- ~18 effects (reverb, EQ, compressor, limiter, delay, distortion, pitch shift, spectrum analyzer).
- **Resource(config) → Instance(runtime) factory split**: AudioStream → AudioStreamPlayback;
  AudioEffect → AudioEffectInstance. Serializable config, realtime-safe instances.
- Formats: WAV (incl. QOA compression), Ogg Vorbis, MP3 (streamed); AudioStreamGenerator
  (procedural); randomizer/polyphonic/interactive-music composite streams.
- Positional: 2D (distance + attenuation curve + panning) and 3D (4 attenuation models, cone,
  air-absorption LPF, doppler).
- Dedicated audio thread; ~11 ms fixed latency (buffer size hardcoded — known wart); dummy driver
  for headless, clocked by frames in movie mode (deterministic).

## 9. Animation

- `Animation` = pure-data Resource: tracks (value/position/rotation/scale/blend-shape/method/
  bezier/audio/animation) × keyframes × interpolation modes. Fully text-authorable.
- `AnimationPlayer` (timeline: play/queue/sections/markers, capture-blend into a clip) on top of
  `AnimationMixer` (blending engine with defined blend pipeline).
- `AnimationTree` node graph: Blend2/3, Add, OneShot, TimeScale, Transition, BlendSpace1D/2D
  (Delaunay), full StateMachine with transition modes + travel() pathfinding. All resources = data.
- `Tween`: transient code-driven interpolation, 12 transitions × 4 eases, chaining/parallel/loops.
  The most LLM-natural game-feel primitive.
- Root motion extraction for character locomotion; BoneMap retargeting.

## 10. GUI & text

- `Control`: anchors (0–1 fractions) + pixel offsets, 16 layout presets, size flags, min-size
  propagation; containers do auto-layout (Box/Grid/Flow/Margin/Center/Scroll/Split/Tab + graph
  editing). Focus system with explicit neighbors (gamepad-ready). Full RTL mirroring.
- Theming: typed key/value dictionary (color/constant/font/font_size/icon/stylebox) with
  inheritance + variations; StyleBoxFlat = fully procedural (no image assets needed).
- Text stack: TextServer abstraction — advanced impl (HarfBuzz + ICU + Graphite: full complex-script
  shaping, bidi) vs fallback impl (size-optimized). MSDF fonts (resolution-independent).
  RichTextLabel with BBCode + animated per-char effects. i18n: PO/CSV catalogs, plurals,
  pseudolocalization, auto-translate on Controls.
- **Accessibility**: real AccessKit integration — semantic roles (~50) + actions on every Control,
  OS screen-reader bridge. Notable recent investment.

## 11. Platform, networking, modules, build, testing

- 7 platforms (Windows/macOS/Linux+Wayland/Android/iOS/visionOS/Web-WASM). Headless = runtime mode,
  not a platform. Web: Emscripten, WebGL2 via GLES3, no threads by default, WebAudio.
- Networking: StreamPeer/PacketPeer abstractions, TCP/UDP/TLS(mbedTLS)/DTLS/HTTP client/WebSocket/
  WebRTC/ENet; high-level MultiplayerAPI (RPC annotations, spawners, per-property synchronizers) as
  a replaceable module.
- 57 compile-time modules (config.py + register_types, statically linked); 4-level init ordering;
  feature-strip flags (disable_3d etc). Runtime extensibility = GDExtension only.
- Build: SCons, editor vs template_debug vs template_release targets, single monolithic binary;
  cold build tens of minutes — game logic lives in scripts precisely so it never needs a rebuild.
- Testing: doctest compiled into binary behind `tests=yes`, run via `--test --headless`; ~172 test
  files covering engine primitives via DisplayServerMock; **no game-level behavioral testing** (no
  input playback, no state snapshots, no golden frames).

---

## 12. AI-first delta map (the iteration surface)

### Keep (proven patterns worth adopting)
| Pattern | Why |
|---|---|
| Text-first scene format (INI-like, typed literals, diff-over-defaults, VCS-optimized) | Already near-ideal for agent editing |
| Node/Resource split; owner-vs-parent; instancing-as-reference + property-diff overrides | Compact, composable scene model |
| Resource(config) → Instance(runtime) factory split (audio/animation/theming) | Serializable-as-text AND realtime-safe |
| Server + RID architecture | Decouples object model from backends; headless/dummy swapping |
| **API-as-data**: extension_api.json, doc XML merge, per-method compat hashes | Ready-made agent tool schema; version-drift detection |
| **Remote debugger protocol** (scene dump, live inspect/mutate, expression eval, frame step) | A complete agent perceive→act→verify loop, already designed |
| Import pipeline shape: .import sidecars + MD5 cache + content-addressed outputs | Clean, text-diffable, idempotent |
| UID indirection (rename-safe references) | Critical for text-first refactoring |
| Gradual typing (untyped runs → annotations add safety + speed monotonically) | Matches how agents iterate |
| 4-level init ordering; fixed-timestep loop + `--fixed-fps`; synthetic input injection | Determinism + automation hooks |
| Validation contracts (configuration warnings, 40+ analyzer warning categories, `--check-only`) | High-signal machine-readable diagnostics |
| AccessKit semantic UI model | Accessibility ~free once controls declare roles |

### Change (right idea, wrong execution for agents)
| Godot today | AI-first version |
|---|---|
| Opaque generated IDs (`1_ldc4g`, `uid://...`, unique_id) requiring mint-and-cross-reference | Stable, human/agent-derivable IDs (name/content-derived or plain paths); editor bookkeeping in a regenerable side-channel |
| Diff-over-defaults means file ≠ full state; defaults compiled into engine | Ship a queryable machine-readable class-default/schema manifest; validate-before-write, not warn-after |
| Warnings after the fact (config warnings in editor) | Schema/validation layer agents consult before writing |
| Import bolted onto editor (`--import` boots the editor) | First-class `engine import` CLI verb, idempotent, JSON output |
| CLI output mostly human text; few meaningful exit codes | Every verb: structured JSON output + exit codes |
| Headless ⇒ dummy ⇒ zero pixels; screenshots need Xvfb | True offscreen GPU rendering: `render_frame() → PNG` with no display; one-shot capture API |
| MovieWriter determinism scoped to movie mode | Deterministic mode (fixed seed + timestep + reproducible sim) as a general engine flag |
| Physics determinism incidental, threaded ordering unpinned | Contractual deterministic replay mode (Jolt-style); snapshot/restore at the tick boundary |
| Compile-time module system (recompile to toggle features) | Runtime-first modularity; static linking as release optimization only |
| Editor-side jobs (import/export/validate) need the tools binary | All pipeline verbs in one headless-first binary |

### Drop (GUI-workflow surface the thesis deletes)
- The entire GUI editor: docks, inspector, gizmos, project manager, thumbnails/previews, visual
  shader graphs, VCS plugin (agents use git), timeline animation editing UX (author keyframes as
  data; prefer Tween/procedural), 3.x migration tooling.
- Keep only the *data* plugin hooks (import/export/post-import plugins, native extensions).

### Missing entirely (must be built; no Godot reference)
1. **Game-level behavioral testing**: scripted input playback, deterministic tick control from
   tests, scene-state snapshotting/assertions, headless scene playback — "drive player to door,
   assert level loads" as a first-class test.
2. **Golden-frame verification**: deterministic frame hashing, reference-image comparison, diff
   images — agents must verify visuals without eyes.
3. **Machine-readable render/frame metadata** as a query API (why does this frame look like this:
   draw calls, culled counts, effects that ran).
4. **Query/explain surface**: "why did X happen at tick N" over recorded runs (logs + snapshots +
   replay).
5. **Agent-oriented docs**: the engine's API + docs packaged as context for coding agents
   (Godot's extension_api.json + XML docs are 90% of the raw material).

### Notable numbers (reference defaults)
Physics 60 Hz, max 8 catch-up steps, jitter fix 0.5; gravity 980 px/s² (2D); solver 16 iterations;
floor max angle 45°; collision layers 32-bit; audio latency ~11 ms; scene format version 3;
PCK format v4; 16 script-language slots; ~813 documented classes.

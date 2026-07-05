# Midday Engine API reference

GENERATED from engine_api.json. DO NOT EDIT.
Signature compat hashes are XXH3-64 over signature-only JSON (docs excluded).

- engine_version: `0.1.0`
- api_compat_hash: `204ffe872e263449`

## Classes

_None registered._

## Events

### `trigger.entered`

A body began overlapping a trigger volume. Key: the trigger entity.

- level: `core`
- compat_hash: `d9d4b0d4f4ce21a0`

| field | type | doc |
| --- | --- | --- |
| `trigger` | `entity_ref` | The trigger volume's entity. |
| `other` | `entity_ref` | The entity that entered. |

### `trigger.exited`

A body stopped overlapping a trigger volume. Key: the trigger entity.

- level: `core`
- compat_hash: `c954eee9e00e1c41`

| field | type | doc |
| --- | --- | --- |
| `trigger` | `entity_ref` | The trigger volume's entity. |
| `other` | `entity_ref` | The entity that exited. |

### `contact.began`

Physics contact created between two bodies. Dispatched after the physics step in body-pair order (Appendix A phase 6). Key: each involved entity.

- level: `core`
- compat_hash: `08d68516245c6356`

| field | type | doc |
| --- | --- | --- |
| `self` | `entity_ref` | The listening body's entity. |
| `other` | `entity_ref` | The other body's entity. |
| `position` | `vec3` | Contact point, world space. |
| `normal` | `vec3` | Contact normal, world space, from self toward other. |
| `impulse` | `float` | Total normal impulse of the first contact. |

### `contact.ended`

Physics contact between two bodies ceased. Dispatched after the physics step in body-pair order. Key: each involved entity.

- level: `core`
- compat_hash: `b9651ca9f2ae10e3`

| field | type | doc |
| --- | --- | --- |
| `self` | `entity_ref` | The listening body's entity. |
| `other` | `entity_ref` | The other body's entity. |

### `state.finished`

A sequence state's playhead reached its end (end mode 'finish'). Sequence chaining rides this event (spec 4.2). Key: the owning entity.

- level: `core`
- compat_hash: `a8afc4b1ee21c6e0`

| field | type | doc |
| --- | --- | --- |
| `entity` | `entity_ref` | The entity owning the state machine. |
| `region` | `name` | The region containing the finished state. |
| `state` | `name` | The state whose sequence finished. |

### `entity.spawned`

An entity went live at structural apply (Appendix A phase 8), after its initial states entered. Key: the spawned entity.

- level: `core`
- compat_hash: `e749d4b199f3f803`

| field | type | doc |
| --- | --- | --- |
| `entity` | `entity_ref` | The entity that spawned. |
| `parent` | `entity_ref` | The parent it was attached under. |

### `entity.despawned`

An entity was removed at structural apply, after its full exit chains ran; its handles read .alive == false. Key: the despawned entity.

- level: `core`
- compat_hash: `77298a15af3d8e3e`

| field | type | doc |
| --- | --- | --- |
| `entity` | `entity_ref` | The entity that despawned. |

### `action.pressed`

A named input action activated (Appendix A phase 2). Digital bindings report strength 1. Key: global.

- level: `core`
- compat_hash: `f7c859b38c3f04fd`

| field | type | doc |
| --- | --- | --- |
| `action` | `name` | The action-map action name. |
| `strength` | `float` | Activation strength in [0, 1]. |
| `device` | `int` | Originating device index; 0 is the primary. |

### `action.released`

A named input action deactivated. Key: global.

- level: `core`
- compat_hash: `419aa21d0d3630fa`

| field | type | doc |
| --- | --- | --- |
| `action` | `name` | The action-map action name. |
| `device` | `int` | Originating device index; 0 is the primary. |

## Expression functions

### `int(x: float) -> int`

Convert float to int, truncating toward zero. NaN or a value outside int64 range is a runtime expression error.

- level: `core`
- compat_hash: `97aa61acc9a8e040`

### `float(x: int) -> float`

Convert int to float32 (IEEE round-to-nearest).

- level: `core`
- compat_hash: `77bc1325370c7618`

### `abs(x: float) -> float`

Absolute value.

- level: `core`
- compat_hash: `2be5e694437bc61c`

### `sign(x: float) -> float`

1 for positive, -1 for negative, 0 for zero and NaN.

- level: `core`
- compat_hash: `296d5ef965cc7c04`

### `floor(x: float) -> float`

Largest integral value <= x (exact).

- level: `core`
- compat_hash: `e54f5e9135af5e72`

### `ceil(x: float) -> float`

Smallest integral value >= x (exact).

- level: `core`
- compat_hash: `3b9f82e3077fc97c`

### `round(x: float) -> float`

Nearest integral value, halves away from zero (exact).

- level: `core`
- compat_hash: `2b2869a0144ed810`

### `trunc(x: float) -> float`

Integral value toward zero (exact).

- level: `core`
- compat_hash: `0cfe9724f151c1c9`

### `fract(x: float) -> float`

Fractional part: x - floor(x).

- level: `core`
- compat_hash: `00d5fce352c14da3`

### `sqrt(x: float) -> float`

Square root (IEEE correctly rounded). Negative input yields NaN; comparisons against NaN are false.

- level: `core`
- compat_hash: `aae426be34c69e4a`

### `min(a: float, b: float) -> float`

Smaller of a and b (b < a selects b; NaN operands select a).

- level: `core`
- compat_hash: `3c704a47b5553036`

### `max(a: float, b: float) -> float`

Larger of a and b (a < b selects b; NaN operands select a).

- level: `core`
- compat_hash: `ab4ef95b37badfd8`

### `clamp(x: float, lo: float, hi: float) -> float`

x clamped to [lo, hi].

- level: `core`
- compat_hash: `d10ee01d61c3a636`

### `saturate(x: float) -> float`

x clamped to [0, 1].

- level: `core`
- compat_hash: `848a4a2a2801d84e`

### `lerp(a: float, b: float, t: float) -> float`

Linear blend a + (b - a) * t.

- level: `core`
- compat_hash: `0944ebe3db2577e4`

### `vec2(x: float, y: float) -> vec2`

Construct a vec2 from components.

- level: `core`
- compat_hash: `70f80b50873f6801`

### `vec3(x: float, y: float, z: float) -> vec3`

Construct a vec3 from components.

- level: `core`
- compat_hash: `1088c11f718323ef`

### `vec4(x: float, y: float, z: float, w: float) -> vec4`

Construct a vec4 from components.

- level: `core`
- compat_hash: `aec17f3cad127096`

### `quat(x: float, y: float, z: float, w: float) -> quat`

Construct a quaternion from raw components (NOT normalized; rotation use requires unit length — core/math policy).

- level: `core`
- compat_hash: `cdc7f2fd7306d364`

### `dot(a: vec3, b: vec3) -> float`

Dot product.

- level: `core`
- compat_hash: `45304b27cc0a68c3`

### `cross(a: vec3, b: vec3) -> vec3`

Cross product (right-handed).

- level: `core`
- compat_hash: `85571dff02a189d9`

### `length(v: vec3) -> float`

Euclidean length.

- level: `core`
- compat_hash: `afa1e751e469324c`

### `length_squared(v: vec3) -> float`

Squared length (no sqrt — cheaper for radius checks).

- level: `core`
- compat_hash: `f8911f262ef12ccf`

### `normalize(v: vec3) -> vec3`

Unit vector; the zero vector normalizes to zero (core/math policy).

- level: `core`
- compat_hash: `800d5528e1bc254d`

### `distance(a: vec3, b: vec3) -> float`

Euclidean distance between points.

- level: `core`
- compat_hash: `13d9eeac8b5a9f86`

### `distance_squared(a: vec3, b: vec3) -> float`

Squared distance (no sqrt — cheaper for radius checks).

- level: `core`
- compat_hash: `cdca8cde3ff914ad`

### `rotate(q: quat, v: vec3) -> vec3`

Rotate v by the UNIT quaternion q.

- level: `core`
- compat_hash: `81253976db5c7276`

## CLI verbs

### `midday version`

print engine name, version, and build info

- compat_hash: `65ca0a75d45f9a01`

_No flags._

_No positionals._

### `midday selftest`

run the doctest registry embedded in the engine binary

- compat_hash: `8baedffec8c4c9e8`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--filter` | `string` | no |  | doctest test-case filter pattern (e.g. 'cli.*') |

_No positionals._

### `midday help`

show the verb list or one verb's flags and usage

- compat_hash: `4addeb5d102e6d5c`

_No flags._

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `verb` | `name` | no | no | verb to describe; omit for the full verb list |

### `midday api`

emit, diff, or generate from engine_api.json, the canonical API document

- compat_hash: `d026ff9478b1f9bf`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--out` | `string` | no |  | dump: write the document to this path instead of printing it |
| `--out-dir` | `string` | no | `"api"` | codegen: directory for the four generated artifacts |
| `--cache-dir` | `string` | no | `".midday-cache/ts"` | codegen: TS toolchain content-hash cache (regenerable, never committed) |
| `--selfhost` | `bool` | no |  | codegen: run the self-hosted TS-on-QuickJS generator (the default) |
| `--bootstrap` | `bool` | no |  | codegen: run the TEMPORARY native bootstrap generator instead |
| `--verify-equivalence` | `bool` | no |  | codegen: run BOTH generators and byte-compare all four artifacts |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `action` | `name` | yes | no | dump \| diff \| codegen |
| `input` | `string` | no | no | diff: baseline engine_api.json; codegen: input document (default api/engine_api.json) |

### `midday script`

typecheck, lint, transpile, and benchmark TypeScript on the embedded runtime

- compat_hash: `12cc2bd464b3d6cd`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--cache-dir` | `string` | no | `".midday-cache/ts"` | content-hash cache directory (regenerable, never committed) |
| `--stats` | `bool` | no |  | build: report {transpiled, cache_hits} counters in the payload |
| `--out` | `string` | no |  | extract: project-level component schema manifest path to write (required; never api/schema_manifest.json) |
| `--entities` | `int` | no | `1000` | bench: entity count for the budget sweep |
| `--ticks` | `int` | no | `60` | bench: measured ticks (after warmup) |
| `--warmup` | `int` | no | `5` | bench: unmeasured warmup ticks before the window |
| `--naive` | `bool` | no |  | bench: per-field host-hook accessors (the chatty comparison mode) |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `action` | `name` | yes | no | check \| build \| extract \| bench |
| `path` | `string` | no | no | TypeScript source file (bench: overrides the committed fixture) |

### `midday run`

load a scene and step the deterministic sim headless (FLIGHT-recorded)

- compat_hash: `b5a3a0af9153dba5`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--ticks` | `int` | no |  | run exactly N fixed ticks |
| `--to-tick` | `int` | no |  | run until the sim tick reaches N |
| `--seed` | `int` | no | `0` | sim seed (journal identity + RNG streams) |
| `--record` | `string` | no |  | run.mrj bundle path (default: the .midday-cache/run/last.mrj scratch bundle) |
| `--cache-dir` | `string` | no |  | TS build cache directory (default: .midday-cache/ts) |
| `--assert` | `string` | no |  | drive + verify a registered assertion pack: case=<name> (available: appendix_a_golden, determinism_kata) |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `scene` | `string` | yes | no | the *.scene.yaml to load and run |

### `midday journal`

interrogate run.mrj bundles (diff: first-divergent-tick over two runs)

- compat_hash: `108eb4754ab5c342`

_No flags._

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `op` | `string` | yes | no | operation: diff |
| `a` | `string` | yes | no | first run.mrj bundle |
| `b` | `string` | yes | no | second run.mrj bundle |

### `midday rhi`

GPU seam tools (probe: device availability/caps; render: M0 scenes to PNG + decoded-pixel hashes)

- compat_hash: `354cb19c011cf98c`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--backend` | `name` | no | `"vulkan"` | seam implementation: vulkan \| metal (metal is macOS-only) |
| `--validation` | `bool` | no |  | enable the Vulkan validation layer (refuses if not installed) |
| `--software` | `bool` | no |  | require a software rasterizer (lavapipe class; golden lane sets this) |
| `--scene` | `name` | no |  | render one scene: clear \| triangle \| textured_quad (default: all) |
| `--out-dir` | `string` | no |  | write <scene>.png + <scene>.hash + driver.txt here (render) |
| `--goldens` | `string` | no |  | compare decoded-pixel hashes against this golden dir (render) |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `op` | `string` | yes | no | operation: probe \| render |

### `midday shot`

screenshot tools (compare: two-tier golden comparison — decoded-pixel hash + explicit-threshold tolerance, optional diff image)

- compat_hash: `5534b9a4eb0f4256`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--tolerance` | `int` | no | `2` | per-channel delta a pixel may carry without counting as over (0-255) |
| `--max-pct-over` | `float` | no | `0` | percent of pixels allowed over --tolerance before tier 2 fails |
| `--max-mean` | `float` | no | `1` | mean absolute channel delta budget (perceptual drift bound) |
| `--diff` | `string` | no |  | write an amplified per-pixel delta image (x8, saturated) to this PNG path |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `op` | `string` | yes | no | operation: compare |
| `a` | `string` | yes | no | first PNG (golden/reference) |
| `b` | `string` | yes | no | second PNG (candidate) |

### `midday validate`

validate a strict-YAML file against a schema_manifest.json format entry

- compat_hash: `88aa1796cf79b6f3`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--schema` | `name` | no |  | format name to look up in the schema manifest's formats[] table |
| `--manifest` | `string` | no |  | schema manifest path, used with --schema (default: api/schema_manifest.json) |
| `--schema-file` | `string` | no |  | a standalone format-entry JSON document (bypasses the manifest) |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `file` | `string` | yes | no | the strict-YAML file to validate |

### `midday fmt`

canonicalize a strict-YAML file (schema-agnostic; see: midday validate)

- compat_hash: `e8e3a6d51369c502`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--write` | `bool` | no |  | rewrite the file in place with its canonical form |
| `--check` | `bool` | no |  | exit 1 without writing if the file is not already canonical |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `file` | `string` | yes | no | the strict-YAML file to canonicalize |

### `midday check`

audit {uid, path} asset references against the project's .uid sidecars

- compat_hash: `27b0db707c7356c6`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--fix` | `bool` | no |  | repair fixable drift/missing-uid findings in place |
| `--cache-dir` | `string` | no |  | uid registry cache directory (default: <root>/.midday-cache/uid) |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `root` | `string` | yes | no | project directory to scan |

### `midday mv`

move an asset (+ its .uid sidecar) and rewrite referencing paths; the uid never changes

- compat_hash: `47add0a316f29193`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--root` | `string` | no |  | directory to scan for referencing files (default: the current directory) |
| `--cache-dir` | `string` | no |  | uid registry cache directory (default: <root>/.midday-cache/uid) |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `src` | `string` | yes | no | the asset's current path |
| `dst` | `string` | yes | no | the asset's new path |

### `midday new`

scaffold a fresh project: config, import policy, input map, and a first empty scene

- compat_hash: `53e06b733082d58e`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--name` | `string` | no |  | project display name (default: the target directory's own name) |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `dir` | `string` | yes | no | target directory for the new project (must not exist, or be empty) |

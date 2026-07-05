# CODEGEN.md — the byte contract of the engine_api.json code generator

This document specifies EVERY formatting rule the code generator applies.
Two implementations exist and must produce **byte-identical** output:
the SELF-HOSTED TS-on-QuickJS generator (`ts/codegen`, **authoritative**
since `m0-codegen-selfhost` — `midday api codegen`) and the native
bootstrap (`tools/codegen_bootstrap`, TEMPORARY, kept only as the
byte-equivalence pin until it retires post-M0). ONE scoped exception: the
`bindings_spec.json` `batch_envelope` member is self-host-only glue
(D-BUILD-069, see its section) — the equivalence gate compares that file
modulo this member. Because the outputs are otherwise byte-identical,
generated headers are generator-neutral — they never name which
implementation produced them. If a rule is not written here, it is a doc
defect — fix the doc in the same commit that relies on the rule.

Inputs and outputs:

- Input: one `engine_api.json` (format 1, meta-schema
  `formats/engine_api.schema.json`). If the parsed top-level object has **no**
  `format_version` key but has an object under `"api"`, the generator descends
  into it — so `midday api dump --json > f` (the CLI envelope) and
  `midday api dump > f` (the raw document) are both valid inputs.
- Outputs, written to `--out-dir` (default `api/`), always LF line endings and
  exactly one trailing newline, no timestamps, no absolute paths — pure
  functions of the input bytes:
  1. `engine.d.ts` — agent-facing TypeScript declarations.
  2. `schema_manifest.json` — validate-before-write source (per-format
     schemas derived from the reflected type model).
  3. `api_docs.md` — generated reference (agents read this).
  4. `bindings_spec.json` — the glue spec `m0-batch-bindings` implements.
- JSON outputs are serialized with the core writer (`base::Json::dump()`:
  compact, insertion-ordered, deterministic number formatting) plus `"\n"`.

Validation order (all failures are structured errors, tool exit 3):

1. JSON parse (strict core parser) — `json.parse`.
2. Envelope unwrap (rule above).
3. `api::check_document` — `api.malformed` (includes unknown
   `format_version`).
4. Type-spelling walk: every `type`/`returns` string in every section must
   `reflect::TypeDesc::parse` — `codegen.unknown_type` with
   `details.{section,entry,type}`. Every entry/field shape the emitters
   consume is checked here — `codegen.malformed` — so emitters never see
   garbage.

Tool exit classes: `usage.*` → 2; `codegen.io.write`, `codegen.selfcheck`
(post-generation d.ts shape self-check failed — a generator bug, not bad
input) → 1; every other error → 3. Success prints
`{"ok":true,"out_dir":...,"api_compat_hash":...,"files":[...]}` on stdout;
errors print `{"ok":false,"error":{...}}` on stdout (D-BUILD-038).

## Shared text rules

- **pascal_case(name)**: split on `.`, `_`, `-`; uppercase the first ASCII
  letter of each segment; segments otherwise verbatim; join with nothing.
  `trigger.entered` → `TriggerEntered`, `length_squared` → `LengthSquared`.
- **JSDoc escape**: doc strings are emitted on ONE JSDoc line,
  `/** <text> */`; every `*/` inside the text becomes `*\/`; every newline
  becomes a single space. Empty/missing doc ⇒ no JSDoc line at all.
- **Markdown cell escape**: inside table cells, `|` becomes `\|`, newlines
  become a single space.
- Names (events, classes, functions, verbs, fields) are emitted verbatim —
  they are `[a-z0-9._-]` identifiers by convention and never escaped.
- **Dumped defaults are verbatim**: wherever a `default` value is rendered
  with the JSON writer (docs signature ` = <dump>`, docs table cells), the
  dump is emitted as-is inside backticks — it is NOT cell-escaped. A `|`
  inside a default string therefore lands raw in the markdown cell; both
  generators reproduce this byte-for-byte.
- **Member quoting (d.ts only)**: interface MEMBER names (class properties,
  event payload fields, verb flags/positionals) are emitted bare only when
  they are valid TypeScript identifiers (`[A-Za-z_$][A-Za-z0-9_$]*`);
  otherwise they are double-quoted — `"dry-run"?: boolean;`. Lookup-map keys
  are always quoted (unchanged). Function/method PARAMETER names cannot be
  quoted in TypeScript and stay verbatim; a non-identifier param name is a
  registration defect, surfaced by tsc validation of the generated file
  (`midday script check`, m0-quickjs-ts-toolchain).

## Numbers (the writer both generators reproduce)

All JSON output (manifest, bindings, every dumped `default`) goes through
ONE deterministic number writer — core `base::Json::dump()` natively,
`ts/codegen/json.ts` self-hosted:

- **Integer tokens** (no `.`/`e`/`E`) parse as int64 and re-emit exactly
  their canonical decimal digits (JSON already forbids `+`, leading zeros).
  `-0` is the one exception: it parses as double `-0.0` and dumps as `-0`.
  Integer tokens beyond int64 range degrade to double (standard JSON
  interop).
- **Doubles** emit the unique shortest round-trip digits (vendored
  dragonbox natively; `toExponential()` — the same shortest-with-even-ties
  digits by uniqueness — self-hosted) formatted with the
  `std::to_chars(general)` rule: fixed or scientific, whichever is
  SHORTER, ties to fixed. Scientific spells `d[.ddd]e±XX` with an exponent
  of at least two digits (`1e-07`, `1e+21`); fixed forms with no
  fractional part expand the double's EXACT integer value
  (`9223372036854775808`, never zero-padded shortest digits). Non-finite
  doubles serialize as `null` (JSON has no NaN/Inf).
- Pinned by the number-edge corpus (`testkit/codegen_corpus.h`,
  `kCodegenNumberDocument`): int64 range ends, a >2^53 int64 JS `Number`
  cannot hold, `-0`, beyond-int64 degradation, `0.0001` → `1e-04` (the
  shorter-form tie zone), denormal/max doubles, and exact integer
  expansion — byte-compared across both generators by
  `codegen.selfhost.numbers`.

## TypeDesc → TypeScript mapping (the table)

| TypeDesc spelling | TypeScript type | JSON wire shape (manifest) |
|---|---|---|
| `bool` | `boolean` | `boolean` |
| `int` | `number` (int64 semantics) | `integer` |
| `float` | `number` (float32 semantics) | `number` |
| `string` | `string` | `string` |
| `name` | `string` (interned Name) | `string` |
| `vec2` | `Vec2` | `number_tuple` size 2 |
| `vec3` | `Vec3` | `number_tuple` size 3 |
| `vec4` | `Vec4` | `number_tuple` size 4 |
| `quat` | `Quat` | `number_tuple` size 4 |
| `color` | `Color` | `number_tuple` size 4 |
| `entity_ref` | `EntityRef` | `string` (symbolic key) |
| `asset_ref` | `AssetRef` (= `string`) | `string` (project-relative path) |
| `array<T>` | `ts(T)[]` (suffix `[]`) | `array_of_element` |
| `map<T>` | `Record<string, ts(T)>` | `object_of_element` |

## engine.d.ts layout

File = header (4 `//` lines: fixed first line; then
`// engine_version <v>, api_compat_hash <h> (signatures only; docs excluded).`;
then two fixed pointer lines) + blank line + `declare namespace midday {` +
five sections + `}` + newline. Namespace members are indented 4 spaces,
interface/function bodies 8. Every block (banner comment, interface,
function, `namespace expr`) is separated from the next by exactly one blank
line.

Section banners (fixed strings, in order):

1. `// -- Value types (fixed preamble; scalar TypeDesc spellings map per api/CODEGEN.md) --`
2. `// -- Reflected classes (engine_api.json "classes", registration order) --`
3. `// -- Event payloads (engine_api.json "events", registration order) --`
4. `// -- Expression functions (engine_api.json "functions"): expression-language signatures for editor tooling, not TS-callable --`
5. `// -- CLI verbs (engine_api.json "verbs"): midday argv schemas as types, manifest order --`

Section contents:

1. **Value types**: a fixed preamble (see the emitter/golden): `Vec2` `Vec3`
   `Vec4` `Quat` `Color` interfaces with `number` fields, `EntityRef` with
   `readonly alive: boolean;`, `type AssetRef = string;`.
2. **Classes**: per class `interface <Pascal(name)> { ... }` — properties
   first (declaration order; `read_only` flag ⇒ `readonly ` prefix; always
   required — runtime state is fully populated, defaults apply at load),
   then methods `name(params): ret;` where a param with a `default` key gets
   `?`. Then the map `interface Classes { "<name>": <Pascal>; }` (empty ⇒
   `interface Classes {}` on one line) with JSDoc
   `/** Class name -> reflected interface. */`.
3. **Events**: per event `interface <Pascal(name)>Event` with one field per
   payload entry (all required), then map `interface EventPayloads` from
   quoted event name to the interface, JSDoc
   `/** Event name -> payload type. */`.
4. **Functions**: `namespace expr { ... }` containing
   `function <name>(<p>: <T>, ...): <R>;` per function (param with
   `default` ⇒ `?`). Empty inventory ⇒ `namespace expr {}` on one line.
5. **Verbs**: per verb `interface <Pascal(name)>VerbArgs` — flags first
   (declaration order; `bool` flags always `name?: boolean`; other flags
   `?` unless `required`), then positionals (variadic ⇒ `name: T[]`,
   ignoring `required`; else `?` unless required). Then map
   `interface VerbArgsByName { "<name>": <Pascal>VerbArgs; }`, JSDoc
   `/** Verb name -> parsed-argument type. */`.

Generated interface names must be unique across the whole file (checked;
collision is a `codegen.duplicate_symbol` validation error).

## schema_manifest.json layout (format 1)

Key order: `format_version` (1), `api_compat_hash` (copied from input),
`value_types`, `events`, `expr_functions`, `formats`.

- `value_types`: the FIXED 14-entry table above, in TypeKind declaration
  order — `{"spelling","json"[,"size"]}` where `json` ∈ `boolean | integer |
  number | string | number_tuple | array_of_element | object_of_element`
  (`size` only for `number_tuple`). This mirrors
  `reflect::TypeDesc::accepts` — the validate-before-write truth.
- `events`: `[{"name","payload":[{"name","type"}...]}...]`, input order,
  docs stripped.
- `expr_functions`: `[{"name","params":[<spelling>...],"returns"}...]` —
  positional type spellings only (the expr inventory has no defaults,
  D-BUILD-034).
- `formats`: `[]` — scene/machine format schemas join at `m1-scene-format`
  (they append entries here; the key exists from day one so consumers can
  iterate it unconditionally).

Meta-schema: `formats/schema_manifest.schema.json` (subset validator).

## api_docs.md layout

Header: `# Midday Engine API reference`, blank, the fixed GENERATED
paragraph (2 lines), blank, `- engine_version: \`<v>\`` and
`- api_compat_hash: \`<h>\``. Then four `## ` sections: `Classes`, `Events`,
`Expression functions`, `CLI verbs`. Empty section body ⇒ `_None
registered._`. Every entry: `### \`<title>\`` (verbs:
`### \`midday <name>\``; functions: the full signature
`name(p: type, ...) -> ret`, with ` = <dump(default)>` after defaulted
params), then the doc paragraph (omitted when empty), then a `- level:`
line (classes/events/functions; omitted for verbs, which have no level),
`- base:` (whenever the entry carries a string `base` — classes in
practice), `- compat_hash:` — values in backticks. Then per kind:

- Events: `| field | type | doc |` table, or `_No payload._`.
- Classes: `| property | type | default | flags | doc |` table (default =
  `\`<dump>\`` or empty; flags = comma-joined names) or `_No properties._`;
  then `Methods:` list — `- \`<signature>\` (compat_hash \`<h>\`) -- <doc>`
  (` -- <doc>` omitted when empty) — or `_No methods._`.
- Verbs: `Flags:` `| flag | type | required | default | doc |` (flag names
  spelled `--<name>`) or `_No flags._`; then `Positionals:`
  `| positional | type | required | variadic | doc |` or `_No positionals._`.

All blocks separated by exactly one blank line; file ends with one newline.

## bindings_spec.json layout (format 1)

Key order: `format_version` (1), `api_compat_hash`, `expr_functions`,
`events`, `classes`, `batch_envelope`, `state_script_hooks`.

- `expr_functions`, `events`, `classes`: the input entries **deep-copied
  with every `doc`/`summary` key removed** (at any depth); everything else —
  `level`, `default`, `flags`, per-entry and per-method `compat_hash` — stays
  verbatim in input order. These are the per-function call signatures and
  payload decode/encode specs the C++ ⇄ QuickJS glue implements; the hashes
  make glue staleness detectable. `classes` is empty until classes register.
- `batch_envelope` (SELF-HOST ONLY, D-BUILD-069):
  `{"envelope_version":1,"views":[...]}` — the finalized batch-binding glue
  spec (`m0-batch-bindings`; runtime in `ts/runtime/batch_views.h`,
  script surface `midday/batch`). One entry per class, input order:
  `{"component":<name>,"fields":[...]}` where `fields` keeps property
  declaration order and contains only BATCHABLE columns, each
  `{"name","type",<TypeDesc spelling>,"buffer","width","writable"}`:

  | TypeDesc | buffer | width |
  |---|---|---|
  | `bool` | `u8` | 1 |
  | `int` | `f64` (2^53-exact, the JSON number contract) | 1 |
  | `float` | `f32` | 1 |
  | `vec2` / `vec3` / `vec4` / `quat` / `color` | `f32` | 2 / 3 / 4 / 4 / 4 |

  Reference and composite types (`string`, `name`, `entity_ref`,
  `asset_ref`, `array<>`, `map<>`) never batch — they stay on the
  structured seam. `writable` is false exactly when the property carries
  the `read_only` flag (the runtime never scatters such columns back). At
  runtime each view additionally carries `count` and `buffers` (live typed
  arrays), and the envelope carries `tick`; consumers must refuse
  `envelope_version` 0 (the old placeholder) for actual batching.

  The TEMPORARY bootstrap emitter stays frozen on the version-0 placeholder
  — batch glue is post-bootstrap surface. The byte-equivalence gate
  therefore compares `bindings_spec.json` with the `batch_envelope` member
  nulled on both sides (`selfhost::bindings_equivalence_view`); every other
  byte of the artifact, and the other three artifacts, remain full-byte.
  The envelope derivation itself is pinned against LITERAL bytes by
  `codegen.selfhost.batch_envelope`.
- `state_script_hooks` (SELF-HOST ONLY, D-BUILD-084): the state-script hook
  seam contract (`m0-appendix-a-determinism`; runtime in
  `ts/runtime/state_script.h`) —
  `{"envelope_version":1,"register","introspect","invoke","emit","hooks"}`,
  where the four string members are the seam's global function names
  (`__midday_register_state_script`, `__midday_state_hooks_of`,
  `__midday_invoke_state_hook`, `__midday_emit`) and `hooks` is the ordered
  lifecycle vocabulary `["onEnter","onExit","onUpdate","onFixedUpdate"]`
  (spec 4.1 / Appendix A.2.1). The C++ constants are drift-gated against
  the committed artifact by `golden.ts_hook_parity` (seam test). The frozen
  bootstrap never emits this member, so `bindings_equivalence_view` DROPS
  it (vs nulling `batch_envelope`, which both generators emit).

## Determinism and drift gates

Same input bytes → byte-identical outputs, every platform, BOTH
generators. Enforced by: `midday selftest --filter 'codegen.*'`
(golden-pinned synthetic corpus + dual-run equality on the live document +
the `codegen.selfhost.*` equivalence harness over the whole corpus),
verify.sh (two selfhost runs cmp'd, cmp against the four committed `api/`
artifacts, manifest meta-schema-validated, then
`midday api codegen --verify-equivalence`), and the CI drift lane (same
steps, ci preset). Byte-equivalence is a standing gate until the bootstrap
tool retires post-M0. The committed artifacts are regenerated by running
`build/dev/midday api codegen` from the repo root (defaults: input
`api/engine_api.json`, out-dir `api`, self-hosted generator).

CLI exit classes (`midday api codegen`): usage.* → 2; the input's fault
(json.parse, api.malformed, codegen.unknown_type / codegen.malformed /
codegen.duplicate_symbol, codegen.io read failures) → 3; the generator's
or toolchain's fault (codegen.selfcheck, codegen.internal,
codegen.io.write, codegen.equivalence, script.*) → 1.

# CODEGEN.md — the byte contract of the engine_api.json code generator

This document specifies EVERY formatting rule the bootstrap generator
(`tools/codegen_bootstrap`, native, TEMPORARY) applies. It is the
implementation spec for `m0-codegen-selfhost`: the TS-on-QuickJS generator
re-implements exactly these rules and must produce **byte-identical** output
on the bootstrap corpus before it becomes authoritative. If a rule is not
written here, it is a doc defect — fix the doc in the same commit that
relies on the rule.

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
- **Member quoting (d.ts only)**: interface MEMBER names (class properties,
  event payload fields, verb flags/positionals) are emitted bare only when
  they are valid TypeScript identifiers (`[A-Za-z_$][A-Za-z0-9_$]*`);
  otherwise they are double-quoted — `"dry-run"?: boolean;`. Lookup-map keys
  are always quoted (unchanged). Function/method PARAMETER names cannot be
  quoted in TypeScript and stay verbatim; a non-identifier param name is a
  registration defect, surfaced by tsc validation of the generated file
  (`midday script check`, m0-quickjs-ts-toolchain).

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
`- base:` (classes, only when present), `- compat_hash:` — values in
backticks. Then per kind:

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
`events`, `classes`, `batch_envelope`.

- `expr_functions`, `events`, `classes`: the input entries **deep-copied
  with every `doc`/`summary` key removed** (at any depth); everything else —
  `level`, `default`, `flags`, per-entry and per-method `compat_hash` — stays
  verbatim in input order. These are the per-function call signatures and
  payload decode/encode specs the C++ ⇄ QuickJS glue implements; the hashes
  make glue staleness detectable. `classes` is empty until classes register.
- `batch_envelope`: `{"envelope_version":0,"status":"placeholder","doc":
  <fixed string>,"views":[]}` — reserves the shape `m0-batch-bindings`
  designs: per-query SoA views backed by typed arrays (one segment per
  component column, TypeDesc-mapped), pooled math slots, and per-tick
  crossing/GC counters. `envelope_version` stays 0 and `views` stays `[]`
  until that node lands; consumers must refuse `envelope_version` 0 for
  actual batching.

## Determinism and drift gates

Same input bytes → byte-identical outputs, every platform. Enforced by:
`midday selftest --filter 'codegen.*'` (golden-pinned synthetic corpus +
dual-run equality on the live document), verify.sh (two tool runs cmp'd,
then cmp against the four committed `api/` artifacts, manifest
meta-schema-validated), and the CI drift lane (same steps, ci preset).
The committed artifacts are regenerated by running `build/dev/tools/codegen_bootstrap`
from the repo root (defaults: input `api/engine_api.json`, out-dir `api`).

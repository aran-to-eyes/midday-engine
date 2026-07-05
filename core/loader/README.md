# core/loader

The PERMANENT strict-YAML loader subset (m0-yaml-loader-run): authored
scene / machine / events text -> the EXISTING desc aggregates
(`statechart::MachineDesc` and friends) -> live World/Hierarchy
instantiation. No fixture loader exists anywhere; `m1-scene-format`
extends this library in place (schema-manifest validation, uid
dual-write, prefab overrides) and spec §7's "no code-assembled entities
in public paths" holds from the first golden.

Layout:

- `yaml.h` / `yaml_parse.cpp` — the strict-YAML wrapper over vendored
  rapidyaml (the ONLY TU that sees a ryml type): owned node tree, 1-based
  locations on every node and key, refusals for anchors/aliases/tags,
  duplicate keys, and multi-document streams.
- `yaml_emit.h` / `yaml_emit.cpp` (m1-strict-yaml) — the canonical
  serializer, yaml.h's exact counterpart: one deterministic, schema-agnostic
  rendering (`midday fmt`) that is idempotent by construction.
- `parse_util.h/.cpp` — strict field helpers (unknown-key checks, typed
  scalar reads through the core JSON number grammar, `format: 1` gate). NOT
  installed API outside `core/loader/`'s own TUs — `format_schema.cpp`
  reuses these directly as a sibling in the same library.
- `format_schema.h/.cpp` (m1-strict-yaml) — the GENERIC schema-driven
  validation engine (`midday validate`): loads a `formats[]` manifest entry
  (meta-schema `formats/schema_manifest.schema.json` `$defs/format_entry`),
  applies its data-driven migration registry, and validates a parsed
  document against it. This is the MECHANISM `m1-scene-format`'s
  scene/machine/prefab schemas plug into; it does not itself know about
  scenes, machines, or events.
- `events_load.cpp` / `machine_load.cpp` + `machine_parts.cpp` /
  `scene_load.cpp` — the three format loaders (grammar contract:
  `formats/loader_yaml.md`). `events_load.cpp` also carries
  `load_project_events` (m1-events-format): the PROJECT-WIDE namespace
  pass — every `*.events.yaml` under a root, merged into one `EventsDecl`
  via repeated `load_events_file` calls, so a name declared twice anywhere
  under the root refuses even when no single scene lists both files.
  `midday validate <file>.events.yaml` (extension dispatch, no schema
  flag: `cli/verbs/validate.cpp`) is the CLI surface, root = the file's own
  directory.
- `spawn.cpp` — SceneFile -> World: entities, transforms, physics bodies,
  machine instantiation, children under states, symbolic key resolution.
- `uid.h/.cpp` (m1-uid-system) — engine-assigned asset identity: the
  `uid://` textual form, `mint_uid` (authoring-time entropy, never the
  sim), and `.uid` sidecar I/O (committed, format-versioned, spec lines
  366-368).
- `uid_registry.h/.cpp` (m1-uid-system) — scans every `.uid` sidecar under a
  root into a uid<->path map and serializes it to the regenerable
  `.midday-cache/uid/registry.json` cache (write-only output; nothing in
  this tree ever reads it back — the sidecars are the only source of
  truth).
- `asset_ref.h/.cpp` (m1-uid-system) — the `{uid, path}` / `{path}` dual-
  write ref SHAPE, MUTABLE (rewrite-capable): `midday check`'s classify-
  and-repair pass and `midday mv`'s path rewriter (cli/verbs/check.cpp,
  mv.cpp) are its only callers.
- `asset_ref_parse.h/.cpp` (m1-scene-format) — the READ-ONLY counterpart:
  resolves a `{uid?, path}` or bare-path ref DURING LOADING (never
  rewrites). Shared by `scene_load.cpp`'s `prefab:` and `entity_load.cpp`'s
  `machines[].instance` / `attachments[].of` / `attachments[].entity.prefab`.
- `entity_format.h` (m1-scene-format) — the shared data shapes the grammar
  extension needed: `GenericComponentEntry` (a `{Name: {...}}` component
  entry outside the M0 native three), `OverrideEntry`, `AssetRefDesc`,
  `AttachmentDesc`. `loader.h` defines `EntityFile`/`EntityMachineInstance`
  itself (they need the already-complete `MachineFile` type).
- `generic_components.h/.cpp` (m1-scene-format) — the ONE `{Name: {field:
  value, ...}}` list parser, reused by a scene entity's non-native
  components, an entity file's `base:` list, a machine state's own
  `components:`, and a state child's `components:` — one engine, not four
  copies (the jscpd ratchet enforces this).
- `component_vocab.h/.cpp` (m1-scene-format) — the component-NAME
  vocabulary a generic component entry's type is checked against: the M0
  native set always resolves; a project's TS-extracted schema names
  (`midday script extract`) resolve when supplied.
- `override.h/.cpp` (m1-scene-format) — the property-diff override engine:
  parses an `override:` mapping and resolves each path BY NAME ONLY
  (`formats/loader_yaml.md` "Override path grammar") onto a COPY of a
  `MachineFile` — never the caller's cached one (the same machine file may
  be instanced by many entities, each with its own overrides).
- `machine_emit.h/.cpp` (m1-scene-format) — the canonical machine-file
  serializer, `yaml_emit.h`'s semantic counterpart: `MachineFile` ->
  `YamlNode`, `on:` always desugared to `Transition:`, defaults filled.
  `midday scene print --full` on a `*.machine.yaml` file's round-trip-
  stable text.
- `gaps.h` (m1-scene-format) — the unresolved-reference report: a `Gap`
  (kind/what/file/line/col/detail) collected instead of a hard refusal,
  but ONLY in the opt-in LENIENT mode every loader entry point defaults to
  `false` (every existing/default caller keeps M0's exact strict
  behavior) — `midday scene print` is the one caller that opts in.
- `entity_load.cpp` (m1-scene-format) — the NEW `*.entity.yaml` prefab-file
  loader (`entity:` + `base:` + `machines:` + `attachments:`).
- `scene_components.cpp` / `scene_prefab.cpp` (m1-scene-format) —
  `scene_load.cpp` split by the 500-line ratchet: the M0 native + m1
  generic `components:` list, and `prefab:`/`at:`/`override:` resolution
  respectively (shared context: `scene_ctx.h`).
- `machine_components.cpp` (m1-scene-format) — `machine_load.cpp`/
  `machine_parts.cpp` split by the 500-line ratchet: a state's own
  `components:` and components-aware `children:` parsing.

Consumers: `midday run` (cli/verbs/run.cpp), `midday validate`/`midday fmt`
(cli/verbs/validate.cpp, cli/verbs/fmt.cpp), `midday check`/`midday mv`
(cli/verbs/check.cpp, mv.cpp), `midday scene print` (cli/verbs/scene.cpp,
m1-scene-format), and the `loader.*` selftests.
Decisions: D-BUILD-076..081 (M0), formats/loader_yaml.md (M1 additions).

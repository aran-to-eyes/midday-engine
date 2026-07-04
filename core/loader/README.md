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
- `parse_util.h/.cpp` — strict field helpers (unknown-key checks, typed
  scalar reads through the core JSON number grammar, `format: 1` gate).
- `events_load.cpp` / `machine_load.cpp` + `machine_parts.cpp` /
  `scene_load.cpp` — the three format loaders (grammar contract:
  `formats/loader_yaml.md`).
- `spawn.cpp` — SceneFile -> World: entities, transforms, physics bodies,
  machine instantiation, children under states, symbolic key resolution.

Consumers: `midday run` (cli/verbs/run.cpp) and the `loader.*` selftests.
Decisions: D-BUILD-076..081.

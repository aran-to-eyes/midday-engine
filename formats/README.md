# formats

Scene/model/material text format schemas and validators.

- `cli_envelope.schema.json` — the single definition of the CLI JSON envelope (C++ counterpart: `cli/envelope.h`).
- `log_record.schema.json` — the JSONL log record emitted by `core/base/log.*` (mirrored by the `core.log.*` selftests; change both together).
- `run_mrj.md` — the `run.mrj` journal bundle format (spec section 12): directory layout, header identity/info split, record stream, tiers, seek index, determinism guarantees (C++ counterpart: `core/journal/`).
- `mrj_header.schema.json` — the bundle's `header.json` (identity subset is what the replay-identity hash covers).
- `mrj_record.schema.json` — one journal record line inside `journal.jsonl.zst` (pinned by the `journal.record.*` selftests; change together).
- `loader_yaml.md` — the M0 strict-YAML loader subset: `*.scene.yaml` / `*.machine.yaml` / `*.events.yaml` / `*.input.yaml` / `*.input_profile.yaml` grammar, canonicalizations, and refusal rules (C++ counterpart: `core/loader/`; extended IN PLACE by m1-scene-format and m1-input-actions).
- `engine_api.schema.json` — the canonical generated API document `api/engine_api.json` (emitter: `api/engine_api.{h,cpp}`; drift-gated).
- `schema_manifest.schema.json` — the generated validate-before-write manifest `api/schema_manifest.json` (generator: `tools/codegen_bootstrap`; byte contract: `api/CODEGEN.md`). Its `$defs/format_entry` is the MECHANISM a `formats[]` element carries (m1-strict-yaml): a flat field vocabulary plus a data-driven migration chain, consumed generically by `core/loader/format_schema.h` (no per-format C++) and driven from the CLI by `midday validate`; `midday fmt` (`core/loader/yaml_emit.h`) canonicalizes any strict-YAML file independently of any schema. The committed array stays empty until `m1-scene-format` appends real scene/machine/prefab entries.
- `engine_dts.meta.md` — validation conventions for the generated `api/engine.d.ts` (structural pre-tsc check; tsc-level validation joins at m0-quickjs-ts-toolchain).

All JSON schemas are validated by the ONE subset validator `scripts/validate_envelope.py` (D-BUILD-041).

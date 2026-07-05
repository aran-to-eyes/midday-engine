# Schema-engine fixture corpus (m1-strict-yaml)

`widget.schema.json` is a self-contained format-entry document (the shape a
`schema_manifest.json` `formats[]` element carries —
`core/loader/format_schema.h`, meta-schema
`formats/schema_manifest.schema.json` `$defs/format_entry`). It exists only
to exercise the GENERIC validation engine end-to-end through the real CLI;
it is not, and must never become, a scene/machine/prefab schema (that
vocabulary is `m1-scene-format`'s job — see the node's SCOPE BOUNDARY).

`widget`'s current format is 2 (`name` required string, `kind` a 3-way
enum, `amount` an int, `tags` an `array<string>`); format 1 renamed `count`
to `amount`, registered as the one migration step.

| fixture                                | `midday validate --schema-file widget.schema.json …` |
|-----------------------------------------|-------------------------------------------------------|
| `valid_v2.widget.yaml`                  | exit 0                                                 |
| `valid_v1.widget.yaml`                  | exit 0 — migrates `count` -> `amount` first            |
| `mutation_wrong_type.widget.yaml`       | exit 3, `loader.bad_value` (quoted `"12"` is a string) |
| `mutation_unknown_key.widget.yaml`      | exit 3, `loader.unknown_key`                           |
| `mutation_future_format.widget.yaml`    | exit 3, `loader.bad_format` (format 3 > current 2)     |
| `mutation_bad_enum.widget.yaml`         | exit 3, `schema.bad_enum`                              |
| `mutation_duplicate_key.widget.yaml`    | exit 3, `yaml.strict` (the PARSER's own refusal, surfaced, not reimplemented) |
| `mutation_alias.widget.yaml`            | exit 3, `yaml.strict` (anchors/aliases, likewise the parser's refusal) |
| `mutation_missing_required.widget.yaml` | exit 3, `loader.bad_value` (`name` absent) — bonus coverage beyond the six mandated mutation classes |

`scripts/verify.sh`'s "schema validate + fmt" step drives every row above
through the real `build/dev/midday validate` binary.

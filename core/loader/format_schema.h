// core/loader/format_schema.h — the GENERIC schema-driven validation engine
// (m1-strict-yaml): loads a "format entry" (the data shape a
// schema_manifest.json `formats[]` element carries — meta-schema
// formats/schema_manifest.schema.json $defs/format_entry) and validates a
// parsed strict-YAML document (core/loader/yaml.h) against it.
//
// This is the MECHANISM `m1-scene-format` plugs its scene/machine/prefab
// schemas into; the committed api/schema_manifest.json keeps `formats: []`
// (manifest.ts:73) until that node appends real entries. A format entry is
// pure data — no per-format C++ is ever required:
//
//   {"name": "widget", "current_version": 2,
//    "fields": [{"name": "amount", "type": "int", "required": true,
//                "enum": ["a", "b"]}],
//    "migrations": [{"from": 1, "to": 2,
//                     "ops": [{"op": "rename_key", "from": "count",
//                              "to": "amount"}]}]}
//
// `type` is a canonical reflect::TypeDesc spelling — the SAME vocabulary
// schema_manifest.json's value_types/events/expr_functions already use (one
// type language everywhere, spec section 8); field values are checked with
// `TypeDesc::accepts()` over the JSON literal `parse_util`'s `yaml_to_json`
// already derives from a YamlNode (no second type-checking framework).
//
// Reuse, not duplication: unknown-key and required-field refusals go
// through the SAME `core/loader/parse_util.h` helpers the scene/machine/
// events loaders use (identical codes: loader.unknown_key, loader.bad_value)
// and the format-version-missing/future refusal reuses the "loader.bad_format"
// code check_format() established — but check_format() itself is pinned to
// the loader's single global `kFormatVersion` (loader.h), so a genuinely
// generic multi-version gate (with a migration registry underneath) is new
// logic built from the SAME primitives, not a second copy of check_format's
// job. schema.* codes are reserved for the concerns check_format never had:
// malformed schema documents, unknown migration ops, and enum refusals.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/loader/yaml.h"
#include "core/reflect/type_model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace midday::loader {

// m1-scene-format's answer to "does a flat field vocabulary express the
// nested scene grammar": NO, not fully — a scene/machine/entity file's real
// structure (regions -> states -> states..., an entity's polymorphic
// `components:` list) is recursive and open-ended in ways a finite field
// list can never capture without becoming a second JSON-Schema engine. The
// extension stays DELIBERATELY MODEST: `kind` lets a field be an OBJECT or
// an ARRAY-OF-OBJECT with its own nested `fields` (one PRACTICAL win: a
// scene's `entities: [{entity, prefab, at, override, components,
// machines}, ...]` gets real per-entity shape checking) — empty `fields`
// on an object/array-of-object field means "must be that SHAPE, contents
// unchecked" (used for genuinely open-ended things: a machine's `regions:`
// map, an entity's `machines:`/`attachments:` lists). This validates a
// NECESSARY but not SUFFICIENT structural condition — the loader
// (core/loader/scene_load.cpp / machine_load.cpp / entity_load.cpp) stays
// the semantic authority (event vocab, region-wide name resolution,
// override-path resolution, uid resolution); `midday validate --schema
// scene` catches gross shape mistakes, never a substitute for `midday run`
// / `midday scene print` actually loading the file.
enum class FieldKind : std::uint8_t {
    kScalar = 0,        // `type` (a TypeDesc spelling) + optional `enum`
    kObject = 1,        // `fields` (recursive; empty = opaque object)
    kArrayOfObject = 2, // `fields` describes each element (empty = opaque array)
};

struct FieldSchema {
    std::string name;
    FieldKind kind = FieldKind::kScalar;
    reflect::TypeDesc type = reflect::TypeDesc::scalar(reflect::TypeKind::kString); // kScalar only
    bool required = false;
    std::vector<std::string> enum_values; // kScalar only; empty = unconstrained
    std::vector<FieldSchema> fields;      // kObject / kArrayOfObject only
};

// One migration primitive. The vocabulary grows in place as new needs arise
// (M1 needs exactly one: renaming a top-level key); "op" spells the
// operation, "from"/"to" are its string operands.
struct MigrationOp {
    std::string op; // "rename_key"
    std::string from;
    std::string to;
};

// One version step; migrations chain (from -> to -> to -> ... -> current).
struct MigrationStep {
    std::int64_t from_version = 0;
    std::int64_t to_version = 0;
    std::vector<MigrationOp> ops;
};

struct FormatSchema {
    std::string name;
    std::int64_t current_version = 1;
    std::vector<FieldSchema> fields;
    std::vector<MigrationStep> migrations;

    [[nodiscard]] const FieldSchema* find_field(std::string_view field_name) const;
};

struct SchemaLoadResult {
    std::optional<FormatSchema> schema;
    std::optional<base::Error> error; // "schema.malformed" / "schema.unknown_type" /
                                      // "schema.unknown_migration_op"
};

// Parses ONE format-entry JSON document (the `formats[]` element shape).
// `origin` names the source in diagnostics (a file path, or "--schema <name>").
SchemaLoadResult load_format_schema(const base::Json& document, std::string_view origin);

struct ValidateResult {
    bool ok = false;
    std::int64_t authored_version = 0;
    std::int64_t current_version = 0;
    bool migrated = false;
    std::optional<base::Error> error; // loader.bad_format / loader.unknown_key /
                                      // loader.bad_value / schema.bad_enum /
                                      // schema.no_migration_path
};

// Validates `root` (already strict-YAML parsed, core/loader/yaml.h) against
// `schema`: the format-version gate (missing/future -> loader.bad_format,
// reusing the established code) + migration-chain application (a private
// working copy — the caller's tree is never mutated) + unknown-key +
// required + type + enum checks against the CURRENT version's field list.
// `file` is the diagnostic origin stamped into every refusal's file:line:col.
ValidateResult
validate_document(const FormatSchema& schema, const YamlNode& root, std::string_view file);

} // namespace midday::loader

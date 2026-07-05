// core/loader/project_schema.cpp — project_schema.h: the two built-in
// format-entry documents, loaded once through the SAME
// core/loader/format_schema.h entry point a `--schema-file` document uses.
// Malformed built-in JSON is a BUILD defect (it never depends on user
// input), so a load failure aborts loudly — the registry.cpp
// validate_registry() precedent for compiled-in metadata that must always
// be well-formed by construction.

#include "core/loader/project_schema.h"

#include "core/base/json.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace midday::loader {
namespace {

constexpr std::string_view kProjectConfigJson = R"json({
  "name": "project",
  "current_version": 1,
  "fields": [
    {"name": "name", "type": "string", "required": true},
    {"name": "main_scene", "type": "asset_ref", "required": true},
    {"name": "input_map", "type": "asset_ref"},
    {"name": "collision_layers", "type": "array<string>"},
    {"name": "physics_gravity", "type": "vec3"},
    {"name": "physics_fixed_hz", "type": "int"},
    {"name": "seed", "type": "int"}
  ]
})json";

constexpr std::string_view kImportPolicyJson = R"json({
  "name": "import_policy",
  "current_version": 1,
  "fields": [
    {"name": "default_import", "type": "string", "enum": ["mesh", "texture", "audio", "raw"]},
    {"name": "rules", "type": "map<string>"}
  ]
})json";

FormatSchema load_builtin(std::string_view json_text, std::string_view origin) {
    const base::Json::ParseResult parsed = base::Json::parse(json_text, origin);
    if (parsed.error.has_value()) {
        std::fprintf(stderr,
                     "midday: fatal: %s: malformed built-in schema JSON\n",
                     std::string(origin).c_str());
        std::abort();
    }
    SchemaLoadResult loaded = load_format_schema(parsed.value, origin);
    if (!loaded.schema.has_value()) {
        if (loaded.error.has_value())
            std::fprintf(stderr,
                         "midday: fatal: %s: [%s] %s\n",
                         std::string(origin).c_str(),
                         loaded.error->code.c_str(),
                         loaded.error->message.c_str());
        std::abort();
    }
    return std::move(*loaded.schema);
}

} // namespace

const FormatSchema& project_config_schema() {
    static const FormatSchema schema =
        load_builtin(kProjectConfigJson, "midday.project.yaml schema");
    return schema;
}

const FormatSchema& import_policy_schema() {
    static const FormatSchema schema = load_builtin(kImportPolicyJson, "midday.import.yaml schema");
    return schema;
}

} // namespace midday::loader

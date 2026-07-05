// loader.project_schema — the two built-in format entries `midday new`
// scaffolds and `midday validate`'s extension dispatch checks against
// (m1-project-new): both load once, cleanly, and validate_document()
// enforces their fields through the SAME generic engine format_schema_test
// already covers (required/enum/type refusals are that suite's job, not
// re-proven per schema here) — these tests pin the SHAPE of the two
// project-level schemas themselves.

#include "core/loader/project_schema.h"
#include "core/loader/yaml.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

YamlNode parse(const std::string& text) {
    YamlParseResult parsed = parse_yaml(text, "project_schema_test.yaml");
    REQUIRE_FALSE(parsed.error.has_value());
    return std::move(parsed.root);
}

} // namespace

TEST_CASE("loader.project_schema: midday.project.yaml's schema shape") {
    const FormatSchema& schema = project_config_schema();
    CHECK(schema.name == "project");
    CHECK(schema.current_version == 1);
    const FieldSchema* name = schema.find_field("name");
    REQUIRE(name != nullptr);
    CHECK(name->required);
    const FieldSchema* main_scene = schema.find_field("main_scene");
    REQUIRE(main_scene != nullptr);
    CHECK(main_scene->required);
    CHECK(main_scene->type.canonical() == "asset_ref");
    const FieldSchema* input_map = schema.find_field("input_map");
    REQUIRE(input_map != nullptr);
    CHECK_FALSE(input_map->required);
    const FieldSchema* layers = schema.find_field("collision_layers");
    REQUIRE(layers != nullptr);
    CHECK(layers->type.canonical() == "array<string>");
    CHECK(schema.find_field("physics_gravity")->type.canonical() == "vec3");
    CHECK(schema.find_field("physics_fixed_hz")->type.canonical() == "int");
    CHECK(schema.find_field("seed")->type.canonical() == "int");
}

TEST_CASE("loader.project_schema: a minimal and a fully-populated midday.project.yaml validate") {
    const FormatSchema& schema = project_config_schema();
    YamlNode minimal = parse("format: 1\nname: demo\nmain_scene: scenes/main.scene.yaml\n");
    ValidateResult minimal_result = validate_document(schema, minimal, "midday.project.yaml");
    REQUIRE(minimal_result.ok);

    YamlNode full = parse("format: 1\n"
                          "name: demo\n"
                          "main_scene: scenes/main.scene.yaml\n"
                          "input_map: default.input.yaml\n"
                          "collision_layers: [default]\n"
                          "physics_gravity: [0, -9.81, 0]\n"
                          "physics_fixed_hz: 60\n"
                          "seed: 0\n");
    ValidateResult full_result = validate_document(schema, full, "midday.project.yaml");
    REQUIRE(full_result.ok);

    // A missing required field refuses exactly like every other format's
    // check_field (the generic engine, not new logic).
    YamlNode missing = parse("format: 1\nname: demo\n");
    ValidateResult missing_result = validate_document(schema, missing, "midday.project.yaml");
    CHECK_FALSE(missing_result.ok);
    CHECK(unwrap(missing_result.error).code == "loader.bad_value");
}

TEST_CASE("loader.project_schema: midday.import.yaml's schema shape and validation") {
    const FormatSchema& schema = import_policy_schema();
    CHECK(schema.name == "import_policy");
    CHECK(schema.current_version == 1);
    const FieldSchema* default_import = schema.find_field("default_import");
    REQUIRE(default_import != nullptr);
    CHECK(default_import->enum_values.size() == 4);
    const FieldSchema* rules = schema.find_field("rules");
    REQUIRE(rules != nullptr);
    CHECK(rules->type.canonical() == "map<string>");

    YamlNode doc = parse("format: 1\n"
                         "default_import: raw\n"
                         "rules:\n"
                         "  \"**/*.gltf\": mesh\n"
                         "  \"**/*.png\": texture\n");
    ValidateResult result = validate_document(schema, doc, "midday.import.yaml");
    REQUIRE(result.ok);

    YamlNode bad_enum = parse("format: 1\ndefault_import: sprite\n");
    ValidateResult bad_result = validate_document(schema, bad_enum, "midday.import.yaml");
    CHECK_FALSE(bad_result.ok);
    CHECK(unwrap(bad_result.error).code == "schema.bad_enum");

    // An empty document (no fields at all) is legal — both fields are
    // optional; only `format:` is required (the generic engine's own gate).
    YamlNode empty = parse("format: 1\n");
    CHECK(validate_document(schema, empty, "midday.import.yaml").ok);
}

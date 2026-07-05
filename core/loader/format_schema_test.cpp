// loader.format_schema — the generic schema-driven validation engine: format
// entries load from data (no per-format C++), the format-version gate is
// open-ended (older versions migrate forward through a data-driven op
// vocabulary), and validate_document() refuses unknown keys, missing
// required fields, wrong types, and bad enums with file:line:col — all
// through the SAME primitives the scene/machine/events loaders use.

#include "core/loader/format_schema.h"
#include "core/loader/yaml.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <string>

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

constexpr std::string_view kWidgetSchemaJson = R"({
  "name": "widget",
  "current_version": 2,
  "fields": [
    {"name": "name", "type": "string", "required": true},
    {"name": "kind", "type": "string", "enum": ["circle", "square", "triangle"]},
    {"name": "amount", "type": "int"},
    {"name": "tags", "type": "array<string>"}
  ],
  "migrations": [
    {"from": 1, "to": 2, "ops": [{"op": "rename_key", "from": "count", "to": "amount"}]}
  ]
})";

FormatSchema widget_schema() {
    base::Json::ParseResult parsed = base::Json::parse(kWidgetSchemaJson);
    REQUIRE_FALSE(parsed.error.has_value());
    SchemaLoadResult loaded = load_format_schema(parsed.value, "widget.schema.json");
    REQUIRE_FALSE(loaded.error.has_value());
    return std::move(unwrap(loaded.schema));
}

YamlNode parse(const std::string& text) {
    YamlParseResult parsed = parse_yaml(text, "widget.yaml");
    REQUIRE_FALSE(parsed.error.has_value());
    return std::move(parsed.root);
}

} // namespace

TEST_CASE("loader.format_schema: a well-formed format entry loads") {
    const FormatSchema schema = widget_schema();
    CHECK(schema.name == "widget");
    CHECK(schema.current_version == 2);
    REQUIRE(schema.fields.size() == 4);
    const FieldSchema* kind = schema.find_field("kind");
    REQUIRE(kind != nullptr);
    CHECK(kind->enum_values.size() == 3);
    CHECK(schema.migrations.size() == 1);
    CHECK(schema.find_field("nope") == nullptr);
}

TEST_CASE("loader.format_schema: malformed format entries refuse structurally") {
    auto load = [](std::string_view json) {
        base::Json::ParseResult parsed = base::Json::parse(json);
        REQUIRE_FALSE(parsed.error.has_value());
        return load_format_schema(parsed.value, "s.json");
    };
    CHECK(unwrap(load(R"([])").error).code == "schema.malformed");
    CHECK(unwrap(load(R"({"current_version":1,"fields":[]})").error).code == "schema.malformed");
    CHECK(unwrap(load(R"({"name":"w","fields":[]})").error).code == "schema.malformed");
    CHECK(unwrap(load(R"({"name":"w","current_version":1})").error).code == "schema.malformed");
    CHECK(unwrap(load(R"({"name":"w","current_version":1,
                          "fields":[{"name":"x","type":"nope"}]})")
                     .error)
              .code == "schema.unknown_type");
    CHECK(unwrap(load(R"({"name":"w","current_version":1,"fields":[],
                          "migrations":[{"from":1,"to":2,
                            "ops":[{"op":"bogus"}]}]})")
                     .error)
              .code == "schema.unknown_migration_op");
    // 'enum' constrains string content; on a non-string field it is refused at
    // load (never allowed to reach the validator, which would read it as a
    // string and crash on a document that type-checks).
    const base::Error enum_on_int = unwrap(load(R"({"name":"w","current_version":1,
                        "fields":[{"name":"n","type":"int","enum":["1","2"]}]})")
                                               .error);
    CHECK(enum_on_int.code == "schema.malformed");
    CHECK(enum_on_int.message.find("'enum' is only valid on string/name") != std::string::npos);
    // ...but string/name fields accept it.
    CHECK_FALSE(load(R"({"name":"w","current_version":1,
                         "fields":[{"name":"n","type":"name","enum":["a","b"]}]})")
                    .error.has_value());
}

TEST_CASE("loader.format_schema: missing/future format version") {
    const FormatSchema schema = widget_schema();

    YamlNode no_format = parse("name: bolt\n");
    ValidateResult missing = validate_document(schema, no_format, "w.yaml");
    CHECK_FALSE(missing.ok);
    CHECK(unwrap(missing.error).code == "loader.bad_format");

    YamlNode future = parse("format: 3\nname: bolt\n");
    ValidateResult refused = validate_document(schema, future, "w.yaml");
    CHECK_FALSE(refused.ok);
    CHECK(unwrap(refused.error).code == "loader.bad_format");
    CHECK(unwrap(refused.error).message.find("up to format 2") != std::string::npos);
}

TEST_CASE("loader.format_schema: unknown key, wrong type, bad enum, missing required") {
    const FormatSchema schema = widget_schema();

    YamlNode unknown = parse("format: 2\nname: bolt\nbogus: 1\n");
    ValidateResult unknown_result = validate_document(schema, unknown, "w.yaml");
    CHECK(unwrap(unknown_result.error).code == "loader.unknown_key");

    YamlNode wrong_type = parse("format: 2\nname: bolt\namount: \"12\"\n");
    ValidateResult wrong_type_result = validate_document(schema, wrong_type, "w.yaml");
    CHECK(unwrap(wrong_type_result.error).code == "loader.bad_value");
    CHECK(unwrap(wrong_type_result.error).message.find("expected int, got a string") !=
          std::string::npos);

    YamlNode bad_enum = parse("format: 2\nname: bolt\nkind: hexagon\n");
    ValidateResult bad_enum_result = validate_document(schema, bad_enum, "w.yaml");
    CHECK(unwrap(bad_enum_result.error).code == "schema.bad_enum");
    CHECK(unwrap(bad_enum_result.error).message.find("circle, square, triangle") !=
          std::string::npos);

    YamlNode missing_required = parse("format: 2\nkind: circle\n");
    ValidateResult missing_result = validate_document(schema, missing_required, "w.yaml");
    CHECK(unwrap(missing_result.error).code == "loader.bad_value");
    CHECK(unwrap(missing_result.error).message.find("'name'") != std::string::npos);
}

TEST_CASE("loader.format_schema: a valid document round-trips clean") {
    const FormatSchema schema = widget_schema();
    YamlNode doc = parse("format: 2\nname: bolt\nkind: circle\namount: 12\ntags: [a, b]\n");
    ValidateResult result = validate_document(schema, doc, "w.yaml");
    REQUIRE(result.ok);
    CHECK_FALSE(result.error.has_value());
    CHECK(result.authored_version == 2);
    CHECK_FALSE(result.migrated);
}

TEST_CASE("loader.format_schema: the migration registry renames a key forward and validates") {
    const FormatSchema schema = widget_schema();
    YamlNode v1 = parse("format: 1\nname: bolt\ncount: 12\n");
    ValidateResult migrated = validate_document(schema, v1, "w.yaml");
    REQUIRE(migrated.ok);
    CHECK(migrated.authored_version == 1);
    CHECK(migrated.current_version == 2);
    CHECK(migrated.migrated);
    // The original tree is untouched: migrations work on a private copy.
    CHECK(v1.find("count") != nullptr);
    CHECK(v1.find("amount") == nullptr);
}

TEST_CASE("loader.format_schema: a gap in the migration chain refuses") {
    FormatSchema schema = widget_schema();
    schema.current_version = 3; // no registered step from 2 -> 3
    YamlNode v1 = parse("format: 1\nname: bolt\ncount: 12\n");
    ValidateResult result = validate_document(schema, v1, "w.yaml");
    CHECK_FALSE(result.ok);
    CHECK(unwrap(result.error).code == "schema.no_migration_path");
}

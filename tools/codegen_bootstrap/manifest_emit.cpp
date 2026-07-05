// tools/codegen_bootstrap/manifest_emit.cpp — schema_manifest.json emitter:
// the validate-before-write source (spec section 8). Shape spec:
// api/CODEGEN.md "schema_manifest.json layout"; meta-schema:
// formats/schema_manifest.schema.json. Scene/machine format schemas append
// under "formats" at m1-scene-format.

#include "tools/codegen_bootstrap/codegen.h"
#include "tools/codegen_bootstrap/emit_util.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::codegen {

using base::Json;
using detail::entries;
using detail::str;

namespace {

// `formats[]` (m1-scene-format, manifest.ts:73's reserved slot) — the
// scene/machine/entity format-entry schemas core/loader/format_schema.h's
// generic engine validates against. Hardcoded DATA, exactly like
// value_types() above — never derived from `document` — and byte-for-byte
// the SAME table ts/codegen/manifest.ts's FORMAT_ENTRIES builds (the
// selfhost/bootstrap equivalence gate, api/CODEGEN.md). A scalar field (no
// `kind`) is a TypeDesc spelling; `kind: "object"` / `"array_of_object"`
// fields carry their own nested `fields` (empty = opaque).
struct FieldDef {
    std::string name;
    std::optional<std::string> type;
    bool required = false;
    std::vector<std::string> enum_values;
    std::optional<std::string> kind;
    std::vector<FieldDef> fields;
};

struct FormatDef {
    std::string name;
    std::int64_t current_version = 1;
    std::vector<FieldDef> fields;
};

Json field_to_json(const FieldDef& field) {
    Json out = Json::object();
    out.set("name", field.name);
    if (field.kind.has_value()) {
        out.set("kind", *field.kind);
        Json nested = Json::array();
        for (const FieldDef& child : field.fields)
            nested.push(field_to_json(child));
        out.set("fields", std::move(nested));
    } else {
        out.set("type", field.type.value_or("string"));
    }
    if (field.required)
        out.set("required", true);
    if (!field.enum_values.empty()) {
        Json enum_json = Json::array();
        for (const std::string& value : field.enum_values)
            enum_json.push(value);
        out.set("enum", std::move(enum_json));
    }
    return out;
}

Json format_to_json(const FormatDef& format) {
    Json out = Json::object();
    out.set("name", format.name);
    out.set("current_version", format.current_version);
    Json fields = Json::array();
    for (const FieldDef& field : format.fields)
        fields.push(field_to_json(field));
    out.set("fields", std::move(fields));
    return out;
}

// Plain construct-then-assign throughout this file (never a PARTIAL
// designated initializer): `FieldDef`/`FormatDef` carry several optional
// members, and `-Wmissing-designated-field-initializers` (warnings-as-
// errors, .clang-tidy) refuses a designated-init expression that skips any
// of them — assignment sidesteps the question entirely.
FieldDef scalar_field(std::string name, std::string type, bool required = false) {
    FieldDef field;
    field.name = std::move(name);
    field.type = std::move(type);
    field.required = required;
    return field;
}

FieldDef opaque_list(std::string name) {
    FieldDef field;
    field.name = std::move(name);
    field.kind = "array_of_object";
    return field;
}

FieldDef opaque_object(std::string name, bool required = false) {
    FieldDef field;
    field.name = std::move(name);
    field.required = required;
    field.kind = "object";
    return field;
}

std::vector<FormatDef> format_entries() {
    std::vector<FormatDef> formats;

    FormatDef scene;
    scene.name = "scene";
    scene.fields.push_back(scalar_field("scene", "name", /*required=*/true));
    scene.fields.push_back(scalar_field("events", "array<string>"));
    FieldDef entities = opaque_list("entities");
    entities.fields.push_back(scalar_field("entity", "name", /*required=*/true));
    entities.fields.push_back(opaque_list("components"));
    entities.fields.push_back(opaque_list("machines"));
    entities.fields.push_back(opaque_object("prefab"));
    entities.fields.push_back(scalar_field("at", "array<float>"));
    entities.fields.push_back(opaque_object("override"));
    scene.fields.push_back(std::move(entities));
    formats.push_back(std::move(scene));

    FormatDef machine;
    machine.name = "machine";
    machine.fields.push_back(scalar_field("machine", "name", /*required=*/true));
    machine.fields.push_back(opaque_object("vars"));
    machine.fields.push_back(opaque_object("regions", /*required=*/true));
    formats.push_back(std::move(machine));

    FormatDef entity;
    entity.name = "entity";
    entity.fields.push_back(scalar_field("entity", "name", /*required=*/true));
    entity.fields.push_back(opaque_list("base"));
    entity.fields.push_back(opaque_list("machines"));
    entity.fields.push_back(opaque_list("attachments"));
    formats.push_back(std::move(entity));

    return formats;
}

// The fixed spelling table, TypeKind declaration order — one row per
// TypeDesc spelling, its JSON wire shape mirroring reflect::TypeDesc::accepts.
Json value_types() {
    Json table = Json::array();
    auto add = [&table](std::string_view spelling, std::string_view json, std::int64_t size) {
        Json row = Json::object();
        row.set("spelling", spelling);
        row.set("json", json);
        if (size != 0)
            row.set("size", size);
        table.push(std::move(row));
    };
    add("bool", "boolean", 0);
    add("int", "integer", 0);
    add("float", "number", 0);
    add("string", "string", 0);
    add("name", "string", 0);
    add("vec2", "number_tuple", 2);
    add("vec3", "number_tuple", 3);
    add("vec4", "number_tuple", 4);
    add("quat", "number_tuple", 4);
    add("color", "number_tuple", 4);
    add("entity_ref", "string", 0);
    add("asset_ref", "string", 0);
    add("array", "array_of_element", 0);
    add("map", "object_of_element", 0);
    return table;
}

} // namespace

std::string emit_manifest(const Json& document) {
    Json manifest = Json::object();
    manifest.set("format_version", std::int64_t{1});
    manifest.set("api_compat_hash", str(document, "api_compat_hash"));
    manifest.set("value_types", value_types());

    Json events = Json::array();
    for (const Json& entry : entries(document, "events")) {
        Json event = Json::object();
        event.set("name", str(entry, "name"));
        Json payload = Json::array();
        for (const Json& field : entries(entry, "payload")) {
            Json row = Json::object();
            row.set("name", str(field, "name"));
            row.set("type", str(field, "type"));
            payload.push(std::move(row));
        }
        event.set("payload", std::move(payload));
        events.push(std::move(event));
    }
    manifest.set("events", std::move(events));

    Json functions = Json::array();
    for (const Json& entry : entries(document, "functions")) {
        Json function = Json::object();
        function.set("name", str(entry, "name"));
        Json params = Json::array();
        for (const Json& param : entries(entry, "params"))
            params.push(str(param, "type"));
        function.set("params", std::move(params));
        function.set("returns", str(entry, "returns"));
        functions.push(std::move(function));
    }
    manifest.set("expr_functions", std::move(functions));

    Json formats = Json::array();
    for (const FormatDef& format : format_entries())
        formats.push(format_to_json(format));
    manifest.set("formats", std::move(formats));
    return manifest.dump() + "\n";
}

} // namespace midday::codegen

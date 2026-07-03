// tools/codegen_bootstrap/manifest_emit.cpp — schema_manifest.json emitter:
// the validate-before-write source (spec section 8). Shape spec:
// api/CODEGEN.md "schema_manifest.json layout"; meta-schema:
// formats/schema_manifest.schema.json. Scene/machine format schemas append
// under "formats" at m1-scene-format.

#include "tools/codegen_bootstrap/codegen.h"
#include "tools/codegen_bootstrap/emit_util.h"

#include <cstdint>
#include <string_view>

namespace midday::codegen {

using base::Json;
using detail::entries;
using detail::str;

namespace {

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

    manifest.set("formats", Json::array());
    return manifest.dump() + "\n";
}

} // namespace midday::codegen

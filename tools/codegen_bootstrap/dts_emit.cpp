// tools/codegen_bootstrap/dts_emit.cpp — engine.d.ts emitter. Layout spec:
// api/CODEGEN.md "engine.d.ts layout"; bytes pinned by the codegen.golden
// selftest and the committed api/engine.d.ts drift gate.

#include "tools/codegen_bootstrap/codegen.h"
#include "tools/codegen_bootstrap/emit_util.h"

#include <string>
#include <string_view>
#include <vector>

namespace midday::codegen {

using base::Json;
using detail::entries;
using detail::str;
using detail::text;
using detail::truthy;

namespace {

// The fixed value-type preamble: one declaration per scalar TypeDesc
// spelling that is not a TypeScript primitive (mapping table in CODEGEN.md).
constexpr std::string_view kValueTypes =
    R"(    /** TypeDesc "vec2": 2D float vector. */
    interface Vec2 {
        x: number;
        y: number;
    }

    /** TypeDesc "vec3": 3D float vector. */
    interface Vec3 {
        x: number;
        y: number;
        z: number;
    }

    /** TypeDesc "vec4": 4D float vector. */
    interface Vec4 {
        x: number;
        y: number;
        z: number;
        w: number;
    }

    /** TypeDesc "quat": rotation quaternion; JSON spelling [x, y, z, w]. */
    interface Quat {
        x: number;
        y: number;
        z: number;
        w: number;
    }

    /** TypeDesc "color": linear RGBA; JSON spelling [r, g, b, a]. */
    interface Color {
        r: number;
        g: number;
        b: number;
        a: number;
    }

    /** TypeDesc "entity_ref": generational entity handle; a stale handle reads alive == false. */
    interface EntityRef {
        readonly alive: boolean;
    }

    /** TypeDesc "asset_ref": project-root-relative asset path. */
    type AssetRef = string;
)";

// One JSDoc line at `indent`, or nothing when the doc is empty.
std::string jsdoc(std::string_view doc, std::string_view indent) {
    if (doc.empty())
        return {};
    std::string out(indent);
    out += "/** ";
    out += jsdoc_escape(doc);
    out += " */\n";
    return out;
}

// `    interface <name> {}` or a full body; body lines are pre-indented.
std::string interface_block(std::string_view doc, std::string_view name, std::string_view body) {
    std::string out = jsdoc(doc, "    ");
    out += "    interface ";
    out += name;
    out += body.empty() ? " {}\n" : " {\n" + std::string(body) + "    }\n";
    return out;
}

// `name: T;` member line with optional JSDoc, 8-space indent.
std::string member(std::string_view doc, std::string_view declaration) {
    std::string out = jsdoc(doc, "        ");
    out += "        ";
    out += declaration;
    out += ";\n";
    return out;
}

// `a: number, b?: number` — a param with a default becomes optional.
std::string param_list(const Json& holder) {
    std::string out;
    for (const Json& param : entries(holder, "params")) {
        if (!out.empty())
            out += ", ";
        out += str(param, "name");
        out += param.find("default") != nullptr ? "?: " : ": ";
        out += ts_type(str(param, "type"));
    }
    return out;
}

// The `"<name>": <Type>;` lookup-map interface.
std::string map_block(std::string_view doc,
                      std::string_view map_name,
                      const std::vector<std::pair<std::string, std::string>>& rows) {
    std::string body;
    for (const auto& [key, type] : rows) {
        body += "        \"";
        body += key;
        body += "\": ";
        body += type;
        body += ";\n";
    }
    return interface_block(doc, map_name, body);
}

std::string class_block(const Json& entry) {
    std::string body;
    for (const Json& property : entries(entry, "properties")) {
        std::string declaration;
        const Json* flags = property.find("flags");
        if (flags != nullptr)
            for (const Json& flag : flags->elements())
                if (flag.as_string() == "read_only")
                    declaration += "readonly ";
        declaration += str(property, "name") + ": " + ts_type(str(property, "type"));
        body += member(text(property, "doc"), declaration);
    }
    for (const Json& method : entries(entry, "methods"))
        body += member(text(method, "doc"),
                       str(method, "name") + "(" + param_list(method) +
                           "): " + ts_type(str(method, "returns")));
    return interface_block(text(entry, "doc"), pascal_case(str(entry, "name")), body);
}

std::string event_block(const Json& entry) {
    std::string body;
    for (const Json& field : entries(entry, "payload"))
        body += member(text(field, "doc"), str(field, "name") + ": " + ts_type(str(field, "type")));
    return interface_block(text(entry, "doc"), pascal_case(str(entry, "name")) + "Event", body);
}

std::string expr_block(const Json& document) {
    std::string body;
    for (const Json& entry : entries(document, "functions"))
        body += member(text(entry, "doc"),
                       "function " + str(entry, "name") + "(" + param_list(entry) +
                           "): " + ts_type(str(entry, "returns")));
    if (body.empty())
        return "    namespace expr {}\n";
    return "    namespace expr {\n" + body + "    }\n";
}

std::string verb_block(const Json& entry) {
    std::string body;
    for (const Json& flag : entries(entry, "flags")) {
        const std::string& type = str(flag, "type");
        std::string declaration = str(flag, "name");
        if (type == "bool")
            declaration += "?: boolean";
        else
            declaration += (truthy(flag, "required") ? ": " : "?: ") + ts_type(type);
        body += member(text(flag, "doc"), declaration);
    }
    for (const Json& positional : entries(entry, "positionals")) {
        std::string declaration = str(positional, "name");
        if (truthy(positional, "variadic"))
            declaration += ": " + ts_type(str(positional, "type")) + "[]";
        else
            declaration +=
                (truthy(positional, "required") ? ": " : "?: ") + ts_type(str(positional, "type"));
        body += member(text(positional, "doc"), declaration);
    }
    return interface_block(
        text(entry, "summary"), pascal_case(str(entry, "name")) + "VerbArgs", body);
}

} // namespace

std::string emit_dts(const Json& document) {
    std::vector<std::string> blocks;
    blocks.emplace_back("    // -- Value types (fixed preamble; scalar TypeDesc spellings map "
                        "per api/CODEGEN.md) --\n");
    blocks.emplace_back(kValueTypes);

    blocks.emplace_back(
        "    // -- Reflected classes (engine_api.json \"classes\", registration order) --\n");
    std::vector<std::pair<std::string, std::string>> rows;
    for (const Json& entry : entries(document, "classes")) {
        blocks.push_back(class_block(entry));
        rows.emplace_back(str(entry, "name"), pascal_case(str(entry, "name")));
    }
    blocks.push_back(map_block("Class name -> reflected interface.", "Classes", rows));

    blocks.emplace_back(
        "    // -- Event payloads (engine_api.json \"events\", registration order) --\n");
    rows.clear();
    for (const Json& entry : entries(document, "events")) {
        blocks.push_back(event_block(entry));
        rows.emplace_back(str(entry, "name"), pascal_case(str(entry, "name")) + "Event");
    }
    blocks.push_back(map_block("Event name -> payload type.", "EventPayloads", rows));

    blocks.emplace_back("    // -- Expression functions (engine_api.json \"functions\"): "
                        "expression-language signatures for editor tooling, not TS-callable --\n");
    blocks.push_back(expr_block(document));

    blocks.emplace_back("    // -- CLI verbs (engine_api.json \"verbs\"): midday argv schemas "
                        "as types, manifest order --\n");
    rows.clear();
    for (const Json& entry : entries(document, "verbs")) {
        blocks.push_back(verb_block(entry));
        rows.emplace_back(str(entry, "name"), pascal_case(str(entry, "name")) + "VerbArgs");
    }
    blocks.push_back(map_block("Verb name -> parsed-argument type.", "VerbArgsByName", rows));

    std::string out =
        "// engine.d.ts -- GENERATED by tools/codegen_bootstrap from engine_api.json. "
        "DO NOT EDIT.\n// engine_version ";
    out += str(document, "engine_version");
    out += ", api_compat_hash ";
    out += str(document, "api_compat_hash");
    out += " (signatures only; docs excluded).\n"
           "// Formatting rules + the TypeDesc -> TypeScript mapping table: api/CODEGEN.md.\n"
           "// Structural (pre-tsc) validation conventions: formats/engine_dts.meta.md.\n\n"
           "declare namespace midday {\n";
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (i != 0)
            out += "\n";
        out += blocks[i];
    }
    out += "}\n";
    return out;
}

} // namespace midday::codegen

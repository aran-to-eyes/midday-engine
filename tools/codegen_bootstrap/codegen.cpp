// tools/codegen_bootstrap/codegen.cpp — input loading + validation, the
// shared text rules, the d.ts structural shape check, and exit-class
// mapping. The byte contract for everything here: api/CODEGEN.md.

#include "tools/codegen_bootstrap/codegen.h"

#include "api/engine_api.h"
#include "core/reflect/type_model.h"
#include "tools/codegen_bootstrap/emit_util.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace midday::codegen {

using base::Error;
using base::Json;

namespace {

Error make_error(std::string code, std::string message, Json details) {
    return Error{
        .code = std::move(code), .message = std::move(message), .details = std::move(details)};
}

Json where(std::string_view section, std::string_view entry) {
    Json details = Json::object();
    details.set("section", section);
    details.set("entry", entry);
    return details;
}

std::optional<Error>
malformed(std::string_view section, std::string_view entry, std::string_view problem) {
    Json details = where(section, entry);
    details.set("problem", problem);
    return make_error("codegen.malformed",
                      "section '" + std::string(section) + "' entry '" + std::string(entry) +
                          "': " + std::string(problem),
                      std::move(details));
}

// A "type"-carrying key must hold a canonical TypeDesc spelling.
std::optional<Error> check_spelling(std::string_view section,
                                    std::string_view entry,
                                    const Json& holder,
                                    std::string_view key) {
    const Json* spelling = holder.find(key);
    if (spelling == nullptr || !spelling->is_string())
        return malformed(section, entry, "missing string '" + std::string(key) + "'");
    if (!reflect::TypeDesc::parse(spelling->as_string()).has_value()) {
        Json details = where(section, entry);
        details.set("type", *spelling);
        return make_error("codegen.unknown_type",
                          "section '" + std::string(section) + "' entry '" + std::string(entry) +
                              "': unknown type spelling '" + spelling->as_string() + "'",
                          std::move(details));
    }
    return std::nullopt;
}

// A field list ("payload", "params", "properties", "flags", "positionals"):
// an array of objects, each with a string name and a valid type spelling.
std::optional<Error> check_fields(std::string_view section,
                                  std::string_view entry,
                                  const Json& holder,
                                  std::string_view key) {
    const Json* fields = holder.find(key);
    if (fields == nullptr || !fields->is_array())
        return malformed(section, entry, "missing array '" + std::string(key) + "'");
    for (const Json& field : fields->elements()) {
        const Json* name = field.find("name");
        if (!field.is_object() || name == nullptr || !name->is_string())
            return malformed(section, entry, std::string(key) + " item without a string name");
        if (auto error = check_spelling(section, entry, field, "type"))
            return error;
    }
    return std::nullopt;
}

// Everything the emitters read beyond api::check_document's guarantees:
// field lists, method/function signatures, and generated-symbol uniqueness.
std::optional<Error> check_generation_model(const Json& document) {
    std::vector<std::string> symbols = {"Vec2",
                                        "Vec3",
                                        "Vec4",
                                        "Quat",
                                        "Color",
                                        "EntityRef",
                                        "AssetRef",
                                        "expr",
                                        "Classes",
                                        "EventPayloads",
                                        "VerbArgsByName"};
    auto claim = [&symbols](std::string symbol) -> std::optional<Error> {
        for (const std::string& seen : symbols)
            if (seen == symbol) {
                Json details = Json::object();
                details.set("symbol", symbol);
                return make_error("codegen.duplicate_symbol",
                                  "generated TypeScript symbol '" + symbol +
                                      "' collides (api/CODEGEN.md naming rules)",
                                  std::move(details));
            }
        symbols.push_back(std::move(symbol));
        return std::nullopt;
    };

    for (const Json& entry : detail::entries(document, "classes")) {
        const std::string& name = detail::str(entry, "name");
        if (auto error = check_fields("classes", name, entry, "properties"))
            return error;
        const Json* methods = entry.find("methods");
        if (methods == nullptr || !methods->is_array())
            return malformed("classes", name, "missing array 'methods'");
        for (const Json& method : methods->elements()) {
            const Json* method_name = method.find("name");
            if (!method.is_object() || method_name == nullptr || !method_name->is_string())
                return malformed("classes", name, "method without a string name");
            if (auto error = check_fields("classes", name, method, "params"))
                return error;
            if (auto error = check_spelling("classes", name, method, "returns"))
                return error;
        }
        if (auto error = claim(pascal_case(name)))
            return error;
    }
    for (const Json& entry : detail::entries(document, "events")) {
        const std::string& name = detail::str(entry, "name");
        if (auto error = check_fields("events", name, entry, "payload"))
            return error;
        if (auto error = claim(pascal_case(name) + "Event"))
            return error;
    }
    for (const Json& entry : detail::entries(document, "functions")) {
        const std::string& name = detail::str(entry, "name");
        if (auto error = check_fields("functions", name, entry, "params"))
            return error;
        if (auto error = check_spelling("functions", name, entry, "returns"))
            return error;
    }
    for (const Json& entry : detail::entries(document, "verbs")) {
        const std::string& name = detail::str(entry, "name");
        if (auto error = check_fields("verbs", name, entry, "flags"))
            return error;
        if (auto error = check_fields("verbs", name, entry, "positionals"))
            return error;
        if (auto error = claim(pascal_case(name) + "VerbArgs"))
            return error;
    }
    return std::nullopt;
}

std::string ts_of(const reflect::TypeDesc& type) {
    switch (type.kind()) {
    case reflect::TypeKind::kBool:
        return "boolean";
    case reflect::TypeKind::kInt:
    case reflect::TypeKind::kFloat:
        return "number";
    case reflect::TypeKind::kString:
    case reflect::TypeKind::kName:
        return "string";
    case reflect::TypeKind::kVec2:
        return "Vec2";
    case reflect::TypeKind::kVec3:
        return "Vec3";
    case reflect::TypeKind::kVec4:
        return "Vec4";
    case reflect::TypeKind::kQuat:
        return "Quat";
    case reflect::TypeKind::kColor:
        return "Color";
    case reflect::TypeKind::kEntityRef:
        return "EntityRef";
    case reflect::TypeKind::kAssetRef:
        return "AssetRef";
    case reflect::TypeKind::kArray:
        return ts_of(type.element()) + "[]";
    case reflect::TypeKind::kMap:
        return "Record<string, " + ts_of(type.element()) + ">";
    }
    return {}; // unreachable; kept for MSVC's control-path analysis
}

// "*/" -> "* SEP /"-free spelling, newlines -> spaces (api/CODEGEN.md).
std::string replace_all(std::string text_in, std::string_view from, std::string_view to) {
    std::string out = std::move(text_in);
    std::size_t pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
        out.replace(pos, from.size(), to);
        pos += to.size();
    }
    return out;
}

} // namespace

LoadResult load_document(std::string_view bytes, std::string_view origin) {
    Json::ParseResult parsed = Json::parse(bytes, origin);
    if (parsed.error.has_value())
        return {.document = Json(), .error = base::to_error(*parsed.error)};
    Json document = std::move(parsed.value);
    // `midday api dump --json` envelope tolerance: no format_version at the
    // top but an object under "api" -> descend (api/CODEGEN.md).
    if (document.is_object() && document.find("format_version") == nullptr) {
        const Json* payload = document.find("api");
        if (payload != nullptr && payload->is_object()) {
            Json inner = *payload;
            document = std::move(inner);
        }
    }
    if (auto error = api::check_document(document))
        return {.document = Json(), .error = std::move(error)};
    if (auto error = check_generation_model(document))
        return {.document = Json(), .error = std::move(error)};
    return {.document = std::move(document), .error = std::nullopt};
}

Outputs generate(const Json& document) {
    return Outputs{.dts = emit_dts(document),
                   .manifest = emit_manifest(document),
                   .docs = emit_docs(document),
                   .bindings = emit_bindings(document)};
}

std::vector<std::string> dts_shape_errors(std::string_view dts, const Json& document) {
    std::vector<std::string> errors;
    static constexpr std::string_view kTokens[] = {"TODO", "FIXME", "XXX", "PLACEHOLDER"};
    long depth = 0;
    std::size_t line_number = 0;
    for (std::size_t pos = 0; pos <= dts.size();) {
        const std::size_t end = dts.find('\n', pos);
        std::string_view line =
            dts.substr(pos, (end == std::string_view::npos ? dts.size() : end) - pos);
        ++line_number;
        while (!line.empty() && line.front() == ' ')
            line.remove_prefix(1);
        const bool comment = line.starts_with("//") || line.starts_with("/*");
        if (!comment) {
            for (const char c : line) {
                depth += (c == '{') - (c == '}');
                if (depth < 0)
                    errors.push_back("line " + std::to_string(line_number) + ": unbalanced '}'");
            }
            for (const std::string_view token : kTokens)
                if (line.find(token) != std::string_view::npos)
                    errors.push_back("line " + std::to_string(line_number) +
                                     ": unresolved-generation token '" + std::string(token) + "'");
        }
        if (depth < 0)
            break;
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }
    if (depth > 0)
        errors.push_back("unbalanced '{': " + std::to_string(depth) + " unclosed");

    auto need = [&dts, &errors](const std::string& fragment) {
        if (dts.find(fragment) == std::string_view::npos)
            errors.push_back("missing declaration fragment: " + fragment);
    };
    for (const Json& entry : detail::entries(document, "classes")) {
        need("interface " + pascal_case(detail::str(entry, "name")) + " ");
        need("\"" + detail::str(entry, "name") + "\":");
    }
    for (const Json& entry : detail::entries(document, "events")) {
        need("interface " + pascal_case(detail::str(entry, "name")) + "Event ");
        need("\"" + detail::str(entry, "name") + "\":");
    }
    for (const Json& entry : detail::entries(document, "functions"))
        need("function " + detail::str(entry, "name") + "(");
    for (const Json& entry : detail::entries(document, "verbs")) {
        need("interface " + pascal_case(detail::str(entry, "name")) + "VerbArgs ");
        need("\"" + detail::str(entry, "name") + "\":");
    }
    return errors;
}

std::string pascal_case(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool boundary = true;
    for (const char c : name) {
        if (c == '.' || c == '_' || c == '-') {
            boundary = true;
            continue;
        }
        out.push_back(boundary ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                               : c);
        boundary = false;
    }
    return out;
}

std::string ts_type(std::string_view spelling) {
    const std::optional<reflect::TypeDesc> parsed = reflect::TypeDesc::parse(spelling);
    if (!parsed.has_value()) { // pre-condition breach: validation walks first
        std::fprintf(stderr,
                     "codegen: fatal: ts_type on unvalidated spelling '%.*s'\n",
                     static_cast<int>(spelling.size()),
                     spelling.data());
        std::abort();
    }
    return ts_of(*parsed);
}

std::string jsdoc_escape(std::string_view doc) {
    return replace_all(replace_all(std::string(doc), "\n", " "), "*/", "*\\/");
}

std::string cell_escape(std::string_view doc) {
    return replace_all(replace_all(std::string(doc), "\n", " "), "|", "\\|");
}

} // namespace midday::codegen

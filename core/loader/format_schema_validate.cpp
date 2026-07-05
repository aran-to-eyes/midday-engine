// core/loader/format_schema_validate.cpp — format_schema.h's
// validate_document() engine: format-version gate, migration-chain
// application, unknown-key/required/type/enum checks, and (m1-scene-format)
// the recursive kObject/kArrayOfObject nested-shape checks. Split out of
// format_schema.cpp to hold the 500-line ratchet; loading (JSON ->
// FormatSchema) stays there.

#include "core/loader/format_schema.h"
#include "core/loader/parse_util.h"

#include <utility>

namespace midday::loader {

namespace {

using detail::err_node;
using detail::Parsed;

// Same shape check_format() enforces (missing / non-integer / unknown), but
// open-ended: `schema.current_version` is the ceiling, not a fixed constant,
// so an older-but-known version can migrate forward instead of refusing.
struct FormatGate {
    std::int64_t version = 0;
    std::optional<base::Error> error;
};

FormatGate gate_format(const FormatSchema& schema, const YamlNode& root, std::string_view file) {
    FormatGate out;
    if (!root.is_map()) {
        out.error =
            err_node("loader.bad_value", file, root, schema.name + " file must be a mapping");
        return out;
    }
    const YamlNode* format = root.find("format");
    if (format == nullptr) {
        out.error =
            err_node("loader.bad_format", file, root, schema.name + " file requires 'format: N'");
        return out;
    }
    Parsed<std::int64_t> version = detail::get_int(*format, file);
    if (version.error.has_value()) {
        out.error = std::move(version.error);
        return out;
    }
    if (version.value < 1 || version.value > schema.current_version) {
        out.error = err_node(
            "loader.bad_format",
            file,
            *format,
            "unknown " + schema.name + " format version " + std::to_string(version.value) +
                " (this engine reads up to format " + std::to_string(schema.current_version) + ")");
        return out;
    }
    out.version = version.value;
    return out;
}

const MigrationStep* find_step(const FormatSchema& schema, std::int64_t from_version) {
    for (const MigrationStep& step : schema.migrations)
        if (step.from_version == from_version)
            return &step;
    return nullptr;
}

void apply_ops(YamlNode& working, const std::vector<MigrationOp>& ops) {
    for (const MigrationOp& op : ops) {
        if (op.op != "rename_key")
            continue; // unreachable: load_format_schema rejects unknown ops
        for (YamlEntry& entry : working.map)
            if (entry.key == op.from) {
                entry.key = op.to;
                break;
            }
    }
}

// Migrates `working` in place from `authored` to `schema.current_version`;
// nullopt on success, "schema.no_migration_path" when the chain has a gap.
std::optional<base::Error> migrate(const FormatSchema& schema,
                                   YamlNode& working,
                                   std::int64_t authored,
                                   std::string_view file) {
    std::int64_t version = authored;
    while (version < schema.current_version) {
        const MigrationStep* step = find_step(schema, version);
        if (step == nullptr)
            return err_node("schema.no_migration_path",
                            file,
                            working,
                            schema.name + " format " + std::to_string(version) +
                                " has no registered migration toward format " +
                                std::to_string(schema.current_version));
        apply_ops(working, step->ops);
        version = step->to_version;
    }
    return std::nullopt;
}

std::string_view describe_kind(const base::Json& value) {
    if (value.is_null())
        return "null";
    if (value.is_bool())
        return "a bool";
    if (value.is_int())
        return "an int";
    if (value.is_double())
        return "a float";
    if (value.is_string())
        return "a string";
    if (value.is_array())
        return "an array";
    return "an object";
}

std::optional<base::Error>
check_field(const FieldSchema& field, const YamlNode& node, std::string_view file);

// A NESTED object's own contents (format_schema.h's "modest" extension):
// empty `fields` means opaque (the shape check above already confirmed
// map/seq; contents are the loader's job, never this engine's). Non-empty
// `fields` gets the SAME unknown-key + required + per-field checks the
// top-level document already runs (validate_document below) — reused, not
// duplicated.
std::optional<base::Error> check_object_contents(const std::vector<FieldSchema>& fields,
                                                 const YamlNode& node,
                                                 std::string_view file) {
    if (fields.empty())
        return std::nullopt;
    std::vector<std::string_view> allowed;
    allowed.reserve(fields.size());
    for (const FieldSchema& nested : fields)
        allowed.emplace_back(nested.name);
    if (auto error = detail::check_keys(node, file, allowed))
        return error;
    for (const FieldSchema& nested : fields) {
        const YamlNode* nested_node = node.find(nested.name);
        if (nested_node == nullptr) {
            if (nested.required)
                return detail::require_field(node, file, nested.name, "an object").error;
            continue;
        }
        if (auto error = check_field(nested, *nested_node, file))
            return error;
    }
    return std::nullopt;
}

std::optional<base::Error>
check_field(const FieldSchema& field, const YamlNode& node, std::string_view file) {
    if (field.kind == FieldKind::kObject) {
        if (!node.is_map())
            return err_node(
                "loader.bad_value", file, node, "field '" + field.name + "' expected an object");
        return check_object_contents(field.fields, node, file);
    }
    if (field.kind == FieldKind::kArrayOfObject) {
        if (!node.is_seq())
            return err_node(
                "loader.bad_value", file, node, "field '" + field.name + "' expected an array");
        for (const YamlNode& element : node.seq) {
            if (!field.fields.empty() && !element.is_map())
                return err_node("loader.bad_value",
                                file,
                                element,
                                "field '" + field.name + "' elements must be objects");
            if (auto error = check_object_contents(field.fields, element, file))
                return error;
        }
        return std::nullopt;
    }

    Parsed<base::Json> value = detail::yaml_to_json(node, file);
    if (value.error.has_value())
        return std::move(value.error);
    if (!field.type.accepts(value.value))
        return err_node("loader.bad_value",
                        file,
                        node,
                        "field '" + field.name + "' expected " + field.type.canonical() + ", got " +
                            std::string(describe_kind(value.value)));
    if (!field.enum_values.empty()) {
        const std::string& text = value.value.as_string();
        bool allowed = false;
        for (const std::string& candidate : field.enum_values)
            allowed = allowed || candidate == text;
        if (!allowed) {
            std::string list;
            for (const std::string& candidate : field.enum_values) {
                if (!list.empty())
                    list += ", ";
                list += candidate;
            }
            return err_node("schema.bad_enum",
                            file,
                            node,
                            "field '" + field.name + "' must be one of: " + list + " (got '" +
                                text + "')");
        }
    }
    return std::nullopt;
}

} // namespace

ValidateResult
validate_document(const FormatSchema& schema, const YamlNode& root, std::string_view file) {
    ValidateResult out;
    FormatGate gate = gate_format(schema, root, file);
    if (gate.error.has_value()) {
        out.error = std::move(gate.error);
        return out;
    }
    out.authored_version = gate.version;
    out.current_version = schema.current_version;

    YamlNode working = root; // migrations never mutate the caller's tree
    if (gate.version < schema.current_version) {
        if (auto error = migrate(schema, working, gate.version, file)) {
            out.error = std::move(error);
            return out;
        }
        out.migrated = true;
    }

    std::vector<std::string_view> allowed;
    allowed.reserve(schema.fields.size() + 1);
    allowed.emplace_back("format");
    for (const FieldSchema& field : schema.fields)
        allowed.emplace_back(field.name);
    if (auto error = detail::check_keys(working, file, allowed)) {
        out.error = std::move(error);
        return out;
    }

    for (const FieldSchema& field : schema.fields) {
        const YamlNode* node = working.find(field.name);
        if (node == nullptr) {
            if (field.required) {
                out.error =
                    detail::require_field(working, file, field.name, "a " + schema.name + " file")
                        .error;
                return out;
            }
            continue;
        }
        if (auto error = check_field(field, *node, file)) {
            out.error = std::move(error);
            return out;
        }
    }

    out.ok = true;
    return out;
}

} // namespace midday::loader

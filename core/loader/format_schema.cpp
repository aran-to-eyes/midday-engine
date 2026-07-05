// core/loader/format_schema.cpp — format_schema.h: format-entry JSON ->
// FormatSchema, and the generic validate_document() engine (format-version
// gate, migration-chain application, unknown-key/required/type/enum checks).

#include "core/loader/format_schema.h"

#include "core/loader/parse_util.h"

#include <utility>

namespace midday::loader {

const FieldSchema* FormatSchema::find_field(std::string_view field_name) const {
    for (const FieldSchema& field : fields)
        if (field.name == field_name)
            return &field;
    return nullptr;
}

namespace {

base::Error malformed(std::string_view origin, const std::string& what) {
    return base::Error{.code = "schema.malformed", .message = std::string(origin) + ": " + what};
}

// ---- loading ----------------------------------------------------------

// Mirrors detail::Parsed<T> (a plain value, meaningless when error is set) —
// never std::optional<T> for the value: dereferencing it right after an
// `error.has_value()` check on the SIBLING field is exactly the shape
// bugprone-unchecked-optional-access cannot correlate across two fields.
struct FieldLoad {
    FieldSchema field;
    std::optional<base::Error> error;
};

FieldLoad load_field(const base::Json& entry, std::string_view origin, std::size_t index) {
    FieldLoad out;
    if (!entry.is_object()) {
        out.error = malformed(origin, "fields[" + std::to_string(index) + "] is not an object");
        return out;
    }
    const base::Json* name = entry.find("name");
    if (name == nullptr || !name->is_string() || name->as_string().empty()) {
        out.error =
            malformed(origin, "fields[" + std::to_string(index) + "] needs a non-empty 'name'");
        return out;
    }
    const base::Json* type = entry.find("type");
    if (type == nullptr || !type->is_string()) {
        out.error = malformed(origin, "field '" + name->as_string() + "' needs a 'type'");
        return out;
    }
    std::optional<reflect::TypeDesc> parsed_type = reflect::TypeDesc::parse(type->as_string());
    if (!parsed_type.has_value()) {
        base::Error error{.code = "schema.unknown_type",
                          .message = std::string(origin) + ": field '" + name->as_string() +
                                     "' has unknown type '" + type->as_string() + "'"};
        out.error = std::move(error);
        return out;
    }

    FieldSchema field;
    field.name = name->as_string();
    field.type = std::move(*parsed_type);
    if (const base::Json* required = entry.find("required")) {
        if (!required->is_bool()) {
            out.error = malformed(origin, "field '" + field.name + "': 'required' must be a bool");
            return out;
        }
        field.required = required->as_bool();
    }
    if (const base::Json* enum_values = entry.find("enum")) {
        if (!enum_values->is_array()) {
            out.error = malformed(origin, "field '" + field.name + "': 'enum' must be an array");
            return out;
        }
        for (const base::Json& value : enum_values->elements()) {
            if (!value.is_string()) {
                out.error =
                    malformed(origin, "field '" + field.name + "': 'enum' entries must be strings");
                return out;
            }
            field.enum_values.push_back(value.as_string());
        }
        // An `enum` constrains string content; on any other type it is
        // nonsensical AND unsafe — validate_document()'s enum check reads the
        // value as a string, so a non-string field carrying an enum would make
        // the validator itself crash on a document that type-checks. Refuse the
        // schema at load, where the mistake actually is.
        if (field.type.kind() != reflect::TypeKind::kString &&
            field.type.kind() != reflect::TypeKind::kName) {
            out.error = malformed(origin,
                                  "field '" + field.name +
                                      "': 'enum' is only valid on string/name fields (got type '" +
                                      field.type.canonical() + "')");
            return out;
        }
    }
    out.field = std::move(field);
    return out;
}

// Same reasoning as FieldLoad above.
struct MigrationLoad {
    MigrationStep step;
    std::optional<base::Error> error;
};

MigrationLoad load_migration(const base::Json& entry, std::string_view origin, std::size_t index) {
    MigrationLoad out;
    const std::string where = "migrations[" + std::to_string(index) + "]";
    if (!entry.is_object()) {
        out.error = malformed(origin, where + " is not an object");
        return out;
    }
    const base::Json* from = entry.find("from");
    const base::Json* to = entry.find("to");
    const base::Json* ops = entry.find("ops");
    if (from == nullptr || !from->is_int() || to == nullptr || !to->is_int()) {
        out.error = malformed(origin, where + " needs integer 'from'/'to'");
        return out;
    }
    if (to->as_int() <= from->as_int()) {
        out.error = malformed(origin, where + ": 'to' must be greater than 'from'");
        return out;
    }
    if (ops == nullptr || !ops->is_array()) {
        out.error = malformed(origin, where + " needs an 'ops' array");
        return out;
    }
    MigrationStep step;
    step.from_version = from->as_int();
    step.to_version = to->as_int();
    for (std::size_t i = 0; i < ops->elements().size(); ++i) {
        const base::Json& op_json = ops->elements()[i];
        const std::string op_where = where + ".ops[" + std::to_string(i) + "]";
        if (!op_json.is_object()) {
            out.error = malformed(origin, op_where + " is not an object");
            return out;
        }
        const base::Json* op = op_json.find("op");
        if (op == nullptr || !op->is_string()) {
            out.error = malformed(origin, op_where + " needs a string 'op'");
            return out;
        }
        if (op->as_string() != "rename_key") {
            base::Error error{.code = "schema.unknown_migration_op",
                              .message = std::string(origin) + ": " + op_where +
                                         " has unknown op '" + op->as_string() + "'"};
            out.error = std::move(error);
            return out;
        }
        const base::Json* rename_from = op_json.find("from");
        const base::Json* rename_to = op_json.find("to");
        if (rename_from == nullptr || !rename_from->is_string() || rename_to == nullptr ||
            !rename_to->is_string()) {
            out.error = malformed(origin, op_where + " (rename_key) needs string 'from'/'to'");
            return out;
        }
        step.ops.push_back(MigrationOp{
            .op = "rename_key", .from = rename_from->as_string(), .to = rename_to->as_string()});
    }
    out.step = std::move(step);
    return out;
}

} // namespace

SchemaLoadResult load_format_schema(const base::Json& document, std::string_view origin) {
    SchemaLoadResult out;
    if (!document.is_object()) {
        out.error = malformed(origin, "format entry is not a JSON object");
        return out;
    }
    const base::Json* name = document.find("name");
    if (name == nullptr || !name->is_string() || name->as_string().empty()) {
        out.error = malformed(origin, "format entry needs a non-empty 'name'");
        return out;
    }
    const base::Json* current_version = document.find("current_version");
    if (current_version == nullptr || !current_version->is_int() || current_version->as_int() < 1) {
        out.error = malformed(origin, "format entry needs an integer 'current_version' >= 1");
        return out;
    }
    const base::Json* fields = document.find("fields");
    if (fields == nullptr || !fields->is_array()) {
        out.error = malformed(origin, "format entry needs a 'fields' array");
        return out;
    }

    FormatSchema schema;
    schema.name = name->as_string();
    schema.current_version = current_version->as_int();
    for (std::size_t i = 0; i < fields->elements().size(); ++i) {
        FieldLoad loaded = load_field(fields->elements()[i], origin, i);
        if (loaded.error.has_value()) {
            out.error = std::move(loaded.error);
            return out;
        }
        schema.fields.push_back(std::move(loaded.field));
    }
    if (const base::Json* migrations = document.find("migrations")) {
        if (!migrations->is_array()) {
            out.error = malformed(origin, "'migrations' must be an array");
            return out;
        }
        for (std::size_t i = 0; i < migrations->elements().size(); ++i) {
            MigrationLoad loaded = load_migration(migrations->elements()[i], origin, i);
            if (loaded.error.has_value()) {
                out.error = std::move(loaded.error);
                return out;
            }
            schema.migrations.push_back(std::move(loaded.step));
        }
    }
    out.schema = std::move(schema);
    return out;
}

// ---- validation ---------------------------------------------------------

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
check_field(const FieldSchema& field, const YamlNode& node, std::string_view file) {
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

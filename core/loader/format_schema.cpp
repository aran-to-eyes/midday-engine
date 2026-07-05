// core/loader/format_schema.cpp — format_schema.h: format-entry JSON ->
// FormatSchema (loading + the migration-registry data). The
// validate_document() engine itself (format-version gate, migration-chain
// application, unknown-key/required/type/enum + m1-scene-format's nested
// kObject/kArrayOfObject checks) lives in format_schema_validate.cpp — the
// 500-line ratchet split.

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

struct FieldsLoad {
    std::vector<FieldSchema> fields;
    std::optional<base::Error> error;
};

FieldsLoad load_fields_array(const base::Json& array_json, std::string_view origin);

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

    // kObject / kArrayOfObject: a NESTED shape (format_schema.h's "modest"
    // extension) — `type`/`enum` are meaningless here; `fields` (possibly
    // empty, meaning "opaque") describes the object/each array element.
    if (const base::Json* kind = entry.find("kind")) {
        if (!kind->is_string()) {
            out.error =
                malformed(origin, "field '" + name->as_string() + "': 'kind' must be a string");
            return out;
        }
        const bool is_object = kind->as_string() == "object";
        const bool is_array_of_object = kind->as_string() == "array_of_object";
        if (!is_object && !is_array_of_object) {
            out.error = malformed(origin,
                                  "field '" + name->as_string() + "': unknown kind '" +
                                      kind->as_string() + "' (object, array_of_object)");
            return out;
        }
        FieldSchema field;
        field.name = name->as_string();
        field.kind = is_object ? FieldKind::kObject : FieldKind::kArrayOfObject;
        if (const base::Json* required = entry.find("required")) {
            if (!required->is_bool()) {
                out.error =
                    malformed(origin, "field '" + field.name + "': 'required' must be a bool");
                return out;
            }
            field.required = required->as_bool();
        }
        const base::Json* nested = entry.find("fields");
        if (nested == nullptr || !nested->is_array()) {
            out.error = malformed(origin,
                                  "field '" + field.name + "': kind '" + kind->as_string() +
                                      "' needs a 'fields' array (possibly empty)");
            return out;
        }
        FieldsLoad nested_load = load_fields_array(*nested, origin);
        if (nested_load.error.has_value()) {
            out.error = std::move(nested_load.error);
            return out;
        }
        field.fields = std::move(nested_load.fields);
        out.field = std::move(field);
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

FieldsLoad load_fields_array(const base::Json& array_json, std::string_view origin) {
    FieldsLoad out;
    for (std::size_t i = 0; i < array_json.elements().size(); ++i) {
        FieldLoad loaded = load_field(array_json.elements()[i], origin, i);
        if (loaded.error.has_value()) {
            out.error = std::move(loaded.error);
            return out;
        }
        out.fields.push_back(std::move(loaded.field));
    }
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
    FieldsLoad top_fields = load_fields_array(*fields, origin);
    if (top_fields.error.has_value()) {
        out.error = std::move(top_fields.error);
        return out;
    }
    schema.fields = std::move(top_fields.fields);
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

} // namespace midday::loader

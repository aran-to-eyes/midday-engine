// `midday validate <file> (--schema <name> [--manifest <path>] |
// --schema-file <path>)` — the generic schema-driven validator (m1-strict-
// yaml, spec section 8's "validate-before-write" pillar): parses `file`
// through the strict loader, resolves a FormatSchema (core/loader/
// format_schema.h — the MECHANISM a schema_manifest.json `formats[]` entry
// carries), and runs the generic engine against it.
//
// Schema selection (the two ways a format entry reaches this verb):
//   --schema-file <path>   a standalone format-entry JSON document — the
//                          self-contained path testkit fixtures and future
//                          nodes' ad-hoc schemas use.
//   --schema <name> [--manifest <path>]
//                          look up `name` in <manifest>'s `formats[]` array
//                          (default api/schema_manifest.json). The committed
//                          manifest keeps `formats: []` until m1-scene-
//                          format appends real entries (manifest.ts:73) —
//                          until then every --schema lookup refuses with
//                          the (empty) list of known names, which is the
//                          honest answer, not a bug.
// Every failure here is the validation class (exit 3): an unreadable or
// malformed schema is DATA, exactly like `api diff`'s baseline document
// (api.cpp precedent) — usage errors (exit 2) are reserved for the flag
// combination itself being wrong.

#include "cli/verb.h"
#include "core/base/file_io.h"
#include "core/loader/format_schema.h"
#include "core/loader/yaml.h"

#include <optional>
#include <string>
#include <utility>

namespace midday::cli {
namespace {

constexpr std::string_view kDefaultManifest = "api/schema_manifest.json";

VerbOutcome usage(std::string code, std::string message) {
    VerbOutcome out;
    out.exit = Exit::Usage;
    out.error = Error{.code = std::move(code), .message = std::move(message)};
    return out;
}

VerbOutcome refuse(Error error) {
    VerbOutcome out;
    out.exit = Exit::Validation;
    out.error = std::move(error);
    return out;
}

struct SchemaResolution {
    std::optional<loader::FormatSchema> schema;
    VerbOutcome failure; // engaged iff !schema.has_value()
};

Json read_json_or_fail(const std::string& path, std::optional<VerbOutcome>& failure) {
    base::ReadFileResult file = base::read_file(path, "schema.io");
    if (file.error.has_value()) {
        failure = refuse(std::move(*file.error));
        return {};
    }
    Json::ParseResult parsed = Json::parse(file.bytes, path);
    if (parsed.error.has_value()) {
        failure = refuse(base::to_error(*parsed.error));
        return {};
    }
    return std::move(parsed.value);
}

SchemaResolution resolve_from_file(const std::string& path) {
    SchemaResolution out;
    std::optional<VerbOutcome> failure;
    Json document = read_json_or_fail(path, failure);
    if (failure.has_value()) {
        out.failure = std::move(*failure);
        return out;
    }
    loader::SchemaLoadResult loaded = loader::load_format_schema(document, path);
    if (loaded.error.has_value()) {
        out.failure = refuse(std::move(*loaded.error));
        return out;
    }
    out.schema = std::move(loaded.schema);
    return out;
}

SchemaResolution resolve_from_manifest(const std::string& name, const std::string& manifest_path) {
    SchemaResolution out;
    std::optional<VerbOutcome> failure;
    Json manifest = read_json_or_fail(manifest_path, failure);
    if (failure.has_value()) {
        out.failure = std::move(*failure);
        return out;
    }
    const Json* formats = manifest.is_object() ? manifest.find("formats") : nullptr;
    if (formats == nullptr || !formats->is_array()) {
        out.failure = refuse(Error{.code = "schema.malformed",
                                   .message = manifest_path + ": missing a 'formats' array"});
        return out;
    }
    for (const Json& entry : formats->elements()) {
        const Json* entry_name = entry.is_object() ? entry.find("name") : nullptr;
        if (entry_name != nullptr && entry_name->is_string() && entry_name->as_string() == name) {
            loader::SchemaLoadResult loaded = loader::load_format_schema(entry, manifest_path);
            if (loaded.error.has_value()) {
                out.failure = refuse(std::move(*loaded.error));
                return out;
            }
            out.schema = std::move(loaded.schema);
            return out;
        }
    }
    Error error{.code = "schema.unknown_format",
                .message = "no format '" + name + "' in " + manifest_path + "'s formats[] table"};
    Json known = Json::array();
    for (const Json& entry : formats->elements())
        if (const Json* entry_name = entry.is_object() ? entry.find("name") : nullptr;
            entry_name != nullptr && entry_name->is_string())
            known.push(*entry_name);
    error.details.set("known", std::move(known));
    out.failure = refuse(std::move(error));
    return out;
}

VerbOutcome validate_verb(const VerbArgs& args) {
    const bool by_name = args.present("schema");
    const bool by_file = args.present("schema-file");
    if (by_name == by_file)
        return usage("usage.bad_schema_selector",
                     "exactly one of --schema <name> or --schema-file <path> is required");
    if (!by_name && args.present("manifest"))
        return usage("usage.unexpected_flag", "--manifest only applies together with --schema");

    SchemaResolution resolution =
        by_file ? resolve_from_file(args.get_string("schema-file"))
                : resolve_from_manifest(args.get_string("schema"),
                                        args.present("manifest") ? args.get_string("manifest")
                                                                 : std::string(kDefaultManifest));
    if (!resolution.schema.has_value())
        return std::move(resolution.failure);

    const std::string& path = args.get_string("file");
    loader::YamlParseResult parsed = loader::parse_yaml_file(path);
    if (parsed.error.has_value())
        return refuse(std::move(*parsed.error));

    loader::ValidateResult result =
        loader::validate_document(*resolution.schema, parsed.root, path);
    if (result.error.has_value())
        return refuse(std::move(*result.error));

    VerbOutcome out;
    out.payload.set("file", path);
    out.payload.set("schema", resolution.schema->name);
    Json version = Json::object();
    version.set("authored", result.authored_version);
    version.set("current", result.current_version);
    version.set("migrated", result.migrated);
    out.payload.set("format_version", std::move(version));
    out.human =
        path + ": valid (" + resolution.schema->name + ", format " +
        std::to_string(result.current_version) +
        (result.migrated ? " — migrated from " + std::to_string(result.authored_version) : "") +
        ")";
    return out;
}

constexpr FlagSpec kFlags[] = {
    {.name = "schema",
     .type = "name",
     .doc = "format name to look up in the schema manifest's formats[] table"},
    {.name = "manifest",
     .type = "string",
     .doc = "schema manifest path, used with --schema (default: api/schema_manifest.json)"},
    {.name = "schema-file",
     .type = "string",
     .doc = "a standalone format-entry JSON document (bypasses the manifest)"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "file", .type = "string", .doc = "the strict-YAML file to validate"},
};

} // namespace

const VerbSpec& validate_spec() {
    static const VerbSpec spec{
        .name = "validate",
        .summary = "validate a strict-YAML file against a schema_manifest.json format entry",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &validate_verb,
    };
    return spec;
}

} // namespace midday::cli

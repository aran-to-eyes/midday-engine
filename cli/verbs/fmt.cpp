// `midday fmt <file> [--write | --check]` — the canonical strict-YAML
// formatter (m1-strict-yaml): parses through the strict loader
// (core/loader/yaml.h) and re-serializes with core/loader/yaml_emit.h's ONE
// deterministic rendering. Schema-agnostic by design (spec section 8's
// "canonical YAML on disk" applies to every text format alike, present or
// future) — `midday fmt` never needs to know whether the file is a scene, a
// machine, or a format nobody has invented yet.
//
// Modes: default prints the canonical text (stdout in human mode, `.yaml`
// payload field in --json mode, mirroring `api dump`'s no-flag behavior);
// --write rewrites the file in place and reports whether it changed;
// --check never writes and fails (exit 1, `fmt.not_canonical`) when the
// file would change — the clang-format --dry-run --Werror precedent this
// repo's own gate already uses. Malformed/refused YAML is the validation
// class (exit 3): the same yaml.parse/yaml.strict diagnostics `midday
// validate` and `midday run` already surface.

#include "cli/verb.h"
#include "core/base/file_io.h"
#include "core/loader/yaml.h"
#include "core/loader/yaml_emit.h"

#include <optional>
#include <string>
#include <utility>

namespace midday::cli {
namespace {

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

VerbOutcome fmt_verb(const VerbArgs& args) {
    const bool write = args.get_bool("write");
    const bool check = args.get_bool("check");
    if (write && check)
        return usage("usage.conflicting_flags", "--write and --check are mutually exclusive");

    const std::string& path = args.get_string("file");
    base::ReadFileResult file = base::read_file(path, "loader.io");
    if (file.error.has_value())
        return refuse(std::move(*file.error));

    loader::YamlParseResult parsed = loader::parse_yaml(file.bytes, path);
    if (parsed.error.has_value())
        return refuse(std::move(*parsed.error));

    const std::string canonical = loader::emit_yaml(parsed.root);
    const bool already_canonical = canonical == file.bytes;

    VerbOutcome out;
    out.payload.set("file", path);
    out.payload.set("canonical", already_canonical);

    if (check) {
        if (!already_canonical) {
            out.exit = Exit::Failure;
            out.error = Error{.code = "fmt.not_canonical",
                              .message = path + " is not in canonical form (run: midday fmt " +
                                         path + " --write)"};
        }
        out.human = already_canonical ? path + ": canonical" : path + ": would reformat";
        return out;
    }

    if (write) {
        if (!already_canonical) {
            if (auto error = base::write_file(path, canonical, "fmt.io"))
                return refuse(std::move(*error));
        }
        out.payload.set("changed", !already_canonical);
        out.human = already_canonical ? path + ": already canonical" : path + ": reformatted";
        return out;
    }

    out.payload.set("yaml", canonical);
    out.human = canonical;
    return out;
}

constexpr FlagSpec kFlags[] = {
    {.name = "write", .type = "bool", .doc = "rewrite the file in place with its canonical form"},
    {.name = "check",
     .type = "bool",
     .doc = "exit 1 without writing if the file is not already canonical"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "file", .type = "string", .doc = "the strict-YAML file to canonicalize"},
};

} // namespace

const VerbSpec& fmt_spec() {
    static const VerbSpec spec{
        .name = "fmt",
        .summary = "canonicalize a strict-YAML file (schema-agnostic; see: midday validate)",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &fmt_verb,
    };
    return spec;
}

} // namespace midday::cli

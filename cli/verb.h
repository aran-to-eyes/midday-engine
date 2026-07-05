// cli/verb.h — the verb framework (m0-cli-framework). Permanent shape: one
// function per verb, envelope-only output. A verb DECLARES its interface
// (flags/positionals typed with reflect TypeDesc spellings); the framework
// parses and validates argv against that schema BEFORE the verb runs — a verb
// never sees raw arguments and never emits its own usage errors. Help (human
// and --json) is GENERATED from the same metadata (cli/help.h). Adding a verb
// is one file under cli/verbs/ plus one manifest line in
// cli/verbs/registry.cpp (D-BUILD-037); the framework itself never changes.

#pragma once

#include "cli/envelope.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::cli {

// One --flag. `type` is a reflect TypeDesc canonical spelling restricted to
// what a command line can carry: bool | int | float | string | name.
// bool flags are presence switches (--x, or explicit --x=true/false): never
// required, never defaulted — absence means false. Numeric values use the
// strict core JSON number grammar (D-BUILD-039); string/name values take the
// token verbatim.
struct FlagSpec {
    std::string_view name;              // long name; spelled --<name>
    std::string_view type;              // TypeDesc spelling (see above)
    std::string_view doc;               // one-line help text
    const char* default_text = nullptr; // literal default (unquoted); nullptr = none
    bool required = false;
};

// One positional argument: int | float | string | name (no bool positionals —
// presence semantics need a flag). Optional positionals follow required ones;
// a variadic positional must be last and collects every remaining argument.
struct PositionalSpec {
    std::string_view name;
    std::string_view type;
    std::string_view doc;
    bool required = true;
    bool variadic = false;
};

class VerbArgs;

struct VerbOutcome {
    Exit exit = Exit::Ok;
    Json payload = Json::object(); // verb-specific top-level envelope fields
    std::optional<Error> error;    // required by the schema whenever exit != Ok
    std::string human;             // summary for non-JSON mode
};

using VerbFn = VerbOutcome (*)(const VerbArgs&);

// Declarative verb metadata. Declaration order of flags/positionals is the
// canonical order in help, schemas, and engine_api.json.
struct VerbSpec {
    std::string_view name;
    std::string_view summary;
    std::span<const FlagSpec> flags = {};
    std::span<const PositionalSpec> positionals = {};
    VerbFn run = nullptr;
};

// Typed, framework-validated arguments. Every accessor addresses a DECLARED
// flag/positional by name; misuse (undeclared name, wrong-type accessor,
// reading an absent value with no default) is a programming error and aborts
// loudly — user input can never reach those paths because parsing already
// rejected it with a structured usage error.
class VerbArgs {
public:
    bool json = false; // --json seen (framework global, accepted anywhere)

    [[nodiscard]] bool present(std::string_view name) const;
    [[nodiscard]] bool get_bool(std::string_view name) const; // absent bool flag = false
    [[nodiscard]] std::int64_t get_int(std::string_view name) const;
    [[nodiscard]] double get_float(std::string_view name) const;
    [[nodiscard]] const std::string& get_string(std::string_view name) const; // string | name
    [[nodiscard]] const Json::Array& get_list(std::string_view name) const;   // variadic tail

private:
    friend struct ArgAssembler; // parse-time construction (cli/parse.cpp)
    [[nodiscard]] const Json* find_value(std::string_view name) const;

    const VerbSpec* spec_ = nullptr;
    std::vector<std::pair<std::string_view, Json>> values_;
};

// Registry. The manifest lives in cli/verbs/registry.cpp; it is validated
// once on first access and a malformed spec aborts loudly (reflect
// D-BUILD-023 precedent). Manifest order = deterministic output order.
std::span<const VerbSpec* const> verbs();
const VerbSpec* find_verb(std::string_view name);
VerbOutcome usage_unknown_verb(std::string_view name); // exit 2 + verb-list payload

// Framework-owned global flags (--json, --help), shown in every verb's help.
std::span<const FlagSpec> global_flags();

// Phase 1: split argv into pre-verb globals / verb / raw verb arguments.
// Only global flags may precede the verb.
struct Invocation {
    std::string_view verb; // empty when no verb token was given
    bool json = false;
    bool help = false;
    std::vector<std::string_view> rest; // tokens after the verb, verbatim
    std::optional<Error> usage;         // pre-verb usage error (exit 2)
};

Invocation split_invocation(int argc, const char* const* argv);

// Phase 2: parse and validate the verb's arguments against its schema.
// Unknown flag / missing required / type mismatch come back as structured
// usage.* Errors (exit 2) and the verb never runs. --help anywhere wins over
// usage errors so help stays reachable from a half-typed command line.
struct ParsedArgs {
    VerbArgs args;
    bool help = false;
    std::optional<Error> usage;
};

ParsedArgs parse_verb_args(const VerbSpec& spec, const Invocation& inv);

// Registration-time schema validation (spec.* codes). The registry aborts on
// any error; exposed separately for the cli.spec tests.
std::optional<Error> validate_spec(const VerbSpec& spec);

// Text -> typed JSON value for a TypeDesc spelling (argv tokens, flag
// defaults). nullopt when the text does not inhabit the type.
std::optional<Json> arg_value_from_text(std::string_view type, std::string_view text);

// Per-verb specs — one translation unit per verb under cli/verbs/.
const VerbSpec& version_spec();
const VerbSpec& selftest_spec();
const VerbSpec& help_spec();
const VerbSpec& api_spec();
const VerbSpec& script_spec();
const VerbSpec& run_spec();
const VerbSpec& journal_spec();
const VerbSpec& rhi_spec();
const VerbSpec& shot_spec();
const VerbSpec& validate_spec();
const VerbSpec& fmt_spec();
const VerbSpec& check_spec();
const VerbSpec& mv_spec();
const VerbSpec& new_spec();

} // namespace midday::cli

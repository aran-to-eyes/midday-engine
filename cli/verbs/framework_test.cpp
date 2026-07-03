// cli.* framework tests — written first (test-first): schema validation
// errors, typed flag access, help-generation determinism, and the exit-code
// mapping including the validation class 3 (distinct from usage 2).

#include "cli/help.h"
#include "cli/verb.h"
#include "core/expr/diag.h"
#include "doctest/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <initializer_list>
#include <ostream>
#include <string_view>

using midday::cli::Error;
using midday::cli::Exit;
using midday::cli::FlagSpec;
using midday::cli::Invocation;
using midday::cli::Json;
using midday::cli::ParsedArgs;
using midday::cli::PositionalSpec;
using midday::cli::VerbArgs;
using midday::cli::VerbOutcome;
using midday::cli::VerbSpec;
using midday::testkit::unwrap;

// Exit-code discipline: the validation class (3) is pinned to the expr
// compile-diagnostic precedent — one mapping tree-wide, checked at compile.
static_assert(midday::expr::kCompileDiagExitCode == static_cast<int>(Exit::Validation));
static_assert(static_cast<int>(Exit::Usage) == 2 && static_cast<int>(Exit::Validation) == 3);

namespace {

VerbOutcome probe_run(const VerbArgs&) {
    return {};
}

constexpr FlagSpec kProbeFlags[] = {
    {.name = "count", .type = "int", .doc = "an int flag", .required = true},
    {.name = "ratio", .type = "float", .doc = "a float flag", .default_text = "0.5"},
    {.name = "tag", .type = "name", .doc = "a name flag"},
    {.name = "verbose", .type = "bool", .doc = "a bool switch"},
    {.name = "label", .type = "string", .doc = "a string flag", .default_text = "none"},
};

constexpr PositionalSpec kProbePositionals[] = {
    {.name = "input", .type = "string", .doc = "required input"},
    {.name = "extras",
     .type = "string",
     .doc = "variadic tail",
     .required = false,
     .variadic = true},
};

constexpr VerbSpec kProbe{.name = "probe",
                          .summary = "framework test probe",
                          .flags = kProbeFlags,
                          .positionals = kProbePositionals,
                          .run = &probe_run};

constexpr VerbSpec kBare{.name = "bare", .summary = "no arguments at all", .run = &probe_run};

ParsedArgs parse(const VerbSpec& spec, std::initializer_list<std::string_view> tokens) {
    Invocation inv;
    inv.verb = spec.name;
    inv.rest.assign(tokens.begin(), tokens.end());
    return midday::cli::parse_verb_args(spec, inv);
}

} // namespace

TEST_CASE("cli.parse: typed access, defaults, and presence") {
    ParsedArgs p = parse(kProbe, {"--count", "7", "--verbose", "in.txt"});
    REQUIRE(!p.usage.has_value());
    CHECK(!p.help);
    CHECK(p.args.get_int("count") == 7);
    CHECK(p.args.get_bool("verbose"));
    CHECK(p.args.get_float("ratio") == 0.5);       // default filled by the framework
    CHECK(p.args.get_string("label") == "none");   // default filled by the framework
    CHECK(!p.args.present("tag"));                 // optional, no default, not given
    CHECK(p.args.get_string("input") == "in.txt"); // positional
    CHECK(p.args.get_list("extras").empty());      // variadic, none given
    CHECK(!p.args.json);
}

TEST_CASE("cli.parse: --flag=value form, variadic tail, --json after the verb") {
    ParsedArgs p = parse(kProbe, {"--count=1", "--tag=boss", "a", "b", "c", "--json"});
    REQUIRE(!p.usage.has_value());
    CHECK(p.args.json);
    CHECK(p.args.get_string("tag") == "boss");
    CHECK(p.args.get_string("input") == "a");
    const Json::Array& extras = p.args.get_list("extras");
    REQUIRE(extras.size() == 2);
    CHECK(extras[0].as_string() == "b");
    CHECK(extras[1].as_string() == "c");
}

TEST_CASE("cli.parse: int flag accepts an int literal as float, floats stay exact") {
    ParsedArgs p = parse(kProbe, {"--count", "3", "--ratio", "2", "x"});
    REQUIRE(!p.usage.has_value());
    CHECK(p.args.get_float("ratio") == 2.0); // int literal coerces where a float is declared
}

TEST_CASE("cli.parse: unknown flag is usage.unknown_flag with the known-flag list") {
    ParsedArgs p = parse(kProbe, {"--count", "1", "--nope", "x", "in"});
    CHECK(unwrap(p.usage).code == "usage.unknown_flag");
    CHECK(unwrap(p.usage).details.find("flag")->as_string() == "nope");
    // Deterministic: declaration order, then the framework globals.
    CHECK(unwrap(p.usage).details.find("known")->dump() ==
          "[\"count\",\"ratio\",\"tag\",\"verbose\",\"label\",\"json\",\"help\"]");
}

TEST_CASE("cli.parse: missing required flags aggregate into one usage.missing_flag") {
    ParsedArgs p = parse(kProbe, {"in"});
    CHECK(unwrap(p.usage).code == "usage.missing_flag");
    CHECK(unwrap(p.usage).details.find("missing")->dump() == "[\"count\"]");
}

TEST_CASE("cli.parse: type mismatch is usage.invalid_flag_value with structured details") {
    ParsedArgs p = parse(kProbe, {"--count", "seven", "in"});
    CHECK(unwrap(p.usage).code == "usage.invalid_flag_value");
    CHECK(unwrap(p.usage).details.find("flag")->as_string() == "count");
    CHECK(unwrap(p.usage).details.find("type")->as_string() == "int");
    CHECK(unwrap(p.usage).details.find("value")->as_string() == "seven");

    // Strict JSON number grammar: a float literal does not inhabit int.
    ParsedArgs q = parse(kProbe, {"--count", "3.5", "in"});
    CHECK(unwrap(q.usage).code == "usage.invalid_flag_value");

    // Bool switches only take explicit true/false.
    ParsedArgs r = parse(kProbe, {"--count", "1", "--verbose=x", "in"});
    CHECK(unwrap(r.usage).code == "usage.invalid_flag_value");
}

TEST_CASE("cli.parse: value flag at end of line is usage.missing_flag_value") {
    ParsedArgs p = parse(kProbe, {"in", "--count"});
    CHECK(unwrap(p.usage).code == "usage.missing_flag_value");
    CHECK(unwrap(p.usage).details.find("flag")->as_string() == "count");
}

TEST_CASE("cli.parse: duplicate value flag rejected, repeated bool switch idempotent") {
    ParsedArgs p = parse(kProbe, {"--count", "1", "--count", "2", "in"});
    CHECK(unwrap(p.usage).code == "usage.duplicate_flag");

    ParsedArgs q = parse(kProbe, {"--count", "1", "--verbose", "--verbose", "in"});
    REQUIRE(!q.usage.has_value());
    CHECK(q.args.get_bool("verbose"));
}

TEST_CASE("cli.parse: positional completeness and overflow") {
    ParsedArgs p = parse(kProbe, {"--count", "1"});
    CHECK(unwrap(p.usage).code == "usage.missing_argument");
    CHECK(unwrap(p.usage).details.find("argument")->as_string() == "input");

    ParsedArgs q = parse(kBare, {"stray"});
    CHECK(unwrap(q.usage).code == "usage.unexpected_argument");
    CHECK(unwrap(q.usage).details.find("argument")->as_string() == "stray");
}

TEST_CASE("cli.parse: bad int positional is usage.invalid_argument_value") {
    static constexpr PositionalSpec kNumPos[] = {
        {.name = "n", .type = "int", .doc = "a number"},
    };
    static constexpr VerbSpec kNums{.name = "nums",
                                    .summary = "int positional probe",
                                    .positionals = kNumPos,
                                    .run = &probe_run};
    ParsedArgs p = parse(kNums, {"twelve"});
    CHECK(unwrap(p.usage).code == "usage.invalid_argument_value");
    CHECK(unwrap(p.usage).details.find("argument")->as_string() == "n");
}

TEST_CASE("cli.parse: --help short-circuits validation and survives usage errors") {
    ParsedArgs p = parse(kProbe, {"--help"});
    CHECK(p.help);
    CHECK(!p.usage.has_value()); // no missing-required errors when help is requested

    // --help stays reachable even after a broken token.
    ParsedArgs q = parse(kProbe, {"--nope", "--help"});
    CHECK(q.help);
}

TEST_CASE("cli.split: global flags before the verb, verbatim rest after it") {
    const char* argv1[] = {"midday", "--json", "version"};
    Invocation a = midday::cli::split_invocation(3, argv1);
    CHECK(!a.usage.has_value());
    CHECK(a.json);
    CHECK(a.verb == "version");
    CHECK(a.rest.empty());

    const char* argv2[] = {"midday", "selftest", "--filter", "cli.*"};
    Invocation b = midday::cli::split_invocation(4, argv2);
    CHECK(b.verb == "selftest");
    REQUIRE(b.rest.size() == 2);
    CHECK(b.rest[0] == "--filter");
    CHECK(b.rest[1] == "cli.*");

    const char* argv3[] = {"midday", "--frob", "version"};
    Invocation c = midday::cli::split_invocation(3, argv3);
    CHECK(unwrap(c.usage).code == "usage.unknown_flag");
}

TEST_CASE("cli.spec: registration-time schema validation rejects malformed specs") {
    auto code_of = [](const VerbSpec& spec) {
        const std::optional<Error> error = midday::cli::validate_spec(spec);
        return error ? error->code : std::string();
    };

    constexpr FlagSpec kBadType[] = {{.name = "v", .type = "vec3", .doc = "not a CLI type"}};
    CHECK(code_of({.name = "x", .summary = "s", .flags = kBadType, .run = &probe_run}) ==
          "spec.bad_type");

    constexpr FlagSpec kBoolRequired[] = {
        {.name = "b", .type = "bool", .doc = "d", .required = true}};
    CHECK(code_of({.name = "x", .summary = "s", .flags = kBoolRequired, .run = &probe_run}) ==
          "spec.bool_flag_required");

    constexpr FlagSpec kRequiredDefault[] = {
        {.name = "n", .type = "int", .doc = "d", .default_text = "1", .required = true}};
    CHECK(code_of({.name = "x", .summary = "s", .flags = kRequiredDefault, .run = &probe_run}) ==
          "spec.required_has_default");

    constexpr FlagSpec kBadDefault[] = {
        {.name = "n", .type = "int", .doc = "d", .default_text = "many"}};
    CHECK(code_of({.name = "x", .summary = "s", .flags = kBadDefault, .run = &probe_run}) ==
          "spec.bad_default");

    constexpr FlagSpec kReserved[] = {{.name = "json", .type = "int", .doc = "d"}};
    CHECK(code_of({.name = "x", .summary = "s", .flags = kReserved, .run = &probe_run}) ==
          "spec.duplicate_name");

    constexpr PositionalSpec kBoolPos[] = {{.name = "b", .type = "bool", .doc = "d"}};
    CHECK(code_of({.name = "x", .summary = "s", .positionals = kBoolPos, .run = &probe_run}) ==
          "spec.bad_type");

    constexpr PositionalSpec kOrder[] = {
        {.name = "a", .type = "string", .doc = "d", .required = false},
        {.name = "b", .type = "string", .doc = "d"},
    };
    CHECK(code_of({.name = "x", .summary = "s", .positionals = kOrder, .run = &probe_run}) ==
          "spec.positional_order");

    constexpr PositionalSpec kVariadic[] = {
        {.name = "a", .type = "string", .doc = "d", .required = false, .variadic = true},
        {.name = "b", .type = "string", .doc = "d"},
    };
    CHECK(code_of({.name = "x", .summary = "s", .positionals = kVariadic, .run = &probe_run}) ==
          "spec.variadic_not_last");

    CHECK(code_of({.name = "x", .summary = "s"}) == "spec.no_run");
    CHECK(!midday::cli::validate_spec(kProbe).has_value());
}

TEST_CASE("cli.help: schemas are generated in declaration order, byte-deterministic") {
    const Json schema = midday::cli::verb_schema(kProbe);
    const Json::Array& flags = schema.find("flags")->elements();
    REQUIRE(flags.size() == 5);
    CHECK(flags[0].find("name")->as_string() == "count");
    CHECK(flags[1].find("name")->as_string() == "ratio");
    CHECK(flags[2].find("name")->as_string() == "tag");
    CHECK(flags[3].find("name")->as_string() == "verbose");
    CHECK(flags[4].find("name")->as_string() == "label");
    CHECK(flags[1].find("default")->dump() == "0.5"); // typed default, not text
    CHECK(schema.dump() == midday::cli::verb_schema(kProbe).dump());

    // The version verb's schema is byte-pinned: this exact JSON is what
    // m0-api-json embeds in engine_api.json.
    CHECK(midday::cli::verb_schema(midday::cli::version_spec()).dump() ==
          "{\"name\":\"version\",\"summary\":\"print engine name, version, and build info\","
          "\"flags\":[],\"positionals\":[]}");
}

TEST_CASE("cli.help: overview lists verbs in manifest order with global flags") {
    const VerbOutcome out = midday::cli::help_overview();
    CHECK(out.exit == Exit::Ok);
    const Json::Array& listed = out.payload.find("verbs")->elements();
    REQUIRE(listed.size() == midday::cli::verbs().size());
    CHECK(listed[0].find("name")->as_string() == "version");
    CHECK(listed[1].find("name")->as_string() == "selftest");
    CHECK(listed[2].find("name")->as_string() == "help");
    REQUIRE(out.payload.find("globals") != nullptr);
    CHECK(out.payload.find("globals")->elements().size() == 2);
    CHECK(out.payload.dump() == midday::cli::help_overview().payload.dump());
    CHECK(out.human == midday::cli::help_overview().human);
    CHECK(out.human.find("usage: midday <verb>") != std::string::npos);
}

TEST_CASE("cli.help: the help verb resolves targets through the registry") {
    Invocation inv;
    inv.verb = "help";
    inv.rest = {"selftest"};
    ParsedArgs p = midday::cli::parse_verb_args(midday::cli::help_spec(), inv);
    REQUIRE(!p.usage.has_value());
    const VerbOutcome out = midday::cli::help_spec().run(p.args);
    CHECK(out.exit == Exit::Ok);
    CHECK(out.payload.find("name")->as_string() == "selftest");
    CHECK(out.payload.find("flags")->elements().size() == 1);
    CHECK(out.human.find("usage: midday selftest [--filter <string>] [--json]") !=
          std::string::npos);

    inv.rest = {"frobnicate"};
    ParsedArgs q = midday::cli::parse_verb_args(midday::cli::help_spec(), inv);
    const VerbOutcome bad = midday::cli::help_spec().run(q.args);
    CHECK(bad.exit == Exit::Usage);
    CHECK(unwrap(bad.error).code == "usage.unknown_verb");
}

TEST_CASE("cli.exit: envelope carries the validation class distinctly from usage") {
    Error err;
    err.code = "expr.type_mismatch";
    err.message = "compile diagnostic";
    const Json validation =
        midday::cli::make_envelope("check", Exit::Validation, Json::object(), &err);
    CHECK(validation.find("exit_code")->dump() == "3");
    CHECK(validation.find("ok")->dump() == "false");

    const Json usage = midday::cli::make_envelope("check", Exit::Usage, Json::object(), &err);
    CHECK(usage.find("exit_code")->dump() == "2");
}

TEST_CASE("cli.verbs: registry validates and resolves the manifest") {
    REQUIRE(midday::cli::verbs().size() == 3);
    for (const VerbSpec* spec : midday::cli::verbs())
        CHECK(!midday::cli::validate_spec(*spec).has_value());
    CHECK(midday::cli::find_verb("version") == &midday::cli::version_spec());
    CHECK(midday::cli::find_verb("help") == &midday::cli::help_spec());
    CHECK(midday::cli::find_verb("frobnicate") == nullptr);

    const VerbOutcome unknown = midday::cli::usage_unknown_verb("frobnicate");
    CHECK(unknown.exit == Exit::Usage);
    CHECK(unwrap(unknown.error).code == "usage.unknown_verb");
    CHECK(unknown.payload.find("verbs")->dump() == "[\"version\",\"selftest\",\"help\"]");
}

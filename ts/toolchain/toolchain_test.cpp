// script.toolchain / script.lint / script.cache doctests — the vendored
// TypeScript compiler on QuickJS: fixtures transpile+run, type errors and
// lint hits come back with file:line, the content-hash cache reports zero
// re-transpiles on the second run, and cached bytes are byte-stable
// (two independent builds compared, never a self-diff).

#include "core/base/file_io.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"
#include "ts/toolchain/toolchain.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

using midday::base::Json;
using midday::script::BuildOutcome;
using midday::script::CheckOutcome;
using midday::script::Diagnostic;
using midday::script::ExtractOutcome;
using midday::script::ScriptRuntime;
using midday::script::Toolchain;
using midday::script::ToolchainConfig;
using midday::testkit::TempDir;

constexpr const char* kHello = "testkit/fixtures/ts/hello.ts";
constexpr const char* kTypeError = "testkit/fixtures/ts/type_error.ts";
constexpr const char* kRuntimeThrow = "testkit/fixtures/ts/runtime_throw.ts";
constexpr const char* kLint = "testkit/fixtures/ts/lint_violations.ts";
constexpr const char* kEngineTypes = "testkit/fixtures/ts/engine_types.ts";

// Fresh, gitignored cache dir per test (verify.sh runs from the repo root).
ToolchainConfig fresh_config(const std::string& name) {
    ToolchainConfig config;
    config.cache_dir = ".midday-cache/selftest/" + name;
    std::filesystem::remove_all(config.cache_dir);
    return config;
}

// 1-based line of the first line containing `needle` — fixtures stay
// self-describing instead of tests pinning brittle literal line numbers.
std::int64_t line_of(const char* path, const std::string& needle) {
    midday::base::ReadFileResult file = midday::base::read_file(path, "test.io");
    REQUIRE_FALSE(file.error.has_value());
    std::int64_t line = 1;
    std::size_t start = 0;
    while (start < file.bytes.size()) {
        std::size_t end = file.bytes.find('\n', start);
        if (end == std::string::npos)
            end = file.bytes.size();
        if (file.bytes.substr(start, end - start).find(needle) != std::string::npos)
            return line;
        start = end + 1;
        ++line;
    }
    FAIL((std::string("fixture ") + path + " does not contain '" + needle + "'"));
    return 0;
}

const Diagnostic* find_lint(const std::vector<Diagnostic>& diagnostics, const std::string& what) {
    for (const Diagnostic& diag : diagnostics)
        if (diag.kind == "lint" && diag.message.find(what) != std::string::npos)
            return &diag;
    return nullptr;
}

} // namespace

TEST_CASE("script.toolchain: ts_hello typechecks, transpiles, runs, and hits the host hook") {
    Toolchain toolchain(fresh_config("hello"));
    ScriptRuntime runtime;
    std::int64_t emitted = 0;
    runtime.register_host_fn("__midday_emit", [&](const Json::Array& args) {
        midday::script::HostResult result;
        REQUIRE(args.size() == 1);
        emitted = args[0].as_int();
        return result;
    });
    Toolchain::LoadOutcome loaded = toolchain.load_module(runtime, kHello);
    REQUIRE_MESSAGE(!loaded.error.has_value(),
                    (loaded.error ? loaded.error->message : std::string()));
    CHECK(emitted == 42);
    CHECK(toolchain.stats().transpiled == 1);
}

TEST_CASE("script.toolchain: game code typechecks against the generated engine.d.ts") {
    Toolchain toolchain(fresh_config("engine_types"));
    CheckOutcome outcome = toolchain.check(kEngineTypes);
    REQUIRE_MESSAGE(!outcome.failure.has_value(),
                    (outcome.failure ? outcome.failure->message : std::string()));
    for (const Diagnostic& diag : outcome.diagnostics)
        MESSAGE(diag.to_string());
    CHECK(outcome.ok);
}

TEST_CASE(
    "script.toolchain: warden's Health component extracts its @field schema, order-preserving") {
    Toolchain toolchain(fresh_config("extract_health"));
    ExtractOutcome outcome = toolchain.extract("examples/warden/components/health.ts");
    REQUIRE_MESSAGE(!outcome.check.failure.has_value(),
                    (outcome.check.failure ? outcome.check.failure->message : std::string()));
    for (const Diagnostic& diag : outcome.check.diagnostics)
        MESSAGE(diag.to_string());
    REQUIRE(outcome.check.ok);
    REQUIRE(outcome.components.is_array());
    REQUIRE(outcome.components.elements().size() == 1);
    const Json& health = outcome.components.elements()[0];
    CHECK(health.find("name")->as_string() == "Health");
    CHECK(health.find("file")->as_string() == "examples/warden/components/health.ts");

    const Json& fields = *health.find("fields");
    REQUIRE(fields.elements().size() == 2);
    const Json& max_field = fields.elements()[0];
    CHECK(max_field.find("name")->as_string() == "max");
    CHECK(max_field.find("type")->as_string() == "float");
    CHECK(max_field.find("default")->as_int() == 100);
    CHECK(max_field.find("min")->as_int() == 0); // @field({min: 0}) — decorator arg, read as-is
    const Json& value_field = fields.elements()[1];
    CHECK(value_field.find("name")->as_string() == "value");
    CHECK(value_field.find("type")->as_string() == "float");
    CHECK(value_field.find("default")->as_int() == 100);
    CHECK(value_field.find("min") == nullptr); // bare @field() carries no constraint args

    const Json& methods = *health.find("methods");
    REQUIRE(methods.elements().size() == 1);
    const Json& damage = methods.elements()[0];
    CHECK(damage.find("name")->as_string() == "damage");
    const Json& params = *damage.find("params");
    REQUIRE(params.elements().size() == 2);
    CHECK(params.elements()[0].find("name")->as_string() == "amount");
    CHECK(params.elements()[0].find("type")->as_string() == "float");
    CHECK(params.elements()[1].find("name")->as_string() == "by");
    CHECK(params.elements()[1].find("type")->as_string() == "entity_ref");
    CHECK(damage.find("returns") == nullptr); // damage() declares no return type -> omitted
}

TEST_CASE(
    "script.toolchain: a component whose top-level code would emit/throw still extracts cleanly") {
    // The static-AST proof (exit test #2): the fixture's module-scope code
    // would fire an event and throw if it ever ran; extract() never runs
    // it — the walk is entirely syntactic (ts/toolchain/driver.js).
    Toolchain toolchain(fresh_config("extract_throws"));
    ExtractOutcome outcome = toolchain.extract("testkit/fixtures/ts/component_extract_throws.ts");
    REQUIRE_MESSAGE(!outcome.check.failure.has_value(),
                    (outcome.check.failure ? outcome.check.failure->message : std::string()));
    for (const Diagnostic& diag : outcome.check.diagnostics)
        MESSAGE(diag.to_string());
    REQUIRE(outcome.check.ok);
    REQUIRE(outcome.components.elements().size() == 1);
    const Json& dangerous = outcome.components.elements()[0];
    CHECK(dangerous.find("name")->as_string() == "Dangerous");
    const Json& fields = *dangerous.find("fields");
    REQUIRE(fields.elements().size() == 2);
    CHECK(fields.elements()[0].find("name")->as_string() == "power");
    CHECK(fields.elements()[0].find("default")->as_int() == 10);
    CHECK(fields.elements()[1].find("name")->as_string() == "label");
    CHECK(fields.elements()[1].find("type")->as_string() == "string");
    CHECK(fields.elements()[1].find("default")->as_string() == "dangerous");
}

TEST_CASE("script.toolchain: an unresolvable @field type refuses as a schema diagnostic") {
    Toolchain toolchain(fresh_config("extract_unknown_type"));
    TempDir dir{"extract-unknown-type"};
    const std::string path = dir.file("bad.ts");
    REQUIRE_FALSE(midday::base::write_file(path,
                                           "import {Component, component, field} from 'midday'\n"
                                           "@component()\n"
                                           "export class Bad extends Component {\n"
                                           "    @field() mystery: object = {};\n"
                                           "}\n",
                                           "test.io")
                      .has_value());
    ExtractOutcome outcome = toolchain.extract(path);
    REQUIRE_FALSE(outcome.check.failure.has_value());
    REQUIRE_FALSE(outcome.check.ok);
    CHECK(outcome.components.elements().empty());
    REQUIRE(outcome.check.diagnostics.size() == 1);
    CHECK(outcome.check.diagnostics.front().kind == "schema");
    CHECK(outcome.check.diagnostics.front().code == "schema.unresolved_type");
    CHECK(outcome.check.diagnostics.front().message.find("mystery") != std::string::npos);
}

TEST_CASE("script.toolchain: type errors come back structured with file:line") {
    Toolchain toolchain(fresh_config("type_error"));
    CheckOutcome outcome = toolchain.check(kTypeError);
    REQUIRE_FALSE(outcome.failure.has_value());
    REQUIRE_FALSE(outcome.ok);
    REQUIRE(outcome.diagnostics.size() == 1);
    const Diagnostic& diag = outcome.diagnostics.front();
    CHECK(diag.kind == "type");
    CHECK(diag.code == "TS2322");
    CHECK(diag.file == kTypeError);
    CHECK(diag.line == line_of(kTypeError, "forty-two"));
    CHECK(diag.col > 0);
}

TEST_CASE("script.lint: the pack flags every clock/random/timer door at file:line") {
    Toolchain toolchain(fresh_config("lint"));
    CheckOutcome outcome = toolchain.check(kLint);
    REQUIRE_FALSE(outcome.failure.has_value());
    REQUIRE_FALSE(outcome.ok);
    for (const Diagnostic& diag : outcome.diagnostics) // the fixture is TYPE-clean
        CHECK_MESSAGE(diag.kind == "lint", diag.to_string());

    const struct {
        const char* what;
        const char* rule;
        const char* marker;
    } expected[] = {
        {"Date.now()", "no-wall-clock", "Date.now()"},
        {"performance.now()", "no-wall-clock", "performance.now()"},
        {"Math.random()", "no-unseeded-random", "Math.random()"},
        {"new Date()", "no-wall-clock", "new Date()"},
        {"setTimeout()", "no-timer", "setTimeout(() =>"},
        {"setInterval()", "no-timer", "setInterval(() =>"},
    };

    for (const auto& expect : expected) {
        const Diagnostic* diag = find_lint(outcome.diagnostics, expect.what);
        REQUIRE_MESSAGE(diag != nullptr, expect.what);
        CHECK(diag->code == expect.rule);
        CHECK(diag->file == kLint);
        CHECK(diag->line == line_of(kLint, expect.marker));
        CHECK(diag->col > 0);
    }
}

TEST_CASE("script.toolchain: runtime throws surface stack + slots; a fake caller fills tick 30") {
    Toolchain toolchain(fresh_config("throw"));
    ScriptRuntime runtime;
    Toolchain::LoadOutcome loaded = toolchain.load_module(runtime, kRuntimeThrow);
    midday::base::Error& error = midday::testkit::unwrap(loaded.error);
    CHECK(error.code == "script.exception");
    const Json* file = error.details.find("file");
    REQUIRE(file != nullptr);
    CHECK(file->as_string() == kRuntimeThrow);
    const Json* stack = error.details.find("stack");
    REQUIRE(stack != nullptr);
    CHECK(stack->as_string().find("detonate") != std::string::npos);
    CHECK(error.details.find("tick") == nullptr); // slot is the CALLER's

    midday::script::annotate_sim_context(error, 30, "run.mrj#tick-30");
    const Json* tick = error.details.find("tick");
    const Json* bookmark = error.details.find("replay_bookmark");
    REQUIRE(tick != nullptr);
    REQUIRE(bookmark != nullptr);
    CHECK(tick->as_int() == 30);
    CHECK(bookmark->as_string() == "run.mrj#tick-30");
}

TEST_CASE("script.cache: the second build run reports zero re-transpiles") {
    ToolchainConfig config = fresh_config("cache_stats");
    Toolchain toolchain(config);
    BuildOutcome first = toolchain.build(kHello);
    REQUIRE(first.check.ok);
    CHECK_FALSE(first.cache_hit);
    CHECK(toolchain.stats().transpiled == 1);
    CHECK(toolchain.stats().cache_hits == 0);

    BuildOutcome second = toolchain.build(kHello);
    REQUIRE(second.check.ok);
    CHECK(second.cache_hit);
    CHECK(toolchain.stats().transpiled == 1); // ZERO new transpiles
    CHECK(toolchain.stats().cache_hits == 1);
    CHECK(second.cache_key == first.cache_key);

    // A fresh toolchain over the same cache dir hits too (cross-process story).
    Toolchain reopened(config);
    BuildOutcome third = reopened.build(kHello);
    REQUIRE(third.check.ok);
    CHECK(third.cache_hit);
    CHECK(reopened.stats().transpiled == 0);
}

TEST_CASE("script.cache: keys and cached bytes are byte-identical across independent builds") {
    Toolchain first(fresh_config("dual_a"));
    Toolchain second(fresh_config("dual_b"));
    BuildOutcome a = first.build(kHello);
    BuildOutcome b = second.build(kHello);
    REQUIRE(a.check.ok);
    REQUIRE(b.check.ok);
    CHECK_FALSE(b.cache_hit); // separate cache dirs: genuinely independent runs
    CHECK(a.cache_key == b.cache_key);
    CHECK(a.js_source == b.js_source);
    midday::base::ReadFileResult bytes_a = midday::base::read_file(a.js_path, "test.io");
    midday::base::ReadFileResult bytes_b = midday::base::read_file(b.js_path, "test.io");
    REQUIRE_FALSE(bytes_a.error.has_value());
    REQUIRE_FALSE(bytes_b.error.has_value());
    CHECK(bytes_a.bytes == bytes_b.bytes);
    CHECK_FALSE(bytes_a.bytes.empty());
}

TEST_CASE("script.cache: content keys are length-prefixed, segment-exact, and pinned") {
    const std::string_view ab[] = {"a", "b"};
    const std::string_view ab_shifted[] = {"ab", ""};
    const std::string_view ab_again[] = {"a", "b"};
    const std::string key = midday::script::content_key_hex(ab);
    CHECK(key == midday::script::content_key_hex(ab_again));
    CHECK(key != midday::script::content_key_hex(ab_shifted)); // boundaries are hashed
    CHECK(key.size() == 32);
    // Cross-platform KAT: XXH3-128 of the framed segments, byte-pinned.
    CHECK(key == "4e8071d3974d16754bcb794c7d699bd6");
}

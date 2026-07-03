// script.runtime doctests — the embedded QuickJS: sim-profile poisoning,
// host hooks, deterministic gas, memory limits, structured errors, and
// resolver-only module loading.

#include "doctest/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "ts/runtime/script_runtime.h"

#include <optional>
#include <string>

namespace {

using midday::base::Error;
using midday::base::Json;
using midday::script::HostResult;
using midday::script::ModuleSource;
using midday::script::RuntimeConfig;
using midday::script::ScriptRuntime;
using midday::testkit::unwrap;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string detail_string(const Error& error, const char* key) {
    const Json* value = error.details.find(key);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

std::int64_t detail_int(const Error& error, const char* key) {
    const Json* value = error.details.find(key);
    return value != nullptr && value->is_int() ? value->as_int() : -1;
}

} // namespace

TEST_CASE("script.runtime: sim profile poisons every wall-clock and randomness door") {
    ScriptRuntime runtime;
    for (const char* probe :
         {"Date.now()", "new Date()", "Date.parse('x')", "Math.random()", "performance.now()"}) {
        std::optional<Error> maybe = runtime.eval_global(probe, "poison_probe.js");
        const Error& error = unwrap(maybe);
        CHECK(error.code == "script.exception");
        CHECK_MESSAGE(contains(error.message, "script.nondeterminism"), error.message);
    }
    // No timer surface exists at all: quickjs-libc is not vendored.
    CHECK_FALSE(runtime
                    .eval_global("if (typeof setTimeout !== 'undefined' || typeof setInterval "
                                 "!== 'undefined') throw new Error('timers leaked');",
                                 "poison_probe.js")
                    .has_value());
}

TEST_CASE("script.runtime: tool profile keeps the clock for the compiler's own use") {
    ScriptRuntime runtime(RuntimeConfig{.deterministic = false});
    CHECK_FALSE(runtime
                    .eval_global("if (typeof Date.now() !== 'number') throw new Error('no clock');",
                                 "tool_probe.js")
                    .has_value());
}

TEST_CASE("script.runtime: host hooks cross JSON values both ways and throw structured") {
    ScriptRuntime runtime;
    runtime.register_host_fn("__midday_sum", [](const Json::Array& args) {
        HostResult result;
        std::int64_t total = 0;
        for (const Json& arg : args)
            total += arg.as_int();
        result.value = Json(total);
        return result;
    });
    runtime.register_host_fn("__midday_refuse", [](const Json::Array&) {
        HostResult result;
        result.error = Error{.code = "script.host", .message = "not today"};
        return result;
    });
    CHECK_FALSE(runtime
                    .eval_global("if (__midday_sum(2, 3, 37) !== 42) throw new Error('bad sum');",
                                 "hooks.js")
                    .has_value());
    std::optional<Error> maybe = runtime.eval_global("__midday_refuse();", "hooks.js");
    const Error& error = unwrap(maybe);
    CHECK_MESSAGE(contains(error.message, "script.host: not today"), error.message);
}

TEST_CASE("script.runtime: gas budget interrupts runaway scripts, deterministically") {
    ScriptRuntime runtime(RuntimeConfig{.gas_limit = 5});
    std::optional<Error> maybe = runtime.eval_global("for (;;) {}", "runaway.js");
    const Error& error = unwrap(maybe);
    CHECK(error.code == "script.interrupted");
    CHECK(detail_int(error, "gas_limit") == 5);

    // The gas unit is VM poll points, not time: identical programs consume
    // identical gas in two independent runtimes (two runs diffed, never a
    // self-diff).
    const char* bounded = "let x = 0; for (let i = 0; i < 200000; i++) x += i;";
    ScriptRuntime first;
    ScriptRuntime second;
    REQUIRE_FALSE(first.eval_global(bounded, "bounded.js").has_value());
    REQUIRE_FALSE(second.eval_global(bounded, "bounded.js").has_value());
    CHECK(first.gas_used() > 0);
    CHECK(first.gas_used() == second.gas_used());
}

TEST_CASE("script.runtime: memory limit surfaces script.out_of_memory, never a crash") {
    ScriptRuntime runtime(RuntimeConfig{.memory_limit_bytes = 4u << 20});
    std::optional<Error> maybe = runtime.eval_global(
        "const hog = []; for (;;) hog.push(new Array(4096).fill(1));", "hog.js");
    CHECK(unwrap(maybe).code == "script.out_of_memory");
}

TEST_CASE("script.runtime: errors carry file:line:col + stack; the sim caller fills the slots") {
    ScriptRuntime runtime;
    std::optional<Error> maybe = runtime.eval_global(
        "function f() {\n    throw new Error('boom');\n}\nf();", "game_script.js");
    Error& error = unwrap(maybe);
    CHECK(error.code == "script.exception");
    CHECK(detail_string(error, "file") == "game_script.js");
    CHECK(detail_int(error, "line") == 2);
    CHECK(detail_int(error, "col") > 0);
    CHECK(contains(detail_string(error, "stack"), "at f"));
    CHECK_MESSAGE(contains(error.message, "game_script.js:2:"), error.message);

    // Slots {tick, replay_bookmark} are absent until the caller — here a
    // fake tick loop — annotates them.
    CHECK(error.details.find("tick") == nullptr);
    midday::script::annotate_sim_context(error, 30, "run.mrj#tick-30");
    CHECK(detail_int(error, "tick") == 30);
    CHECK(detail_string(error, "replay_bookmark") == "run.mrj#tick-30");
}

TEST_CASE("script.runtime: syntax errors are the structured script.syntax class") {
    ScriptRuntime runtime;
    std::optional<Error> maybe = runtime.eval_global("const x = ;", "broken.js");
    const Error& error = unwrap(maybe);
    CHECK(error.code == "script.syntax");
    CHECK(detail_string(error, "file") == "broken.js");
    CHECK(detail_int(error, "line") == 1);
}

TEST_CASE("script.runtime: modules load only through the resolver") {
    ScriptRuntime bare;
    ScriptRuntime::LoadedModule refused = bare.load_module("anything");
    CHECK(unwrap(refused.error).code == "script.module_not_found");

    ScriptRuntime runtime;
    runtime.set_module_resolver(
        [](std::string_view specifier, std::string_view referrer) -> std::optional<ModuleSource> {
            if (specifier == "main" && referrer.empty())
                return ModuleSource{"main",
                                    "import { value } from './dep';\nglobalThis.__loaded = value;"};
            if (specifier == "./dep" && referrer == "main")
                return ModuleSource{"dep", "export const value = 42;"};
            return std::nullopt;
        });
    ScriptRuntime::LoadedModule loaded = runtime.load_module("main");
    REQUIRE_FALSE(loaded.error.has_value());
    CHECK(loaded.resolved == "main");
    CHECK_FALSE(runtime
                    .eval_global("if (globalThis.__loaded !== 42) throw new Error('module lost');",
                                 "check.js")
                    .has_value());

    // Unresolvable imports are structured, and cite the specifier.
    ScriptRuntime::LoadedModule missing = runtime.load_module("nowhere");
    CHECK(unwrap(missing.error).code == "script.module_not_found");
}

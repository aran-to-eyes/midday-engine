// `midday script check|build <path>` — the agent-facing TS toolchain verbs
// (spec sections 7 and 9, m0-quickjs-ts-toolchain). check = typecheck + the
// engine lint pack; build = check + transpile + populate the content-hash
// cache. Exit contract: 3 (validation) for type errors AND lint hits — both
// with file:line diagnostics in the payload — 1 for infrastructure failures
// (missing vendored compiler, unreadable source), 2 for usage.

#include "cli/verb.h"
#include "ts/toolchain/toolchain.h"

#include <string>
#include <utility>
#include <vector>

namespace midday::cli {
namespace {

Json diagnostics_json(const std::vector<script::Diagnostic>& diagnostics) {
    Json out = Json::array();
    for (const script::Diagnostic& diag : diagnostics)
        out.push(diag.to_json());
    return out;
}

std::string diagnostics_human(const std::string& path,
                              const std::vector<script::Diagnostic>& diagnostics) {
    std::string out = path + ": " + std::to_string(diagnostics.size()) + " problem(s)";
    for (const script::Diagnostic& diag : diagnostics)
        out += "\n  " + diag.to_string();
    return out;
}

// Type errors and lint hits share the validation exit class; the error code
// names whichever class appears first so `jq .error.code` stays one hop.
VerbOutcome refuse(const std::string& path, std::vector<script::Diagnostic> diagnostics) {
    VerbOutcome out;
    out.exit = Exit::Validation;
    bool any_type = false;
    for (const script::Diagnostic& diag : diagnostics)
        any_type = any_type || diag.kind == "type";
    Error error{.code = any_type ? "script.type_error" : "script.lint",
                .message = diagnostics.front().to_string()};
    error.details.set("file", path);
    error.details.set("count", static_cast<std::int64_t>(diagnostics.size()));
    out.error = std::move(error);
    out.human = diagnostics_human(path, diagnostics);
    out.payload.set("file", path);
    out.payload.set("diagnostics", diagnostics_json(diagnostics));
    return out;
}

VerbOutcome fail(Error error) {
    VerbOutcome out;
    out.exit = Exit::Failure;
    out.error = std::move(error);
    return out;
}

VerbOutcome run_check(script::Toolchain& toolchain, const std::string& path) {
    script::CheckOutcome outcome = toolchain.check(path);
    if (outcome.failure)
        return fail(std::move(*outcome.failure));
    if (!outcome.ok)
        return refuse(path, std::move(outcome.diagnostics));
    VerbOutcome out;
    out.payload.set("file", path);
    out.payload.set("diagnostics", Json::array());
    out.human = path + ": clean (typecheck + lint)";
    return out;
}

VerbOutcome run_build(script::Toolchain& toolchain, const std::string& path, bool want_stats) {
    script::BuildOutcome built = toolchain.build(path);
    if (built.check.failure)
        return fail(std::move(*built.check.failure));
    if (!built.check.ok)
        return refuse(path, std::move(built.check.diagnostics));
    VerbOutcome out;
    out.payload.set("file", path);
    out.payload.set("js", built.js_path);
    out.payload.set("cache_key", built.cache_key);
    out.payload.set("cache_hit", built.cache_hit);
    if (want_stats) {
        Json stats = Json::object();
        stats.set("transpiled", toolchain.stats().transpiled);
        stats.set("cache_hits", toolchain.stats().cache_hits);
        out.payload.set("stats", std::move(stats));
    }
    out.human =
        path + " -> " + built.js_path + (built.cache_hit ? " (cache hit)" : " (transpiled)");
    return out;
}

VerbOutcome verb_script(const VerbArgs& args) {
    const std::string& action = args.get_string("action");
    if (action != "check" && action != "build") {
        Error error{.code = "usage.unknown_action",
                    .message = "unknown script action '" + action + "' (check | build)"};
        error.details.set("action", action);
        Json known = Json::array();
        known.push("check");
        known.push("build");
        error.details.set("known", std::move(known));
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = std::move(error);
        return out;
    }
    script::ToolchainConfig config;
    config.cache_dir = args.get_string("cache-dir");
    script::Toolchain toolchain(std::move(config));
    const std::string& path = args.get_string("path");
    return action == "check" ? run_check(toolchain, path)
                             : run_build(toolchain, path, args.get_bool("stats"));
}

} // namespace

const VerbSpec& script_spec() {
    static constexpr FlagSpec kFlags[] = {
        {.name = "cache-dir",
         .type = "string",
         .doc = "content-hash cache directory (regenerable, never committed)",
         .default_text = ".midday-cache/ts"},
        {.name = "stats",
         .type = "bool",
         .doc = "build: report {transpiled, cache_hits} counters in the payload"},
    };
    static constexpr PositionalSpec kPositionals[] = {
        {.name = "action", .type = "name", .doc = "check | build"},
        {.name = "path", .type = "string", .doc = "TypeScript source file"},
    };
    static constexpr VerbSpec kSpec{
        .name = "script",
        .summary = "typecheck, lint, and transpile TypeScript on the embedded toolchain",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &verb_script};
    return kSpec;
}

} // namespace midday::cli

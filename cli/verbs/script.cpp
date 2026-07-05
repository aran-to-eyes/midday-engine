// `midday script check|build|extract <path>` and `midday script bench` —
// the agent-facing TS toolchain verbs (spec sections 7 and 9,
// m0-quickjs-ts-toolchain + m0-batch-bindings + m1-ts-components). check =
// typecheck + the engine lint pack; build = check + transpile + populate
// the content-hash cache; extract = check + the AST component-schema walk,
// written to a PROJECT-LEVEL manifest (never api/schema_manifest.json —
// that artifact is engine-only and codegen-owned, api/CODEGEN.md); bench =
// the batch-binding budget harness (boundary crossings and GC bytes per
// tick as JSON — the m0 sweep gates run it at 1k/10k/100k). Exit contract:
// 3 (validation) for type errors, lint hits, AND schema diagnostics — all
// with file:line in the payload — 1 for infrastructure failures (missing
// vendored compiler, unreadable source, an unwritable --out path), 2 for
// usage.

#include "cli/verb.h"
#include "core/base/file_io.h"
#include "ts/runtime/batch_bench.h"
#include "ts/toolchain/toolchain.h"

#include <cstdint>
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

// Type errors, lint hits, and schema diagnostics share the validation exit
// class; the error code names whichever class appears first (in that
// priority order) so `jq .error.code` stays one hop. A file can only carry
// "schema" diagnostics once type+lint are ALREADY clean (extract() never
// walks a dirty compile — ts/toolchain/toolchain.h), so the three kinds
// never actually mix within one refusal; the priority order is defensive.
VerbOutcome refuse(const std::string& path, std::vector<script::Diagnostic> diagnostics) {
    VerbOutcome out;
    out.exit = Exit::Validation;
    bool any_type = false;
    bool any_schema = false;
    for (const script::Diagnostic& diag : diagnostics) {
        any_type = any_type || diag.kind == "type";
        any_schema = any_schema || diag.kind == "schema";
    }
    const char* code = any_type     ? "script.type_error"
                       : any_schema ? "script.schema_error"
                                    : "script.lint";
    Error error{.code = code, .message = diagnostics.front().to_string()};
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

// Project-level component manifest: format_version + components[], the
// SAME filename convention as the committed api/schema_manifest.json but a
// DIFFERENT document (game content, not the engine API) at a caller-chosen
// path — never api/schema_manifest.json, which stays engine-only and
// codegen-owned (api/CODEGEN.md). --out is required so the boundary is
// explicit at every call site, not an easy-to-miss default.
VerbOutcome
run_extract(script::Toolchain& toolchain, const std::string& path, const std::string& out) {
    script::ExtractOutcome outcome = toolchain.extract(path);
    if (outcome.check.failure)
        return fail(std::move(*outcome.check.failure));
    if (!outcome.check.ok)
        return refuse(path, std::move(outcome.check.diagnostics));
    Json manifest = Json::object();
    manifest.set("format_version", static_cast<std::int64_t>(1));
    manifest.set("components", outcome.components);
    if (auto error = base::write_file(out, manifest.dump() + "\n", "script.io"))
        return fail(std::move(*error));
    VerbOutcome result;
    result.payload.set("file", path);
    result.payload.set("out", out);
    result.payload.set("components",
                       static_cast<std::int64_t>(outcome.components.elements().size()));
    result.human = path + " -> " + out + " (" +
                   std::to_string(outcome.components.elements().size()) + " component schema(s))";
    return result;
}

VerbOutcome run_bench(const VerbArgs& args) {
    const std::int64_t entities = args.get_int("entities");
    const std::int64_t ticks = args.get_int("ticks");
    const std::int64_t warmup = args.get_int("warmup");
    if (entities < 1 || ticks < 1 || warmup < 0) {
        Error error{.code = "usage.invalid_value",
                    .message = "script bench needs --entities >= 1, --ticks >= 1, --warmup >= 0"};
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = std::move(error);
        return out;
    }
    script::BenchConfig config;
    config.entities = static_cast<std::uint32_t>(entities);
    config.ticks = static_cast<std::uint32_t>(ticks);
    config.warmup_ticks = static_cast<std::uint32_t>(warmup);
    config.naive = args.get_bool("naive");
    config.cache_dir = args.get_string("cache-dir");
    if (args.present("path"))
        config.script_path = args.get_string("path");
    script::BenchOutcome bench = script::run_script_bench(config);
    if (bench.error)
        return fail(std::move(*bench.error));
    VerbOutcome out;
    for (const auto& [key, value] : bench.budget.items())
        out.payload.set(key, value); // flat payload: jq reads budget fields directly
    out.human = "script bench (" + std::string(config.naive ? "naive" : "batched") +
                "): " + std::to_string(entities) + " entities x " + std::to_string(ticks) +
                " ticks -> " + bench.budget.find("boundary_crossings_per_tick")->dump() +
                " crossings/tick, " + bench.budget.find("gc_alloc_bytes_per_tick")->dump() +
                " gc bytes/tick";
    return out;
}

VerbOutcome verb_script(const VerbArgs& args) {
    const std::string& action = args.get_string("action");
    if (action != "check" && action != "build" && action != "extract" && action != "bench") {
        Error error{.code = "usage.unknown_action",
                    .message =
                        "unknown script action '" + action + "' (check | build | extract | bench)"};
        error.details.set("action", action);
        Json known = Json::array();
        known.push("check");
        known.push("build");
        known.push("extract");
        known.push("bench");
        error.details.set("known", std::move(known));
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = std::move(error);
        return out;
    }
    if (action == "bench")
        return run_bench(args);
    if (!args.present("path")) {
        Error error{.code = "usage.missing_argument",
                    .message = "script " + action + " needs a TypeScript source path"};
        error.details.set("argument", "path");
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = std::move(error);
        return out;
    }
    if (action == "extract" && !args.present("out")) {
        Error error{.code = "usage.missing_argument",
                    .message = "script extract needs --out <schema_manifest.json path> (a "
                               "project-level file — never api/schema_manifest.json)"};
        error.details.set("argument", "out");
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = std::move(error);
        return out;
    }
    script::ToolchainConfig config;
    config.cache_dir = args.get_string("cache-dir");
    script::Toolchain toolchain(std::move(config));
    const std::string& path = args.get_string("path");
    if (action == "check")
        return run_check(toolchain, path);
    if (action == "extract")
        return run_extract(toolchain, path, args.get_string("out"));
    return run_build(toolchain, path, args.get_bool("stats"));
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
        {.name = "out",
         .type = "string",
         .doc = "extract: project-level component schema manifest path to write "
                "(required; never api/schema_manifest.json)"},
        {.name = "entities",
         .type = "int",
         .doc = "bench: entity count for the budget sweep",
         .default_text = "1000"},
        {.name = "ticks",
         .type = "int",
         .doc = "bench: measured ticks (after warmup)",
         .default_text = "60"},
        {.name = "warmup",
         .type = "int",
         .doc = "bench: unmeasured warmup ticks before the window",
         .default_text = "5"},
        {.name = "naive",
         .type = "bool",
         .doc = "bench: per-field host-hook accessors (the chatty comparison mode)"},
    };
    static constexpr PositionalSpec kPositionals[] = {
        {.name = "action", .type = "name", .doc = "check | build | extract | bench"},
        {.name = "path",
         .type = "string",
         .doc = "TypeScript source file (bench: overrides the committed fixture)",
         .required = false},
    };
    static constexpr VerbSpec kSpec{
        .name = "script",
        .summary = "typecheck, lint, transpile, and benchmark TypeScript on the embedded runtime",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &verb_script};
    return kSpec;
}

} // namespace midday::cli

// `midday api dump|diff|codegen` — engine_api.json emission, drift
// detection, and artifact generation (spec section 9; m0-api-json +
// m0-codegen-selfhost). dump prints the canonical document (or writes it
// with --out, always LF + trailing newline — the committed-artifact bytes);
// diff compares a saved document's compat hashes against the current build;
// codegen runs THE code generator — self-hosted TS-on-QuickJS by default
// (authoritative since m0-codegen-selfhost), the TEMPORARY native bootstrap
// with --bootstrap, and --verify-equivalence runs BOTH and byte-compares
// all four artifacts (the standing gate until the bootstrap tool retires).

#include "api/engine_api.h"
#include "cli/help.h"
#include "cli/verb.h"
#include "core/base/file_io.h"
#include "tools/codegen_bootstrap/codegen.h"
#include "ts/codegen/selfhost.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef MIDDAY_VERSION
#define MIDDAY_VERSION "0.0.0-unversioned"
#endif

namespace midday::cli {
namespace {

constexpr std::string_view kIoCode = "api.io";

constexpr std::array<std::string_view, 4> kArtifactNames = {
    "engine.d.ts", "schema_manifest.json", "api_docs.md", "bindings_spec.json"};

Json current_document() {
    const api::BootRegistry boot;
    Json schemas = Json::array();
    for (const VerbSpec* spec : verbs())
        schemas.push(verb_schema(*spec));
    return api::build_document(boot.registry, schemas, MIDDAY_VERSION);
}

VerbOutcome fail(Exit exit, Error error) {
    VerbOutcome out;
    out.exit = exit;
    out.error = std::move(error);
    return out;
}

VerbOutcome usage(std::string code, std::string message, std::string_view key, Json value) {
    Error error{.code = std::move(code), .message = std::move(message)};
    error.details.set(key, std::move(value));
    return fail(Exit::Usage, std::move(error));
}

VerbOutcome run_dump(const VerbArgs& args) {
    if (args.present("input"))
        return usage("usage.unexpected_argument",
                     "api dump takes no positional argument (did you mean: api diff <old>?)",
                     "argument",
                     args.get_string("input"));

    Json document = current_document();
    const std::string hash = document.find("api_compat_hash")->as_string();
    VerbOutcome out;
    if (args.present("out")) {
        const std::string& path = args.get_string("out");
        if (auto error = base::write_file(path, document.dump() + "\n", kIoCode))
            return fail(Exit::Failure, std::move(*error));
        out.payload.set("out", path);
        out.payload.set("api_compat_hash", hash);
        out.human = "engine_api.json -> " + path + " (api_compat_hash " + hash + ")";
    } else {
        // Human mode prints the document itself: `midday api dump > f` and
        // `--out f` produce byte-identical files.
        out.human = document.dump();
        out.payload.set("api", std::move(document));
    }
    return out;
}

VerbOutcome run_diff(const VerbArgs& args) {
    if (!args.present("input"))
        return usage("usage.missing_argument",
                     "api diff needs the baseline document: midday api diff <old.json>",
                     "argument",
                     "input");
    if (args.present("out"))
        return usage(
            "usage.unexpected_flag", "--out applies to api dump, not api diff", "flag", "out");

    // The baseline is DATA: unreadable/unparseable/malformed input is the
    // validation class (exit 3), never a crash (D-BUILD-039 exit mapping).
    const std::string& path = args.get_string("input");
    base::ReadFileResult file = base::read_file(path, kIoCode);
    if (file.error)
        return fail(Exit::Validation, std::move(*file.error));
    Json::ParseResult parsed = Json::parse(file.bytes, path);
    if (parsed.error)
        return fail(Exit::Validation, base::to_error(*parsed.error));
    if (auto error = api::check_document(parsed.value))
        return fail(Exit::Validation, std::move(*error));

    const api::Diff diff = api::diff_documents(parsed.value, current_document());
    VerbOutcome out;
    for (const auto& [key, value] : diff.report.items())
        out.payload.set(key, value); // flat payload (D-BUILD-001)
    if (diff.identical) {
        out.human = "api: identical to " + path + " (api_compat_hash " +
                    diff.report.find("api_compat_hash")->as_string() + ")";
        return out;
    }
    out.exit = Exit::Failure;
    Error error{.code = "api.drift",
                .message =
                    "engine API drifted from " + path + ": " +
                    std::to_string(diff.report.find("added")->elements().size()) + " added, " +
                    std::to_string(diff.report.find("removed")->elements().size()) + " removed, " +
                    std::to_string(diff.report.find("changed")->elements().size()) + " changed"};
    error.details.set("old", path);
    out.error = std::move(error);
    return out;
}

// -------------------------------------------------------------- codegen

// codegen exit classes (api/CODEGEN.md): input problems are validation
// (exit 3); toolchain/runtime infrastructure and generator bugs are
// failures (exit 1) — "exit 3 = your document, exit 1 = the generator".
Exit codegen_exit(const Error& error) {
    const bool generator_bug = error.code == "codegen.selfcheck" ||
                               error.code == "codegen.internal" || error.code == "codegen.io.write";
    return error.code.starts_with("script.") || generator_bug ? Exit::Failure : Exit::Validation;
}

struct GeneratedSet {
    std::array<std::string, 4> files; // kArtifactNames order
    std::string api_compat_hash;
    std::optional<Error> error;
};

GeneratedSet generate_bootstrap(std::string_view bytes, std::string_view origin) {
    GeneratedSet out;
    codegen::LoadResult loaded = codegen::load_document(bytes, origin);
    if (loaded.error) {
        out.error = std::move(loaded.error);
        return out;
    }
    codegen::Outputs generated = codegen::generate(loaded.document);
    if (const auto shape = codegen::dts_shape_errors(generated.dts, loaded.document);
        !shape.empty()) {
        Error error{.code = "codegen.selfcheck",
                    .message = "generated engine.d.ts failed its structural shape check"};
        Json list = Json::array();
        for (const std::string& problem : shape)
            list.push(problem);
        error.details.set("errors", std::move(list));
        out.error = std::move(error);
        return out;
    }
    out.api_compat_hash = loaded.document.find("api_compat_hash")->as_string();
    out.files = {std::move(generated.dts),
                 std::move(generated.manifest),
                 std::move(generated.docs),
                 std::move(generated.bindings)};
    return out;
}

GeneratedSet
generate_selfhost(std::string_view bytes, std::string_view origin, const std::string& cache_dir) {
    selfhost::Config config;
    config.cache_dir = cache_dir;
    config.origin = std::string(origin);
    selfhost::RunResult result = selfhost::run_generator(bytes, config);
    GeneratedSet out;
    if (result.error) {
        out.error = std::move(result.error);
        return out;
    }
    out.api_compat_hash = std::move(result.files.api_compat_hash);
    out.files = {std::move(result.files.dts),
                 std::move(result.files.manifest),
                 std::move(result.files.docs),
                 std::move(result.files.bindings)};
    return out;
}

// First differing byte of two artifacts as a structured location + excerpt.
Json diff_location(const std::string& bootstrap, const std::string& selfhost) {
    const std::size_t limit =
        bootstrap.size() < selfhost.size() ? bootstrap.size() : selfhost.size();
    std::size_t offset = 0;
    while (offset < limit && bootstrap[offset] == selfhost[offset])
        ++offset;
    std::int64_t line = 1;
    for (std::size_t i = 0; i < offset; ++i)
        line += bootstrap[i] == '\n' ? 1 : 0;
    Json out = Json::object();
    out.set("offset", static_cast<std::int64_t>(offset));
    out.set("line", line);
    out.set("bootstrap", bootstrap.substr(offset, 40));
    out.set("selfhost", selfhost.substr(offset, 40));
    return out;
}

VerbOutcome
run_equivalence(const std::string& input, std::string_view bytes, const std::string& cache_dir) {
    GeneratedSet native = generate_bootstrap(bytes, input);
    if (native.error)
        return fail(codegen_exit(*native.error), std::move(*native.error));
    GeneratedSet hosted = generate_selfhost(bytes, input, cache_dir);
    if (hosted.error)
        return fail(codegen_exit(*hosted.error), std::move(*hosted.error));

    Json report = Json::array();
    std::int64_t divergent = 0;
    for (std::size_t i = 0; i < kArtifactNames.size(); ++i) {
        Json entry = Json::object();
        entry.set("file", kArtifactNames[i]);
        // bindings_spec.json compares modulo the batch envelope: it is
        // self-host-only glue the frozen bootstrap never emits (D-BUILD-069;
        // selfhost::bindings_equivalence_view documents the scope).
        const bool bindings = kArtifactNames[i] == "bindings_spec.json";
        const std::string left =
            bindings ? selfhost::bindings_equivalence_view(native.files[i]) : native.files[i];
        const std::string right =
            bindings ? selfhost::bindings_equivalence_view(hosted.files[i]) : hosted.files[i];
        const bool equal = left == right;
        entry.set("equal", equal);
        if (!equal) {
            ++divergent;
            entry.set("diff", diff_location(left, right));
        }
        report.push(std::move(entry));
    }

    VerbOutcome out;
    out.payload.set("input", input);
    out.payload.set("api_compat_hash", native.api_compat_hash);
    out.payload.set("equal", divergent == 0);
    out.payload.set("files", report);
    if (divergent == 0) {
        out.human = "codegen equivalence: selfhost == bootstrap, all 4 artifacts byte-identical (" +
                    input + ", api_compat_hash " + native.api_compat_hash + ")";
        return out;
    }
    out.exit = Exit::Failure;
    Error error{.code = "codegen.equivalence",
                .message = "self-hosted generator diverged from bootstrap on " +
                           std::to_string(divergent) + " artifact(s) for " + input};
    error.details.set("files", std::move(report));
    out.error = std::move(error);
    std::string human = "codegen equivalence FAILED for " + input + ":";
    for (const Json& entry : out.payload.find("files")->elements())
        if (!entry.find("equal")->as_bool()) {
            const Json* diff = entry.find("diff");
            human += "\n  " + entry.find("file")->as_string() + ": first diff at offset " +
                     diff->find("offset")->dump() + " (line " + diff->find("line")->dump() +
                     "): '" + diff->find("bootstrap")->as_string() + "' vs '" +
                     diff->find("selfhost")->as_string() + "'";
        }
    out.human = std::move(human);
    return out;
}

VerbOutcome run_codegen(const VerbArgs& args) {
    if (args.present("out"))
        return usage("usage.unexpected_flag",
                     "--out applies to api dump; api codegen writes into --out-dir",
                     "flag",
                     "out");
    const bool bootstrap = args.get_bool("bootstrap");
    const bool selfhost_requested = args.get_bool("selfhost");
    const bool verify = args.get_bool("verify-equivalence");
    if (bootstrap && selfhost_requested)
        return usage("usage.conflicting_flags",
                     "--selfhost and --bootstrap are mutually exclusive",
                     "flags",
                     "selfhost, bootstrap");
    if (verify && (bootstrap || selfhost_requested))
        return usage("usage.conflicting_flags",
                     "--verify-equivalence always runs BOTH generators",
                     "flags",
                     "verify-equivalence");

    const std::string input =
        args.present("input") ? args.get_string("input") : std::string("api/engine_api.json");
    base::ReadFileResult file = base::read_file(input, "codegen.io");
    if (file.error) // the input is DATA: unreadable input is the validation class
        return fail(Exit::Validation, std::move(*file.error));
    const std::string& cache_dir = args.get_string("cache-dir");
    if (verify)
        return run_equivalence(input, file.bytes, cache_dir);

    GeneratedSet generated = bootstrap ? generate_bootstrap(file.bytes, input)
                                       : generate_selfhost(file.bytes, input, cache_dir);
    if (generated.error)
        return fail(codegen_exit(*generated.error), std::move(*generated.error));

    const std::string& out_dir = args.get_string("out-dir");
    std::error_code fs_error;
    std::filesystem::create_directories(out_dir, fs_error);
    if (fs_error)
        return fail(
            Exit::Failure,
            Error{.code = "codegen.io.write",
                  .message = "cannot create out-dir " + out_dir + ": " + fs_error.message()});
    Json written = Json::array();
    for (std::size_t i = 0; i < kArtifactNames.size(); ++i) {
        const std::filesystem::path path = std::filesystem::path(out_dir) / kArtifactNames[i];
        if (auto error = base::write_file(path, generated.files[i], "codegen.io.write"))
            return fail(Exit::Failure, std::move(*error));
        written.push(kArtifactNames[i]);
    }

    const char* generator = bootstrap ? "bootstrap" : "selfhost";
    VerbOutcome out;
    out.payload.set("generator", generator);
    out.payload.set("out_dir", out_dir);
    out.payload.set("api_compat_hash", generated.api_compat_hash);
    out.payload.set("files", std::move(written));
    out.human = std::string("codegen (") + generator + "): 4 artifacts -> " + out_dir +
                " (api_compat_hash " + generated.api_compat_hash + ")";
    return out;
}

VerbOutcome verb_api(const VerbArgs& args) {
    const std::string& action = args.get_string("action");
    if (action == "dump")
        return run_dump(args);
    if (action == "diff")
        return run_diff(args);
    if (action == "codegen")
        return run_codegen(args);
    Error error{.code = "usage.unknown_action",
                .message = "unknown api action '" + action + "' (dump | diff | codegen)"};
    error.details.set("action", action);
    Json known = Json::array();
    known.push("dump");
    known.push("diff");
    known.push("codegen");
    error.details.set("known", std::move(known));
    return fail(Exit::Usage, std::move(error));
}

} // namespace

const VerbSpec& api_spec() {
    static constexpr FlagSpec kFlags[] = {
        {.name = "out",
         .type = "string",
         .doc = "dump: write the document to this path instead of printing it"},
        {.name = "out-dir",
         .type = "string",
         .doc = "codegen: directory for the four generated artifacts",
         .default_text = "api"},
        {.name = "cache-dir",
         .type = "string",
         .doc = "codegen: TS toolchain content-hash cache (regenerable, never committed)",
         .default_text = ".midday-cache/ts"},
        {.name = "selfhost",
         .type = "bool",
         .doc = "codegen: run the self-hosted TS-on-QuickJS generator (the default)"},
        {.name = "bootstrap",
         .type = "bool",
         .doc = "codegen: run the TEMPORARY native bootstrap generator instead"},
        {.name = "verify-equivalence",
         .type = "bool",
         .doc = "codegen: run BOTH generators and byte-compare all four artifacts"},
    };
    static constexpr PositionalSpec kPositionals[] = {
        {.name = "action", .type = "name", .doc = "dump | diff | codegen"},
        {.name = "input",
         .type = "string",
         .doc = "diff: baseline engine_api.json; codegen: input document "
                "(default api/engine_api.json)",
         .required = false},
    };
    static constexpr VerbSpec kSpec{
        .name = "api",
        .summary = "emit, diff, or generate from engine_api.json, the canonical API document",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &verb_api};
    return kSpec;
}

} // namespace midday::cli

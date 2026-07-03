// `midday api dump|diff` — engine_api.json emission and drift detection
// (spec section 9, m0-api-json). dump prints the canonical document (or
// writes it with --out, always LF + trailing newline — the committed-artifact
// bytes); diff compares a saved document's compat hashes against the current
// build and exits 1 with a structured report on drift.

#include "api/engine_api.h"
#include "cli/help.h"
#include "cli/verb.h"
#include "core/base/file_io.h"

#include <string>
#include <utility>

#ifndef MIDDAY_VERSION
#define MIDDAY_VERSION "0.0.0-unversioned"
#endif

namespace midday::cli {
namespace {

constexpr std::string_view kIoCode = "api.io";

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
    if (args.present("old"))
        return usage("usage.unexpected_argument",
                     "api dump takes no positional argument (did you mean: api diff <old>?)",
                     "argument",
                     args.get_string("old"));

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
    if (!args.present("old"))
        return usage("usage.missing_argument",
                     "api diff needs the baseline document: midday api diff <old.json>",
                     "argument",
                     "old");
    if (args.present("out"))
        return usage(
            "usage.unexpected_flag", "--out applies to api dump, not api diff", "flag", "out");

    // The baseline is DATA: unreadable/unparseable/malformed input is the
    // validation class (exit 3), never a crash (D-BUILD-039 exit mapping).
    const std::string& path = args.get_string("old");
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

VerbOutcome verb_api(const VerbArgs& args) {
    const std::string& action = args.get_string("action");
    if (action == "dump")
        return run_dump(args);
    if (action == "diff")
        return run_diff(args);
    Error error{.code = "usage.unknown_action",
                .message = "unknown api action '" + action + "' (dump | diff)"};
    error.details.set("action", action);
    Json known = Json::array();
    known.push("dump");
    known.push("diff");
    error.details.set("known", std::move(known));
    return fail(Exit::Usage, std::move(error));
}

} // namespace

const VerbSpec& api_spec() {
    static constexpr FlagSpec kFlags[] = {
        {.name = "out",
         .type = "string",
         .doc = "dump: write the document to this path instead of printing it"},
    };
    static constexpr PositionalSpec kPositionals[] = {
        {.name = "action", .type = "name", .doc = "dump | diff"},
        {.name = "old",
         .type = "string",
         .doc = "diff: baseline engine_api.json to compare against",
         .required = false},
    };
    static constexpr VerbSpec kSpec{
        .name = "api",
        .summary = "emit or diff engine_api.json, the canonical generated API document",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &verb_api};
    return kSpec;
}

} // namespace midday::cli

// `midday check <root> [--fix] [--cache-dir <dir>]` — the m1-uid-system
// project-wide asset-reference auditor. Schema-agnostic (like `midday
// fmt`/`validate`'s events dispatch): walks every `*.yaml` under `root` for
// `{uid, path}` / `{path}` ref sites (core/loader/asset_ref.h) and every
// `*.uid` sidecar (core/loader/uid_registry.h) to build the project's
// uid<->path registry, then classifies each ref against it.
//
// Findings: clean (nothing to do) · drift (uid known, path stale — `--fix`
// rewrites the path FROM the sidecar, uid never touches) · missing_uid (a
// legacy path-only ref whose path resolves — `--fix` attaches the correct
// uid, minting + writing a sidecar only when the asset does not already
// have one) · invalid (a uid that is malformed OR simply not backed by any
// `.uid` sidecar anywhere under `root` — spec lines 365-366's "never hand-
// minted" refusal; `--fix` can only self-heal this when the ref's path
// still resolves to a real asset, exactly like missing_uid's repair).
//
// A `.midday-cache/uid/registry.json` cache is (re)written every run —
// PURELY regenerable output (never read back as input here or anywhere
// else): deleting it and rerunning `check` reproduces it byte-for-byte from
// the same sidecars, proven by scripts/verify.sh's "m1-uid-system" step.
//
// Exit codes mirror `midday fmt --check`'s precedent: Ok when everything is
// clean (or --fix repaired everything fixable); Failure(1) when fixable
// drift/missing_uid findings remain because --fix was not passed ("run with
// --fix"); Validation(3) when an invalid (hand-minted or unresolvable)
// finding remains after any fix attempt — a genuine content problem, the
// same exit class every other loader.*/schema.* refusal in this CLI uses.

#include "cli/verb.h"
#include "core/base/file_io.h"
#include "core/loader/asset_ref.h"
#include "core/loader/uid_registry.h"
#include "core/loader/yaml.h"
#include "core/loader/yaml_emit.h"

#include <filesystem>
#include <string>
#include <utility>

namespace midday::cli {
namespace {

// Absolute, lexically-normalized, forward-slash form — the precondition
// core/loader/asset_ref.h and uid_registry.h's path math requires. Not just
// `.lexically_normal().generic_string()`: normalizing "." alone (this
// verb's most common argument) legally keeps a TRAILING separator ("the
// current directory" spelled as an empty last component), which would
// otherwise show up as an ugly "//": is stripped here once, everywhere.
std::string absolute_generic(const std::string& raw) {
    std::string result = std::filesystem::absolute(raw).lexically_normal().generic_string();
    if (result.size() > 1 && result.back() == '/')
        result.pop_back();
    return result;
}

VerbOutcome refuse(Error error) {
    VerbOutcome out;
    out.exit = Exit::Validation;
    out.error = std::move(error);
    return out;
}

std::string_view status_name(loader::RefStatus status) {
    switch (status) {
    case loader::RefStatus::kClean:
        return "clean";
    case loader::RefStatus::kDrift:
        return "drift";
    case loader::RefStatus::kMissingUid:
        return "missing_uid";
    case loader::RefStatus::kInvalid:
        return "invalid";
    }
    return "invalid";
}

Json finding_json(const loader::RefFinding& finding) {
    Json entry = Json::object();
    entry.set("file", finding.file);
    entry.set("line", static_cast<std::int64_t>(finding.line));
    entry.set("col", static_cast<std::int64_t>(finding.col));
    entry.set("status", status_name(finding.status));
    entry.set("uid", finding.uid_text);
    entry.set("path", finding.path);
    entry.set("fixed", finding.fixed);
    entry.set("detail", finding.detail);
    return entry;
}

VerbOutcome check_verb(const VerbArgs& args) {
    const std::string root = absolute_generic(args.get_string("root"));
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
        return refuse(
            base::file_error("loader.io", "check root '" + root + "' is not a directory"));

    loader::BuildRegistryResult build = loader::build_uid_registry(root);
    if (build.error.has_value())
        return refuse(std::move(*build.error));
    loader::UidRegistry registry = std::move(build.registry);

    const bool fix = args.get_bool("fix");
    loader::UidRng rng = loader::make_uid_rng();
    const std::vector<std::string> yaml_files = loader::find_files_with_suffix(root, ".yaml");

    std::vector<loader::RefFinding> all_findings;
    for (const std::string& file : yaml_files) {
        loader::YamlParseResult parsed = loader::parse_yaml_file(file);
        if (parsed.error.has_value())
            return refuse(std::move(*parsed.error));
        const std::string file_dir = std::filesystem::path(file).parent_path().generic_string();
        loader::ScanRefsResult scanned =
            loader::scan_refs(parsed.root, file, file_dir, root, registry, rng, fix);
        if (scanned.changed) {
            if (auto error = base::write_file(file, loader::emit_yaml(parsed.root), "check.io"))
                return refuse(std::move(*error));
        }
        for (loader::RefFinding& finding : scanned.findings)
            all_findings.push_back(std::move(finding));
    }

    const std::string cache_dir =
        args.present("cache-dir") ? args.get_string("cache-dir") : root + "/.midday-cache/uid";
    if (auto error = loader::write_uid_cache(cache_dir + "/registry.json", registry))
        return refuse(std::move(*error));

    std::int64_t clean = 0, drift = 0, missing_uid = 0, invalid = 0, fixed = 0;
    bool invalid_remains = false;
    bool fixable_remains = false;
    for (const loader::RefFinding& finding : all_findings) {
        switch (finding.status) {
        case loader::RefStatus::kClean:
            ++clean;
            break;
        case loader::RefStatus::kDrift:
            ++drift;
            fixable_remains = fixable_remains || !finding.fixed;
            break;
        case loader::RefStatus::kMissingUid:
            ++missing_uid;
            fixable_remains = fixable_remains || !finding.fixed;
            break;
        case loader::RefStatus::kInvalid:
            ++invalid;
            invalid_remains = invalid_remains || !finding.fixed;
            break;
        }
        if (finding.fixed)
            ++fixed;
    }

    VerbOutcome out;
    out.payload.set("root", root);
    out.payload.set("fix", fix);
    out.payload.set("cache_dir", cache_dir);
    out.payload.set("files_scanned", static_cast<std::int64_t>(yaml_files.size()));
    out.payload.set("sidecars", static_cast<std::int64_t>(registry.sorted_entries().size()));
    Json findings = Json::array();
    for (const loader::RefFinding& finding : all_findings)
        findings.push(finding_json(finding));
    out.payload.set("findings", std::move(findings));
    Json counts = Json::object();
    counts.set("clean", clean);
    counts.set("drift", drift);
    counts.set("missing_uid", missing_uid);
    counts.set("invalid", invalid);
    counts.set("fixed", fixed);
    out.payload.set("counts", std::move(counts));

    if (invalid_remains) {
        out.exit = Exit::Validation;
        out.error = Error{.code = "check.invalid_ref",
                          .message = std::to_string(invalid) +
                                     " reference(s) cite a uid with no backing .uid sidecar "
                                     "(hand-minted) or an unresolvable path"};
        out.human = out.error->message;
    } else if (fixable_remains) {
        out.exit = Exit::Failure;
        out.error = Error{.code = "check.needs_fix",
                          .message = std::to_string(drift + missing_uid) +
                                     " reference(s) need --fix (drift: " + std::to_string(drift) +
                                     ", missing uid: " + std::to_string(missing_uid) + ")"};
        out.human = out.error->message;
    } else {
        out.human = root + ": " + std::to_string(all_findings.size()) + " reference(s) clean" +
                    (fixed > 0 ? " (" + std::to_string(fixed) + " fixed)" : "");
    }
    return out;
}

constexpr FlagSpec kFlags[] = {
    {.name = "fix", .type = "bool", .doc = "repair fixable drift/missing-uid findings in place"},
    {.name = "cache-dir",
     .type = "string",
     .doc = "uid registry cache directory (default: <root>/.midday-cache/uid)"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "root", .type = "string", .doc = "project directory to scan"},
};

} // namespace

const VerbSpec& check_spec() {
    static const VerbSpec spec{
        .name = "check",
        .summary = "audit {uid, path} asset references against the project's .uid sidecars",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &check_verb,
    };
    return spec;
}

} // namespace midday::cli

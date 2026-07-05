// `midday mv <src> <dst> [--root <dir>] [--cache-dir <dir>]` — moves an
// asset (+ its `.uid` sidecar, if one exists) and rewrites every
// referencing `path:` field under `--root` to match. The uid NEVER
// changes (spec lines 365-366: "midday mv rewrites paths, uids never
// change") — this verb never mints, it only relocates and rewrites text
// (core/loader/asset_ref.h's rewrite_ref_paths, the same generic
// {uid, path}/{path} ref-shape mechanism `midday check` classifies
// against).
//
// `--root` defaults to the CURRENT DIRECTORY — the same default every
// other tree-scanning CLI tool uses (git, grep, eslint...), and a better
// fit here than "src's own directory": a referencing scene commonly lives
// ABOVE the asset it points at (`scene.yaml` next to an `assets/` subtree),
// so deriving the scan root from src's path would routinely miss exactly
// the files a move needs to fix. Every `*.yaml` under the root is scanned;
// a file that fails to parse is SKIPPED (reported, not fatal): the
// physical move already happened by the time scanning starts, so aborting
// halfway cannot undo it, and an unrelated malformed file elsewhere in the
// tree should not block ref maintenance for the files that DO parse.

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
// `.lexically_normal().generic_string()`: normalizing "." alone (the
// default --root) legally keeps a TRAILING separator ("the current
// directory" spelled as an empty last component), which would otherwise
// show up as an ugly "//"; stripped here once, everywhere.
std::string absolute_generic(const std::string& raw) {
    std::string result = std::filesystem::absolute(raw).lexically_normal().generic_string();
    if (result.size() > 1 && result.back() == '/')
        result.pop_back();
    return result;
}

VerbOutcome refuse(Exit exit, Error error) {
    VerbOutcome out;
    out.exit = exit;
    out.error = std::move(error);
    return out;
}

VerbOutcome mv_verb(const VerbArgs& args) {
    const std::string src = absolute_generic(args.get_string("src"));
    const std::string dst = absolute_generic(args.get_string("dst"));

    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec))
        return refuse(Exit::Validation,
                      base::file_error("mv.src_missing", src + " does not exist"));
    if (std::filesystem::exists(dst, ec))
        return refuse(Exit::Failure,
                      Error{.code = "mv.dst_exists", .message = dst + " already exists"});

    const std::string root = absolute_generic(args.present("root") ? args.get_string("root") : ".");

    std::filesystem::create_directories(std::filesystem::path(dst).parent_path(), ec);
    std::filesystem::rename(src, dst, ec);
    if (ec)
        return refuse(Exit::Validation,
                      base::file_error("mv.io", "cannot move " + src + " to " + dst));

    const std::string src_sidecar = loader::sidecar_path_for(src);
    const std::string dst_sidecar = loader::sidecar_path_for(dst);
    bool sidecar_moved = false;
    if (std::filesystem::is_regular_file(src_sidecar, ec)) {
        std::filesystem::rename(src_sidecar, dst_sidecar, ec);
        if (ec)
            return refuse(Exit::Validation,
                          base::file_error("mv.io",
                                           "moved " + src + " but could not move its sidecar " +
                                               src_sidecar));
        sidecar_moved = true;
    }

    Json files_updated = Json::array();
    Json skipped = Json::array();
    for (const std::string& file : loader::find_files_with_suffix(root, ".yaml")) {
        loader::YamlParseResult parsed = loader::parse_yaml_file(file);
        if (parsed.error.has_value()) {
            Json entry = Json::object();
            entry.set("file", file);
            entry.set("error", parsed.error->message);
            skipped.push(std::move(entry));
            continue;
        }
        const std::string file_dir = std::filesystem::path(file).parent_path().generic_string();
        if (loader::rewrite_ref_paths(parsed.root, file_dir, src, dst)) {
            if (auto error = base::write_file(file, loader::emit_yaml(parsed.root), "mv.io"))
                return refuse(Exit::Validation, std::move(*error));
            files_updated.push(file);
        }
    }

    const std::string cache_dir =
        args.present("cache-dir") ? args.get_string("cache-dir") : root + "/.midday-cache/uid";
    loader::BuildRegistryResult rebuilt = loader::build_uid_registry(root);
    if (rebuilt.error.has_value())
        return refuse(Exit::Validation, std::move(*rebuilt.error));
    if (auto error = loader::write_uid_cache(cache_dir + "/registry.json", rebuilt.registry))
        return refuse(Exit::Validation, std::move(*error));

    VerbOutcome out;
    out.payload.set("from", src);
    out.payload.set("to", dst);
    out.payload.set("sidecar_moved", sidecar_moved);
    out.payload.set("root", root);
    out.payload.set("cache_dir", cache_dir);
    out.payload.set("files_updated", std::move(files_updated));
    out.payload.set("skipped", std::move(skipped));
    out.human = src + " -> " + dst + " (" +
                std::to_string(out.payload.find("files_updated")->elements().size()) +
                " referencing file(s) updated)";
    return out;
}

constexpr FlagSpec kFlags[] = {
    {.name = "root",
     .type = "string",
     .doc = "directory to scan for referencing files (default: the current directory)"},
    {.name = "cache-dir",
     .type = "string",
     .doc = "uid registry cache directory (default: <root>/.midday-cache/uid)"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "src", .type = "string", .doc = "the asset's current path"},
    {.name = "dst", .type = "string", .doc = "the asset's new path"},
};

} // namespace

const VerbSpec& mv_spec() {
    static const VerbSpec spec{
        .name = "mv",
        .summary = "move an asset (+ its .uid sidecar) and rewrite referencing paths; the uid "
                   "never changes",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &mv_verb,
    };
    return spec;
}

} // namespace midday::cli

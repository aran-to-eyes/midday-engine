// core/loader/asset_ref.cpp — asset_ref.h: ref-shape matching, the
// check/fix classifier, and the mv path rewriter.

#include "core/loader/asset_ref.h"

#include <filesystem>

namespace midday::loader {
namespace {

// PURELY LEXICAL throughout this file (never std::filesystem::relative /
// canonical, which resolve symlinks): every path here is built by composing
// already-normalized absolute paths, so a lexical relative computation is
// both cheaper and immune to a registry entry (discovered by walking real
// files) and a ref's resolved path (which may not exist on disk at all, or
// may share a symlinked ancestor) disagreeing over the SAME logical path.
std::string resolve(const std::string& dir, const std::string& authored_path) {
    return (std::filesystem::path(dir) / authored_path).lexically_normal().generic_string();
}

std::string root_relative(const std::string& abs_path, const std::string& scan_root) {
    return std::filesystem::path(abs_path).lexically_relative(scan_root).generic_string();
}

std::string relative_to(const std::string& target_abs, const std::string& from_dir) {
    return std::filesystem::path(target_abs).lexically_relative(from_dir).generic_string();
}

struct AttachResult {
    Uid uid;
    std::optional<base::Error> error;
};

// The shared repair for kMissingUid and "kInvalid but the path resolves":
// reuse the sidecar already sitting at `asset_abs_path` if one exists,
// otherwise mint a fresh uid and write it there. Either way `registry`
// gains the entry so later refs in the SAME scan see it too.
AttachResult attach_correct_uid(const std::string& asset_abs_path,
                                const std::string& root_rel,
                                UidRegistry& registry,
                                UidRng& rng) {
    if (const std::uint64_t* existing = registry.value_for_path(root_rel))
        return {.uid = Uid::from_value(*existing), .error = std::nullopt};
    const Uid minted = mint_uid(registry.known_values(), rng);
    if (std::optional<base::Error> error =
            write_uid_sidecar(sidecar_path_for(asset_abs_path), minted))
        return {.uid = Uid{}, .error = std::move(error)};
    registry.add(minted.value(), root_rel);
    return {.uid = minted, .error = std::nullopt};
}

void insert_uid_field(YamlNode& map, const std::string& uid_text) {
    YamlNode value;
    value.kind = YamlNode::Kind::kScalar;
    value.scalar = uid_text;
    YamlEntry entry;
    entry.key = "uid";
    entry.value.push_back(std::move(value));
    map.map.insert(map.map.begin(), std::move(entry));
}

} // namespace

std::optional<RefFields> match_ref_shape(YamlNode& node) {
    if (!node.is_map() || node.map.empty() || node.map.size() > 2)
        return std::nullopt;
    RefFields fields;
    for (YamlEntry& entry : node.map) {
        if (entry.key == "path")
            fields.path = &entry.value.front();
        else if (entry.key == "uid")
            fields.uid = &entry.value.front();
        else
            return std::nullopt;
    }
    if (fields.path == nullptr || !fields.path->is_scalar())
        return std::nullopt;
    if (fields.uid != nullptr && !fields.uid->is_scalar())
        return std::nullopt;
    return fields;
}

namespace {

void collect_refs(YamlNode& node, std::vector<RefSite>& out) {
    if (std::optional<RefFields> fields = match_ref_shape(node)) {
        out.push_back(RefSite{.map = &node, .fields = *fields});
        return; // leaf shape — nothing more to find inside it
    }
    if (node.is_map()) {
        for (YamlEntry& entry : node.map)
            collect_refs(entry.value.front(), out);
    } else if (node.is_seq()) {
        for (YamlNode& element : node.seq)
            collect_refs(element, out);
    }
}

} // namespace

std::vector<RefSite> find_asset_refs(YamlNode& root) {
    std::vector<RefSite> out;
    collect_refs(root, out);
    return out;
}

ScanRefsResult scan_refs(YamlNode& doc_root,
                         const std::string& file,
                         const std::string& file_dir,
                         const std::string& scan_root,
                         UidRegistry& registry,
                         UidRng& rng,
                         bool fix) {
    ScanRefsResult result;
    for (RefSite& site : find_asset_refs(doc_root)) {
        RefFinding finding;
        finding.file = file;
        finding.line = site.map->line;
        finding.col = site.map->col;
        finding.path = site.fields.path->scalar; // captured before any mutation below
        const std::string authored_path = finding.path;
        const std::string resolved_abs = resolve(file_dir, authored_path);
        std::error_code exists_ec;
        const bool resolves = std::filesystem::is_regular_file(resolved_abs, exists_ec);
        const std::string rel = root_relative(resolved_abs, scan_root);

        if (site.fields.uid == nullptr) {
            if (!resolves) {
                finding.status = RefStatus::kInvalid;
                finding.detail = "path does not resolve to an existing asset";
            } else {
                finding.status = RefStatus::kMissingUid;
                finding.detail = "reference has no uid yet";
                if (fix) {
                    AttachResult attached = attach_correct_uid(resolved_abs, rel, registry, rng);
                    if (attached.error.has_value()) {
                        finding.detail += " (fix failed: " + attached.error->message + ")";
                    } else {
                        insert_uid_field(*site.map, attached.uid.text());
                        finding.uid_text = attached.uid.text();
                        finding.fixed = true;
                        result.changed = true;
                    }
                }
            }
            result.findings.push_back(std::move(finding));
            continue;
        }

        finding.uid_text = site.fields.uid->scalar;
        const std::optional<Uid> parsed_uid = parse_uid_text(site.fields.uid->scalar);
        const bool well_formed = parsed_uid.has_value() && parsed_uid->valid();
        const bool registered = well_formed && registry.has(parsed_uid->value());

        if (registered) {
            const std::string* known_path = registry.path_for(parsed_uid->value());
            if (*known_path == rel) {
                finding.status = RefStatus::kClean;
            } else {
                finding.status = RefStatus::kDrift;
                finding.detail = "asset moved to '" + *known_path + "'";
                if (fix) {
                    const std::string new_abs = (std::filesystem::path(scan_root) / *known_path)
                                                    .lexically_normal()
                                                    .generic_string();
                    site.fields.path->scalar = relative_to(new_abs, file_dir);
                    finding.fixed = true;
                    result.changed = true;
                }
            }
            result.findings.push_back(std::move(finding));
            continue;
        }

        // uid present but not registered: malformed text, or well-formed yet
        // unknown to any sidecar (the "hand-minted" refusal, spec 365-366).
        finding.status = RefStatus::kInvalid;
        finding.detail = well_formed ? "uid is not backed by any .uid sidecar (hand-minted?)"
                                     : "'" + finding.uid_text + "' is not a valid uid";
        if (fix && resolves) {
            AttachResult attached = attach_correct_uid(resolved_abs, rel, registry, rng);
            if (attached.error.has_value()) {
                finding.detail += " (fix failed: " + attached.error->message + ")";
            } else {
                site.fields.uid->scalar = attached.uid.text();
                finding.fixed = true;
                result.changed = true;
            }
        }
        result.findings.push_back(std::move(finding));
    }
    return result;
}

bool rewrite_ref_paths(YamlNode& doc_root,
                       const std::string& file_dir,
                       const std::string& moved_from_abs,
                       const std::string& moved_to_abs) {
    bool changed = false;
    for (RefSite& site : find_asset_refs(doc_root)) {
        const std::string resolved_abs = resolve(file_dir, site.fields.path->scalar);
        if (resolved_abs == moved_from_abs) {
            site.fields.path->scalar = relative_to(moved_to_abs, file_dir);
            changed = true;
        }
    }
    return changed;
}

} // namespace midday::loader

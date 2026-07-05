// core/loader/uid_registry.h — the uid<->path map (m1-uid-system), rebuilt
// by scanning every `.uid` sidecar under a project root; core/loader/
// asset_ref.h's `midday check`/`midday mv` are the only consumers. Unlike
// core/loader/loader.h's load_project_events (which MERGES events into a
// long-lived EventsDecl the caller keeps around), this registry has exactly
// one job per invocation: answer "what path does this uid own" / "does this
// path already have a uid" while a scan is in flight, then get serialized
// to the regenerable cache (write_uid_cache) and discarded. It is NEVER
// loaded back from the cache — the cache is write-only output, the
// sidecars on disk are the only source of truth (spec lines 366-368), which
// is exactly what makes "delete the cache, regenerate, compare" (the m1-
// uid-system exit test) trivially true: two independent builds from the
// SAME sidecars are a pure function of their content, sorted for
// determinism, never of filesystem walk order.

#pragma once

#include "core/base/error.h"
#include "core/loader/uid.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace midday::loader {

// Every file under `root_dir` (recursive) whose name ends in `suffix`,
// lexically-normalized generic-string paths, LEXICOGRAPHICALLY SORTED
// (filesystem walk order is not guaranteed stable across platforms — the
// events loader's load_project_events establishes the same discipline).
// Skips `.midday-cache/` and `build/` subtrees (cache and build output are
// never authored content).
[[nodiscard]] std::vector<std::string> find_files_with_suffix(const std::string& root_dir,
                                                              std::string_view suffix);

class UidRegistry {
public:
    // Registers uid `value` -> `root_relative_path`. Returns false without
    // mutating when `value` is already registered — path_for() then reports
    // which path it was already promised to (the caller's "uid.duplicate"
    // diagnostic).
    bool add(std::uint64_t value, std::string root_relative_path);

    [[nodiscard]] const std::string* path_for(std::uint64_t value) const;
    [[nodiscard]] const std::uint64_t* value_for_path(std::string_view root_relative_path) const;

    [[nodiscard]] bool has(std::uint64_t value) const { return by_value_.contains(value); }

    // Snapshot for mint_uid's collision check.
    [[nodiscard]] std::unordered_set<std::uint64_t> known_values() const;

    // (uid text, root-relative path) pairs, sorted by uid text — the cache's
    // serialization order, independent of insertion order.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> sorted_entries() const;

private:
    std::unordered_map<std::uint64_t, std::string> by_value_;
    std::unordered_map<std::string, std::uint64_t> by_path_;
};

struct BuildRegistryResult {
    UidRegistry registry;
    std::optional<base::Error> error; // loader.io (bad root) / a sidecar's own load error /
                                      // "uid.duplicate"
};

// PRECONDITION: `root_dir` is an ABSOLUTE, lexically-normalized path (the
// CLI verbs resolve it against the current directory exactly once, up
// front) — every path this function records is root_dir-relative, computed
// lexically (never std::filesystem::relative/canonical — see asset_ref.h),
// so an inconsistent root_dir would silently skew every comparison a
// caller makes against core/loader/asset_ref.h's ref-side path math.
//
// Scans every `*.uid` sidecar under `root_dir`, loading each
// (load_uid_sidecar) and registering it by its asset path (the sidecar path
// with the trailing ".uid" stripped), expressed root-relative. First
// failure wins (matches load_scene/load_project_events); two sidecars
// claiming the same uid value is "uid.duplicate", details {sidecar, uid,
// existing_path}.
BuildRegistryResult build_uid_registry(const std::string& root_dir);

// Serializes `registry` to `{"format": 1, "entries": [{"uid","path"}...]}`
// (sorted_entries() order) and writes it to `cache_path`, creating parent
// directories as needed. Purely regenerable output: no reader exists in
// this tree, and none should ever trust it over a fresh
// build_uid_registry() scan.
std::optional<base::Error> write_uid_cache(const std::string& cache_path,
                                           const UidRegistry& registry);

} // namespace midday::loader

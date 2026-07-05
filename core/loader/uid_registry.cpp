// core/loader/uid_registry.cpp — uid_registry.h: the sidecar scan + the
// regenerable cache writer.

#include "core/loader/uid_registry.h"

#include "core/base/file_io.h"
#include "core/base/json.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace midday::loader {
namespace {

constexpr std::string_view kSidecarSuffix = ".uid";

bool is_skipped_dir(const std::string& name) {
    return name == ".midday-cache" || name == "build";
}

} // namespace

std::vector<std::string> find_files_with_suffix(const std::string& root_dir,
                                                std::string_view suffix) {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root_dir, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& entry_path = it->path();
        std::error_code dir_ec;
        if (it->is_directory(dir_ec) && is_skipped_dir(entry_path.filename().string())) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec))
            continue;
        const std::string name = entry_path.filename().string();
        if (std::string_view(name).ends_with(suffix))
            out.push_back(entry_path.lexically_normal().generic_string());
    }
    std::ranges::sort(out);
    return out;
}

bool UidRegistry::add(std::uint64_t value, std::string root_relative_path) {
    if (by_value_.contains(value))
        return false;
    by_path_[root_relative_path] = value;
    by_value_.emplace(value, std::move(root_relative_path));
    return true;
}

const std::string* UidRegistry::path_for(std::uint64_t value) const {
    const auto it = by_value_.find(value);
    return it == by_value_.end() ? nullptr : &it->second;
}

const std::uint64_t* UidRegistry::value_for_path(std::string_view root_relative_path) const {
    const auto it = by_path_.find(std::string(root_relative_path));
    return it == by_path_.end() ? nullptr : &it->second;
}

std::unordered_set<std::uint64_t> UidRegistry::known_values() const {
    std::unordered_set<std::uint64_t> values;
    values.reserve(by_value_.size());
    for (const auto& entry : by_value_)
        values.insert(entry.first);
    return values;
}

std::vector<std::pair<std::string, std::string>> UidRegistry::sorted_entries() const {
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(by_value_.size());
    for (const auto& [value, path] : by_value_)
        entries.emplace_back(Uid::from_value(value).text(), path);
    std::ranges::sort(entries, {}, &std::pair<std::string, std::string>::first);
    return entries;
}

BuildRegistryResult build_uid_registry(const std::string& root_dir) {
    BuildRegistryResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(root_dir, ec)) {
        result.error = base::file_error("loader.io",
                                        "uid registry root '" + root_dir + "' is not a directory");
        return result;
    }
    for (const std::string& sidecar_path : find_files_with_suffix(root_dir, kSidecarSuffix)) {
        SidecarLoadResult loaded = load_uid_sidecar(sidecar_path);
        // Guard on `sidecar` itself (not `error`): load_uid_sidecar sets
        // exactly one of the two, but that cross-field invariant is only
        // known to THIS file's author, not to a dataflow-based checker —
        // guarding the SAME optional the next line dereferences is what
        // actually proves the access, and is defensively correct regardless.
        if (!loaded.sidecar.has_value()) {
            result.error = std::move(loaded.error);
            return result;
        }
        const Uid uid = loaded.sidecar->uid;
        const std::string asset_path =
            sidecar_path.substr(0, sidecar_path.size() - kSidecarSuffix.size());
        // Lexical, not std::filesystem::relative: stays consistent with
        // core/loader/asset_ref.h's ref-side path math, which cannot use
        // canonical() (a dangling ref's path need not exist on disk).
        const std::string root_relative =
            std::filesystem::path(asset_path).lexically_relative(root_dir).generic_string();
        if (!result.registry.add(uid.value(), root_relative)) {
            const std::string* existing = result.registry.path_for(uid.value());
            base::Error error{.code = "uid.duplicate",
                              .message = sidecar_path + ": uid " + uid.text() +
                                         " is already registered to '" +
                                         (existing != nullptr ? *existing : std::string()) + "'"};
            error.details.set("sidecar", sidecar_path);
            error.details.set("uid", uid.text());
            error.details.set("existing_path", existing != nullptr ? *existing : std::string());
            result.error = std::move(error);
            return result;
        }
    }
    return result;
}

std::optional<base::Error> write_uid_cache(const std::string& cache_path,
                                           const UidRegistry& registry) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cache_path).parent_path(), ec);

    base::Json doc = base::Json::object();
    doc.set("format", std::int64_t{1});
    base::Json entries = base::Json::array();
    for (const auto& [uid_text, path] : registry.sorted_entries()) {
        base::Json entry = base::Json::object();
        entry.set("uid", uid_text);
        entry.set("path", path);
        entries.push(std::move(entry));
    }
    doc.set("entries", std::move(entries));
    return base::write_file(cache_path, doc.dump() + "\n", "uid.cache_io");
}

} // namespace midday::loader

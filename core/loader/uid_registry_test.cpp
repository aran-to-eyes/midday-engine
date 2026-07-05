// loader.uid_registry — the sidecar scan, duplicate detection, and the
// regenerable cache writer (uid_registry.h).

#include "core/base/file_io.h"
#include "core/loader/uid_registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <filesystem>
#include <string>
#include <unordered_set>

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

// `dir.file(name)` alone does not create `name`'s parent directories; the
// fixtures below need nested paths ("assets/a.bin.uid", ".midday-cache/uid/
// registry.json").
std::string put(const testkit::TempDir& dir, const std::string& relative, std::string_view text) {
    const std::string path = dir.file(relative);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
    return path;
}

} // namespace

TEST_CASE("loader.uid_registry: find_files_with_suffix sorts and skips cache/build subtrees") {
    testkit::TempDir dir{"uid-registry-walk"};
    put(dir, "b.uid", "");
    put(dir, "a.uid", "");
    put(dir, "nested/c.uid", "");
    put(dir, ".midday-cache/skip.uid", "");
    put(dir, "build/skip.uid", "");
    put(dir, "a.uid.txt", ""); // does not END in .uid

    const std::vector<std::string> found =
        find_files_with_suffix(dir.path.generic_string(), ".uid");
    REQUIRE(found.size() == 3);
    // generic_string() throughout: find_files_with_suffix always returns
    // forward-slash paths (its real callers, core/loader/asset_ref.h, compare
    // them against other generic_string()-derived paths), so the expected
    // values must be built the same way rather than via TempDir::file()'s
    // native (backslash-on-Windows) separators.
    CHECK(found[0] == (dir.path / "a.uid").generic_string());
    CHECK(found[1] == (dir.path / "b.uid").generic_string());
    CHECK(found[2] == (dir.path / "nested/c.uid").generic_string());
}

TEST_CASE("loader.uid_registry: UidRegistry add/lookup/duplicate") {
    UidRegistry registry;
    CHECK(registry.add(1, "a.bin"));
    CHECK(registry.add(2, "b.bin"));
    CHECK_FALSE(registry.add(1, "c.bin")); // duplicate value refused
    REQUIRE(registry.path_for(1) != nullptr);
    CHECK(*registry.path_for(1) == "a.bin"); // untouched by the refused add
    CHECK(registry.path_for(99) == nullptr);
    REQUIRE(registry.value_for_path("b.bin") != nullptr);
    CHECK(*registry.value_for_path("b.bin") == 2);
    CHECK(registry.value_for_path("nope") == nullptr);
    CHECK(registry.has(1));
    CHECK_FALSE(registry.has(3));
    const std::unordered_set<std::uint64_t> known = registry.known_values();
    CHECK(known.size() == 2);
    CHECK(known.contains(1));
    CHECK(known.contains(2));
}

TEST_CASE("loader.uid_registry: sorted_entries orders by uid text, not insertion order") {
    UidRegistry registry;
    registry.add(Uid::from_value(999).value(), "z.bin");
    registry.add(Uid::from_value(1).value(), "a.bin");
    const auto entries = registry.sorted_entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].first == Uid::from_value(1).text());
    CHECK(entries[0].second == "a.bin");
    CHECK(entries[1].first == Uid::from_value(999).text());
}

TEST_CASE("loader.uid_registry: build_uid_registry scans sidecars into root-relative entries") {
    testkit::TempDir dir{"uid-registry-build"};
    std::filesystem::create_directories(
        dir.file("assets")); // write_uid_sidecar needs the dir to exist
    REQUIRE_FALSE(write_uid_sidecar(dir.file("assets/a.bin.uid"), Uid::from_value(11)).has_value());
    REQUIRE_FALSE(write_uid_sidecar(dir.file("assets/b.bin.uid"), Uid::from_value(22)).has_value());

    BuildRegistryResult result = build_uid_registry(dir.path.generic_string());
    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.registry.value_for_path("assets/a.bin") != nullptr);
    CHECK(*result.registry.value_for_path("assets/a.bin") == 11);
    REQUIRE(result.registry.path_for(22) != nullptr);
    CHECK(*result.registry.path_for(22) == "assets/b.bin");
}

TEST_CASE("loader.uid_registry: build_uid_registry refuses a not-a-directory root") {
    testkit::TempDir dir{"uid-registry-noroot"};
    BuildRegistryResult result = build_uid_registry(dir.file("does_not_exist"));
    CHECK(unwrap(result.error).code == "loader.io");
}

TEST_CASE("loader.uid_registry: build_uid_registry refuses two sidecars sharing one uid") {
    testkit::TempDir dir{"uid-registry-dup"};
    REQUIRE_FALSE(write_uid_sidecar(dir.file("a.bin.uid"), Uid::from_value(5)).has_value());
    REQUIRE_FALSE(write_uid_sidecar(dir.file("b.bin.uid"), Uid::from_value(5)).has_value());

    BuildRegistryResult result = build_uid_registry(dir.path.generic_string());
    CHECK(unwrap(result.error).code == "uid.duplicate");
}

TEST_CASE("loader.uid_registry: write_uid_cache is deterministic and re-derivable") {
    testkit::TempDir dir{"uid-registry-cache"};
    UidRegistry registry;
    registry.add(Uid::from_value(2).value(), "b.bin");
    registry.add(Uid::from_value(1).value(), "a.bin");
    const std::string cache_path = dir.file(".midday-cache/uid/registry.json");
    REQUIRE_FALSE(write_uid_cache(cache_path, registry).has_value());

    base::ReadFileResult first = base::read_file(cache_path, "test.io");
    REQUIRE_FALSE(first.error.has_value());

    // Two INDEPENDENT writes from equivalent registries (never a self-diff
    // against the same in-memory object) byte-compare identical.
    UidRegistry rebuilt;
    rebuilt.add(Uid::from_value(1).value(), "a.bin");
    rebuilt.add(Uid::from_value(2).value(), "b.bin");
    const std::string rerun_path = dir.file("rerun.json");
    REQUIRE_FALSE(write_uid_cache(rerun_path, rebuilt).has_value());
    base::ReadFileResult second = base::read_file(rerun_path, "test.io");
    REQUIRE_FALSE(second.error.has_value());
    CHECK(first.bytes == second.bytes);
    CHECK(first.bytes == "{\"format\":1,\"entries\":[{\"uid\":\"" + Uid::from_value(1).text() +
                             "\",\"path\":\"a.bin\"},"
                             "{\"uid\":\"" +
                             Uid::from_value(2).text() + "\",\"path\":\"b.bin\"}]}\n");
}

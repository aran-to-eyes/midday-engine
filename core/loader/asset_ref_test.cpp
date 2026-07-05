// loader.asset_ref — ref-shape matching, the check/fix classifier, and the
// mv path rewriter (asset_ref.h).

#include "core/base/file_io.h"
#include "core/loader/asset_ref.h"
#include "core/loader/yaml.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <filesystem>
#include <string>

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

YamlNode parse(const std::string& text) {
    YamlParseResult result = parse_yaml(text, "t.yaml");
    REQUIRE_FALSE(result.error.has_value());
    return result.root;
}

// `dir.file(name)` alone does not create `name`'s parent directories.
void put(const testkit::TempDir& dir, const std::string& relative, std::string_view text) {
    const std::string path = dir.file(relative);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
}

} // namespace

TEST_CASE("loader.asset_ref: match_ref_shape accepts exactly {path} or {uid, path}") {
    YamlNode path_only = parse("path: a.bin\n");
    std::optional<RefFields> path_only_fields = match_ref_shape(path_only);
    CHECK(unwrap(path_only_fields).uid == nullptr);

    YamlNode dual = parse("uid: uid://1\npath: a.bin\n");
    std::optional<RefFields> fields = match_ref_shape(dual);
    REQUIRE(unwrap(fields).uid != nullptr);
    CHECK(unwrap(fields).uid->scalar == "uid://1");

    YamlNode missing_path = parse("uid: uid://1\n");
    CHECK_FALSE(match_ref_shape(missing_path).has_value());
    YamlNode stray_key = parse("path: a.bin\nextra: x\n");
    CHECK_FALSE(match_ref_shape(stray_key).has_value());
    YamlNode non_scalar_path = parse("path: {x: 1}\n");
    CHECK_FALSE(match_ref_shape(non_scalar_path).has_value());
    YamlNode not_a_map = parse("[]\n");
    CHECK_FALSE(match_ref_shape(not_a_map).has_value());
}

TEST_CASE(
    "loader.asset_ref: find_asset_refs recurses through maps and sequences, never into a ref") {
    YamlNode root = parse("sprite: {path: a.bin}\n"
                          "list:\n"
                          "  - {uid: uid://2, path: b.bin}\n"
                          "  - other: 1\n");
    std::vector<RefSite> sites = find_asset_refs(root);
    REQUIRE(sites.size() == 2);
    CHECK(sites[0].fields.path->scalar == "a.bin");
    CHECK(sites[1].fields.path->scalar == "b.bin");
}

TEST_CASE("loader.asset_ref: scan_refs — clean, drift (with fix), and missing_uid (with fix)") {
    testkit::TempDir dir{"asset-ref-scan"};
    const std::string root = dir.path.generic_string();
    put(dir, "assets/known.bin", "x");
    put(dir, "assets/moved.bin", "x");
    put(dir, "assets/fresh.bin", "x");
    UidRegistry registry;
    registry.add(Uid::from_value(42).value(), "assets/known.bin");
    registry.add(Uid::from_value(99).value(), "assets/moved.bin"); // the CURRENT (post-move) truth

    YamlNode doc = parse("clean_ref: {uid: " + Uid::from_value(42).text() +
                         ", path: assets/known.bin}\n"
                         "stale_ref: {uid: " +
                         Uid::from_value(99).text() +
                         ", path: assets/old_location.bin}\n" // authored path is stale
                         "fresh_ref: {path: assets/fresh.bin}\n");
    UidRng rng(1);
    ScanRefsResult result = scan_refs(doc, "doc.yaml", root, root, registry, rng, /*fix=*/true);
    REQUIRE(result.findings.size() == 3);
    CHECK(result.changed);

    CHECK(result.findings[0].status == RefStatus::kClean);
    CHECK_FALSE(result.findings[0].fixed);

    CHECK(result.findings[1].status == RefStatus::kDrift);
    CHECK(result.findings[1].fixed);
    CHECK(result.findings[1].uid_text == Uid::from_value(99).text()); // uid never touched

    CHECK(result.findings[2].status == RefStatus::kMissingUid);
    CHECK(result.findings[2].fixed);
    CHECK_FALSE(result.findings[2].uid_text.empty());

    // The tree itself was mutated in place: re-fetch the sites and check.
    std::vector<RefSite> sites = find_asset_refs(doc);
    REQUIRE(sites.size() == 3);
    CHECK(sites[1].fields.path->scalar == "assets/moved.bin"); // rewritten from the registry
    CHECK(sites[1].fields.uid->scalar == Uid::from_value(99).text());
    REQUIRE(sites[2].fields.uid != nullptr);
    CHECK(sites[2].fields.uid->scalar == result.findings[2].uid_text);

    // The freshly-minted uid got a real sidecar, and the registry now knows it.
    CHECK(std::filesystem::exists(dir.file("assets/fresh.bin.uid")));
    CHECK(registry.value_for_path("assets/fresh.bin") != nullptr);
}

TEST_CASE("loader.asset_ref: scan_refs reports a hand-minted uid as kInvalid and check-only never "
          "mutates") {
    testkit::TempDir dir{"asset-ref-invalid"};
    put(dir, "assets/real.bin", "x");
    UidRegistry registry; // deliberately empty: nothing is registered
    YamlNode doc = parse("bad_ref: {uid: uid://0000000000001, path: assets/real.bin}\n");
    UidRng rng(2);
    const std::string root = dir.path.generic_string();

    ScanRefsResult report_only =
        scan_refs(doc, "doc.yaml", root, root, registry, rng, /*fix=*/false);
    REQUIRE(report_only.findings.size() == 1);
    CHECK(report_only.findings[0].status == RefStatus::kInvalid);
    CHECK_FALSE(report_only.findings[0].fixed);
    CHECK_FALSE(report_only.changed);
    CHECK(find_asset_refs(doc)[0].fields.uid->scalar == "uid://0000000000001"); // untouched

    // With --fix and a resolving path, the bogus uid self-heals to a real one.
    ScanRefsResult fixed = scan_refs(doc, "doc.yaml", root, root, registry, rng, /*fix=*/true);
    REQUIRE(fixed.findings.size() == 1);
    CHECK(fixed.findings[0].status == RefStatus::kInvalid); // diagnosis is the ORIGINAL problem
    CHECK(fixed.findings[0].fixed);                         // but it WAS repaired
    CHECK(find_asset_refs(doc)[0].fields.uid->scalar != "uid://0000000000001");
}

TEST_CASE("loader.asset_ref: scan_refs cannot fix a ref whose path does not resolve at all") {
    testkit::TempDir dir{"asset-ref-dangling"};
    UidRegistry registry;
    YamlNode doc = parse("dangling: {path: assets/nowhere.bin}\n");
    UidRng rng(3);
    const std::string root = dir.path.generic_string();
    ScanRefsResult result = scan_refs(doc, "doc.yaml", root, root, registry, rng, /*fix=*/true);
    REQUIRE(result.findings.size() == 1);
    CHECK(result.findings[0].status == RefStatus::kInvalid);
    CHECK_FALSE(result.findings[0].fixed);
    CHECK_FALSE(result.changed);
}

TEST_CASE("loader.asset_ref: rewrite_ref_paths retargets only the moved asset, leaving uid alone") {
    testkit::TempDir dir{"asset-ref-mv"};
    YamlNode doc = parse("moved: {uid: uid://1, path: old/name.bin}\n"
                         "untouched: {path: other.bin}\n");
    const std::string root = dir.path.generic_string();
    // generic_string(), not TempDir::file()'s native separators: check/mv
    // (asset_ref.h's real callers) always deal in forward-slash paths, and a
    // backslash-vs-slash mismatch on Windows would make the equality tests
    // below fail for the wrong reason.
    const std::string from_abs = (dir.path / "old/name.bin").lexically_normal().generic_string();
    const std::string to_abs = (dir.path / "new/name.bin").lexically_normal().generic_string();

    const bool changed = rewrite_ref_paths(doc, root, from_abs, to_abs);
    CHECK(changed);

    std::vector<RefSite> sites = find_asset_refs(doc);
    REQUIRE(sites.size() == 2);
    CHECK(sites[0].fields.path->scalar == "new/name.bin");
    CHECK(sites[0].fields.uid->scalar == "uid://1");    // never touched
    CHECK(sites[1].fields.path->scalar == "other.bin"); // untouched ref stays untouched

    const bool unchanged_second_pass = rewrite_ref_paths(doc, root, from_abs, to_abs);
    CHECK_FALSE(unchanged_second_pass); // nothing left pointing at the old path
}

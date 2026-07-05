// loader.uid — textual round-trip, minting collision-avoidance, and .uid
// sidecar I/O (uid.h).

#include "core/base/file_io.h"
#include "core/loader/uid.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <string>
#include <unordered_set>

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

TEST_CASE("loader.uid: text is uid:// + 13 zero-padded lowercase base-36 digits") {
    CHECK(Uid::from_value(1).text() == "uid://0000000000001");
    CHECK(Uid::from_value(0).text() == "uid://0000000000000");
    CHECK(Uid::from_value(0xFFFFFFFFFFFFFFFFULL).text() == "uid://3w5e11264sgsf");
}

TEST_CASE("loader.uid: parse is the exact inverse of text() over round-tripped values") {
    for (const std::uint64_t value : {0ULL, 1ULL, 35ULL, 36ULL, 0x7FFFFFFFFFFFFFFFULL, ~0ULL}) {
        const Uid uid = Uid::from_value(value);
        const std::optional<Uid> parsed = parse_uid_text(uid.text());
        CHECK(unwrap(parsed).value() == value);
    }
}

TEST_CASE("loader.uid: parse is permissive on width — a hand-typed 'uid://1' is valid syntax") {
    const std::optional<Uid> parsed = parse_uid_text("uid://1");
    CHECK(unwrap(parsed).value() == 1);
}

TEST_CASE("loader.uid: parse refuses a bad prefix, bad alphabet, empty body, or overflow") {
    CHECK_FALSE(parse_uid_text("not-a-uid").has_value());
    CHECK_FALSE(parse_uid_text("uid:/abc").has_value());
    CHECK_FALSE(parse_uid_text("uid://").has_value());
    CHECK_FALSE(parse_uid_text("uid://ABC").has_value()); // uppercase is out of alphabet
    CHECK_FALSE(parse_uid_text("uid://_").has_value());
    // 13 'z's overflows 64 bits (36^13 - 1 > 2^64 - 1).
    CHECK_FALSE(parse_uid_text("uid://zzzzzzzzzzzzz").has_value());
}

TEST_CASE("loader.uid: mint_uid never returns 0 and retries around a collision") {
    UidRng rng(42); // fixed seed: deterministic doctest, never production entropy
    std::unordered_set<std::uint64_t> taken;
    for (int i = 0; i < 500; ++i) {
        const Uid minted = mint_uid(taken, rng);
        REQUIRE(minted.valid());
        REQUIRE_FALSE(taken.contains(minted.value()));
        taken.insert(minted.value());
    }
    // Force a collision path: seed a generator to draw a KNOWN value first,
    // pre-populate `taken` with it, and confirm mint_uid does not return it.
    UidRng forced(7);
    std::uniform_int_distribution<std::uint64_t> probe(1, 0x7FFFFFFFFFFFFFFFULL);
    UidRng probe_rng(7);
    const std::uint64_t first_draw = probe(probe_rng);
    std::unordered_set<std::uint64_t> blocked = {first_draw};
    const Uid minted = mint_uid(blocked, forced);
    CHECK(minted.value() != first_draw);
}

TEST_CASE("loader.uid: sidecar round-trip — write then load recovers the same uid") {
    testkit::TempDir dir{"uid-sidecar"};
    const std::string path = dir.file("asset.bin.uid");
    const Uid uid = Uid::from_value(123456789);
    REQUIRE_FALSE(write_uid_sidecar(path, uid).has_value());

    base::ReadFileResult raw = base::read_file(path, "test.io");
    REQUIRE_FALSE(raw.error.has_value());
    CHECK(raw.bytes == "format: 1\nuid: " + uid.text() + "\n");

    SidecarLoadResult loaded = load_uid_sidecar(path);
    REQUIRE_FALSE(loaded.error.has_value());
    CHECK(unwrap(loaded.sidecar).uid.value() == uid.value());
}

TEST_CASE("loader.uid: sidecar_path_for appends .uid verbatim") {
    CHECK(sidecar_path_for("assets/sprite.png") == "assets/sprite.png.uid");
}

TEST_CASE("loader.uid: a malformed sidecar refuses with a structured, located error") {
    testkit::TempDir dir{"uid-sidecar-bad"};

    auto write = [&](std::string_view name, std::string_view text) {
        const std::string path = dir.file(std::string(name));
        REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
        return path;
    };

    SidecarLoadResult missing_format =
        load_uid_sidecar(write("no_format.uid", "uid: uid://0000000000001\n"));
    CHECK(unwrap(missing_format.error).code == "loader.bad_format");

    SidecarLoadResult unknown_key =
        load_uid_sidecar(write("extra_key.uid", "format: 1\nuid: uid://0000000000001\nnote: x\n"));
    CHECK(unwrap(unknown_key.error).code == "loader.unknown_key");

    SidecarLoadResult missing_uid = load_uid_sidecar(write("no_uid.uid", "format: 1\n"));
    CHECK(unwrap(missing_uid.error).code == "loader.bad_value");

    SidecarLoadResult bad_text =
        load_uid_sidecar(write("bad_uid.uid", "format: 1\nuid: not-a-uid\n"));
    CHECK(unwrap(bad_text.error).code == "uid.malformed");
    CHECK(unwrap(bad_text.error).message.starts_with(dir.file("bad_uid.uid") + ":2:"));
}

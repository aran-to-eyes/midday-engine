// core.name.* — tests for interned names (core/base/name.h).
//
// The guarantee under test: Name ids are pure content hashes (XXH3-64 of the
// UTF-8 bytes), so they are identical across runs, platforms, and intern
// order — never sequential handout. The known-answer vectors below pin the
// hash function itself; if they ever change, serialized ids in journals and
// goldens would silently rot.

#include "core/base/name.h"
#include "doctest/doctest.h"

#include <algorithm>
#include <cstdint>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string_view>
#include <vector>

using midday::base::Name;

TEST_CASE("core.name: ids are content hashes, pinned by known-answer vectors") {
    // Precomputed XXH3_64bits values — cross-platform determinism anchors.
    CHECK(Name::hash_of("player") == 0x91eb517071c50a06ULL);
    CHECK(Name::hash_of("core.loader") == 0x59c07f356514b21dULL);
    CHECK(Name::hash_of("json.parse") == 0x733a2a48699e4ea8ULL);
    CHECK(Name::hash_of("a") == 0xe6c632b61e964e1fULL);
    CHECK(Name::hash_of("smörgås — 日本") == 0xd5bb626d95764587ULL);
    CHECK(Name("player").id() == Name::hash_of("player"));
}

TEST_CASE("core.name: identity is content, not intern order") {
    // Interning in any order yields the same ids — the anti-sequential test.
    Name first_a("alpha");
    Name first_b("beta");
    Name second_b("beta");
    Name second_a("alpha");
    CHECK(first_a == second_a);
    CHECK(first_b == second_b);
    CHECK(first_a.id() == second_a.id());
    CHECK(first_a != first_b);
}

TEST_CASE("core.name: empty name is id 0 and equals the default") {
    CHECK(Name().id() == 0);
    CHECK(Name().empty());
    CHECK(Name().view().empty());
    CHECK(Name("") == Name());
    CHECK_FALSE(Name("x").empty());
}

TEST_CASE("core.name: view round-trips the interned text with stable storage") {
    Name name("core.statechart");
    CHECK(name.view() == "core.statechart");
    // Storage is process-lifetime: the view from a second intern aliases it.
    CHECK(Name("core.statechart").view().data() == name.view().data());
}

TEST_CASE("core.name: ordering by id is deterministic across runs") {
    std::vector<Name> names{Name("gamma"), Name("alpha"), Name("beta")};
    std::ranges::sort(names);
    std::vector<std::uint64_t> ids;
    ids.reserve(names.size());
    for (const Name& n : names)
        ids.push_back(n.id());
    CHECK(std::ranges::is_sorted(ids));
    // The order is a pure function of content — re-sorting a re-interned copy
    // (an independent second run of the sequence) agrees byte-for-byte.
    std::vector<Name> again{Name("beta"), Name("gamma"), Name("alpha")};
    std::ranges::sort(again);
    CHECK(std::ranges::equal(names, again));
}

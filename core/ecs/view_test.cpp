// ecs.view — default-active filtering, the explicit include_inactive
// opt-in, multi-component joins, and mutation through the visit callback
// (core/ecs/view.h).

#include "core/base/name.h"
#include "core/ecs/world.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#include <cstdint>
#include <optional>
#include <vector>

using midday::base::Name;
using midday::ecs::EntityRef;
using midday::ecs::World;
using midday::reflect::ClassDesc;
using midday::reflect::Registry;

namespace {

struct Position {
    float x = 0;
    float y = 0;
};

struct Velocity {
    float dx = 0;
    float dy = 0;
};

ClassDesc component_class(const char* name) {
    ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

// A world with Position on entities 0..4 (entity 2 inactive) and Velocity
// on entities 1, 2, 3 (entity 3's velocity inactive).
struct Fixture {
    Registry registry;
    World world{registry};
    std::vector<EntityRef> entities;

    Fixture() {
        world.register_component<Position>(component_class("Position"));
        world.register_component<Velocity>(component_class("Velocity"));
        for (std::uint32_t i = 0; i < 5; ++i) {
            const EntityRef e = world.spawn();
            entities.push_back(e);
            REQUIRE_FALSE(world.emplace(e, Position{float(i), 0}, i != 2));
        }
        for (std::uint32_t i : {1u, 2u, 3u})
            REQUIRE_FALSE(world.emplace(entities[i], Velocity{float(i), 0}, i != 3));
    }
};

} // namespace

TEST_CASE("ecs.view: default iteration visits ACTIVE rows only") {
    Fixture f;
    std::vector<std::uint32_t> visited;
    f.world.view<Position>().each([&](EntityRef ref, Position&) { visited.push_back(ref.index); });
    CHECK(visited == std::vector<std::uint32_t>{0, 1, 3, 4}); // entity 2 inactive

    std::vector<std::uint32_t> all;
    f.world.view<Position>().include_inactive().each(
        [&](EntityRef ref, Position&) { all.push_back(ref.index); });
    CHECK(all == std::vector<std::uint32_t>{0, 1, 2, 3, 4});
}

TEST_CASE("ecs.view: multi-component join requires EVERY component active") {
    Fixture f;
    std::vector<std::uint32_t> visited;
    f.world.view<Position, Velocity>().each(
        [&](EntityRef ref, Position&, Velocity&) { visited.push_back(ref.index); });
    // Entity 2: inactive Position. Entity 3: inactive Velocity. Only 1 joins.
    CHECK(visited == std::vector<std::uint32_t>{1});

    std::vector<std::uint32_t> all;
    f.world.view<Position, Velocity>().include_inactive().each(
        [&](EntityRef ref, Position&, Velocity&) { all.push_back(ref.index); });
    CHECK(all == std::vector<std::uint32_t>{1, 2, 3}); // Velocity drives (smaller pool)
}

TEST_CASE("ecs.view: callbacks mutate components in place") {
    Fixture f;
    f.world.view<Position, Velocity>().include_inactive().each(
        [](EntityRef, Position& p, Velocity& v) { p.x += v.dx * 10; });
    CHECK(f.world.try_get<Position>(f.entities[1])->x == doctest::Approx(11.0F));
    CHECK(f.world.try_get<Position>(f.entities[0])->x == doctest::Approx(0.0F));
}

TEST_CASE("ecs.view: toggling activity mid-iteration is allowed and moves nothing") {
    Fixture f;
    const Position* row_anchor = f.world.try_get<Position>(f.entities[4]);
    f.world.view<Position>().each([&](EntityRef ref, Position&) {
        CHECK(f.world.iterating());
        // Toggle a row's activity from inside the loop: legal, bit-only.
        CHECK_FALSE(f.world.set_active<Position>(f.entities[4], ref.index == 0));
    });
    CHECK_FALSE(f.world.iterating());
    CHECK(f.world.try_get<Position>(f.entities[4]) == row_anchor);
    // Last visited row was entity 4 itself (index != 0): ends inactive.
    CHECK(f.world.is_active<Position>(f.entities[4]) == std::optional<bool>{false});
}

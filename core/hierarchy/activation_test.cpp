// hierarchy.activation — subtree deactivation removes descendants from
// active ECS queries with ZERO memory movement (pointer stability pinned on
// pool data() addresses), reactivation restores the exact per-component
// active pattern, scopes nest (outer shadows inner, inner state preserved),
// and reparenting across dormancy boundaries applies the cover delta
// (core/hierarchy/activation.cpp).

#include "core/base/name.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using midday::base::Error;
using midday::base::Name;
using midday::ecs::EntityRef;
using midday::ecs::World;
using midday::hierarchy::Hierarchy;
using midday::reflect::ClassDesc;
using midday::reflect::Registry;

namespace {

struct Health {
    std::uint32_t value = 100;
};

struct Shield {
    std::uint32_t value = 0;
};

ClassDesc component_class(const char* name) {
    ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

std::string code_of(const std::optional<Error>& error) {
    return error.has_value() ? error->code : std::string("<none>");
}

std::vector<EntityRef> visited(World& world) {
    std::vector<EntityRef> out;
    world.view<Health>().each([&](EntityRef e, Health&) { out.push_back(e); });
    return out;
}

} // namespace

TEST_CASE("hierarchy.activation: deactivate hides the subtree, memory never moves") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));
    Hierarchy hierarchy(world);

    // root -> (child -> leaf); outsider stays visible throughout.
    const EntityRef root = world.spawn();
    const EntityRef child = world.spawn();
    const EntityRef leaf = world.spawn();
    const EntityRef outsider = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(root));
    REQUIRE_FALSE(hierarchy.adopt(outsider));
    REQUIRE_FALSE(hierarchy.queue_attach(child, root));
    REQUIRE_FALSE(hierarchy.queue_attach(leaf, child));
    REQUIRE_FALSE(world.flush_structural());
    for (const EntityRef e : {root, child, leaf, outsider})
        REQUIRE_FALSE(world.emplace(e, Health{e.index + 1}));

    REQUIRE(visited(world).size() == 4);
    const Health* healths_before = world.pool<Health>().data().data();
    const Health* leaf_value_before = world.try_get<Health>(leaf);

    REQUIRE_FALSE(hierarchy.deactivate(root));
    CHECK(visited(world) == std::vector<EntityRef>{outsider});
    CHECK(hierarchy.is_dormant(root));
    CHECK(hierarchy.is_dormant(leaf));
    CHECK_FALSE(hierarchy.is_dormant(outsider));

    // Zero memory movement: same array, same addresses, same values.
    CHECK(world.pool<Health>().data().data() == healths_before);
    CHECK(world.try_get<Health>(leaf) == leaf_value_before);
    CHECK(world.try_get<Health>(leaf)->value == leaf.index + 1);

    REQUIRE_FALSE(hierarchy.activate(root));
    CHECK(visited(world).size() == 4);
    CHECK(world.pool<Health>().data().data() == healths_before);

    // Scope bookkeeping is structured, not silent.
    CHECK(code_of(hierarchy.activate(root)) == "hierarchy.not_deactivated");
    REQUIRE_FALSE(hierarchy.deactivate(root));
    CHECK(code_of(hierarchy.deactivate(root)) == "hierarchy.already_deactivated");
    REQUIRE_FALSE(hierarchy.activate(root));
}

TEST_CASE("hierarchy.activation: reactivation restores the EXACT per-component pattern") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));
    world.register_component<Shield>(component_class("Shield"));
    Hierarchy hierarchy(world);

    const EntityRef root = world.spawn();
    const EntityRef child = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(root));
    REQUIRE_FALSE(hierarchy.queue_attach(child, root));
    REQUIRE_FALSE(world.flush_structural());
    // Mixed pattern: child's Shield is ALREADY inactive (a state-owned
    // component of a currently-inactive state) before the subtree sleeps.
    REQUIRE_FALSE(world.emplace(root, Health{1}));
    REQUIRE_FALSE(world.emplace(child, Health{2}));
    REQUIRE_FALSE(world.emplace(child, Shield{3}, /*active=*/false));

    REQUIRE_FALSE(hierarchy.deactivate(root));
    CHECK(world.is_active<Health>(child) == false);
    CHECK(world.is_active<Shield>(child) == false);

    REQUIRE_FALSE(hierarchy.activate(root));
    // Bit-identical restore: Health returns active, Shield STAYS inactive —
    // no blanket-activate.
    CHECK(world.is_active<Health>(root) == true);
    CHECK(world.is_active<Health>(child) == true);
    CHECK(world.is_active<Shield>(child) == false);
}

TEST_CASE("hierarchy.activation: outer scopes shadow inner ones; inner state survives") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));
    Hierarchy hierarchy(world);

    // outer -> mid -> (inner -> leaf): inner is its own scope root.
    const EntityRef outer = world.spawn();
    const EntityRef mid = world.spawn();
    const EntityRef inner = world.spawn();
    const EntityRef leaf = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(outer));
    REQUIRE_FALSE(hierarchy.queue_attach(mid, outer));
    REQUIRE_FALSE(hierarchy.queue_attach(inner, mid));
    REQUIRE_FALSE(hierarchy.queue_attach(leaf, inner));
    REQUIRE_FALSE(world.flush_structural());
    REQUIRE_FALSE(world.emplace(mid, Health{1}));
    REQUIRE_FALSE(world.emplace(leaf, Health{2}, /*active=*/false)); // pre-inactive

    REQUIRE_FALSE(hierarchy.deactivate(inner));
    REQUIRE_FALSE(hierarchy.deactivate(outer));

    // Lifting the INNER scope under a dormant outer changes nothing yet.
    REQUIRE_FALSE(hierarchy.activate(inner));
    CHECK(hierarchy.is_dormant(leaf));
    CHECK(world.is_active<Health>(mid) == false);

    // Lifting the OUTER scope restores everything to the ORIGINAL pattern —
    // including the leaf's pre-inactive Health, captured by the inner scope.
    REQUIRE_FALSE(hierarchy.activate(outer));
    CHECK_FALSE(hierarchy.is_dormant(mid));
    CHECK_FALSE(hierarchy.is_dormant(leaf));
    CHECK(world.is_active<Health>(mid) == true);
    CHECK(world.is_active<Health>(leaf) == false); // exact, not blanket

    // Mirror order: outer lifted first, inner still holds its subtree.
    REQUIRE_FALSE(hierarchy.deactivate(inner));
    REQUIRE_FALSE(hierarchy.deactivate(outer));
    REQUIRE_FALSE(hierarchy.activate(outer));
    CHECK(world.is_active<Health>(mid) == true); // outside inner: restored
    CHECK(hierarchy.is_dormant(leaf));           // inner scope still covers
    REQUIRE_FALSE(hierarchy.activate(inner));
    CHECK(world.is_active<Health>(leaf) == false); // original pattern again
}

TEST_CASE("hierarchy.activation: reparenting across dormancy boundaries applies the delta") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));
    Hierarchy hierarchy(world);

    const EntityRef sleeper = world.spawn();
    const EntityRef mover = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(sleeper));
    REQUIRE_FALSE(hierarchy.adopt(mover));
    REQUIRE_FALSE(world.emplace(mover, Health{9}));
    REQUIRE_FALSE(hierarchy.deactivate(sleeper));

    // Move into the dormant region: mover sleeps (captured)...
    REQUIRE_FALSE(hierarchy.queue_attach(mover, sleeper));
    REQUIRE_FALSE(world.flush_structural());
    CHECK(hierarchy.is_dormant(mover));
    CHECK(world.is_active<Health>(mover) == false);

    // ...and back out: restored to its captured pattern.
    REQUIRE_FALSE(hierarchy.queue_detach(mover));
    REQUIRE_FALSE(world.flush_structural());
    CHECK_FALSE(hierarchy.is_dormant(mover));
    CHECK(world.is_active<Health>(mover) == true);
}

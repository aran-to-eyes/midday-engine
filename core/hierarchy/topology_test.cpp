// hierarchy.topology — ECS-resident tree links, queue-only reparenting
// (structural mutation mid-iteration impossible by construction), auto-adopt
// of queued spawns, cycle refusal as counted no-op, cascade despawn, owner
// boundaries (core/hierarchy/hierarchy.h).

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#include <optional>
#include <string>
#include <vector>

using midday::base::Error;
using midday::base::Name;
using midday::ecs::EntityRef;
using midday::ecs::FlushStats;
using midday::ecs::World;
using midday::hierarchy::Hierarchy;
using midday::reflect::ClassDesc;
using midday::reflect::Registry;

namespace {

struct Tag {
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

// Children of `parent` in sibling-list order.
std::vector<EntityRef> children_of(const Hierarchy& hierarchy, EntityRef parent) {
    std::vector<EntityRef> out;
    for (EntityRef child = hierarchy.first_child_of(parent); !child.is_null();
         child = hierarchy.next_sibling_of(child))
        out.push_back(child);
    return out;
}

} // namespace

TEST_CASE("hierarchy.topology: adoption builds an ordered root set") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    const EntityRef a = world.spawn();
    const EntityRef b = world.spawn();
    const EntityRef c = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(a));
    REQUIRE_FALSE(hierarchy.adopt(b));
    REQUIRE_FALSE(hierarchy.adopt(c));

    // Roots are siblings under the implicit forest root, in adoption order.
    CHECK(hierarchy.first_root() == a);
    CHECK(hierarchy.next_sibling_of(a) == b);
    CHECK(hierarchy.next_sibling_of(b) == c);
    CHECK(hierarchy.next_sibling_of(c).is_null());
    CHECK(hierarchy.adopted_count() == 3);
    CHECK(hierarchy.parent_of(a).is_null());

    // Double adoption and stranger refs are structured refusals.
    CHECK(code_of(hierarchy.adopt(a)) == "hierarchy.already_adopted");
    const EntityRef stranger = world.spawn();
    CHECK_FALSE(hierarchy.contains(stranger));
    CHECK(code_of(hierarchy.set_owner(stranger, true)) == "hierarchy.not_adopted");
}

TEST_CASE("hierarchy.topology: attach/detach apply only at the structural flush") {
    Registry registry;
    World world(registry);
    world.register_component<Tag>(component_class("Tag"));
    Hierarchy hierarchy(world);

    const EntityRef parent = world.spawn();
    const EntityRef child = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(parent));
    REQUIRE_FALSE(hierarchy.adopt(child));
    REQUIRE_FALSE(world.emplace(parent, Tag{1}));

    // Mid-iteration: direct adoption is refused (the ECS iteration lock);
    // queueing is the sanctioned path and mutates NOTHING until the flush.
    const EntityRef late = world.spawn();
    world.view<Tag>().each([&](EntityRef, Tag&) {
        CHECK(code_of(hierarchy.adopt(late)) == "ecs.structural_during_iteration");
        REQUIRE_FALSE(hierarchy.queue_attach(child, parent));
        CHECK(hierarchy.parent_of(child).is_null()); // still a root: not applied
    });
    CHECK(hierarchy.parent_of(child).is_null());

    FlushStats stats;
    REQUIRE_FALSE(world.flush_structural(&stats));
    CHECK(stats.applied == 1);
    CHECK(hierarchy.parent_of(child) == parent);
    CHECK(children_of(hierarchy, parent) == std::vector<EntityRef>{child});

    // Detach re-roots at the END of the root order.
    REQUIRE_FALSE(hierarchy.queue_detach(child));
    REQUIRE_FALSE(world.flush_structural());
    CHECK(hierarchy.parent_of(child).is_null());
    CHECK(hierarchy.next_sibling_of(parent) == child);
    CHECK(hierarchy.child_count(parent) == 0);
}

TEST_CASE("hierarchy.topology: queued spawn + attach in one tick auto-adopts") {
    Registry registry;
    World world(registry);
    world.register_component<Tag>(component_class("Tag"));
    Hierarchy hierarchy(world);

    const EntityRef parent = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(parent));
    REQUIRE_FALSE(world.emplace(parent, Tag{1}));

    // The canonical spawn-and-attach flow: wire the pending ref mid-iteration.
    EntityRef spawned;
    world.view<Tag>().each([&](EntityRef, Tag&) {
        spawned = world.queue_spawn();
        REQUIRE_FALSE(hierarchy.queue_attach(spawned, parent));
    });
    const auto before = hierarchy.reparent_stats();
    REQUIRE_FALSE(world.flush_structural());
    CHECK(hierarchy.contains(spawned));
    CHECK(hierarchy.parent_of(spawned) == parent);
    CHECK(hierarchy.reparent_stats().auto_adopted == before.auto_adopted + 1);
}

TEST_CASE("hierarchy.topology: cycles are refused as counted deterministic no-ops") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    const EntityRef a = world.spawn();
    const EntityRef b = world.spawn();
    const EntityRef c = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(a));
    REQUIRE_FALSE(hierarchy.queue_attach(b, a));
    REQUIRE_FALSE(hierarchy.queue_attach(c, b));
    REQUIRE_FALSE(world.flush_structural());

    // Self-parenting is refusable already at queue time.
    CHECK(code_of(hierarchy.queue_attach(a, a)) == "hierarchy.cycle");

    // A deeper cycle (grandparent under grandchild) is only detectable at
    // apply time: counted, applied as nothing, topology untouched.
    const auto before = hierarchy.reparent_stats();
    REQUIRE_FALSE(hierarchy.queue_attach(a, c));
    REQUIRE_FALSE(world.flush_structural());
    CHECK(hierarchy.reparent_stats().cycles_refused == before.cycles_refused + 1);
    CHECK(hierarchy.parent_of(a).is_null());
    CHECK(hierarchy.parent_of(b) == a);
    CHECK(hierarchy.parent_of(c) == b);
}

TEST_CASE("hierarchy.topology: reparent onto the current parent moves to the end") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    const EntityRef parent = world.spawn();
    const EntityRef x = world.spawn();
    const EntityRef y = world.spawn();
    const EntityRef z = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(parent));
    for (const EntityRef e : {x, y, z})
        REQUIRE_FALSE(hierarchy.queue_attach(e, parent));
    REQUIRE_FALSE(world.flush_structural());
    CHECK(children_of(hierarchy, parent) == std::vector<EntityRef>{x, y, z});

    REQUIRE_FALSE(hierarchy.queue_attach(x, parent)); // the reorder primitive
    REQUIRE_FALSE(world.flush_structural());
    CHECK(children_of(hierarchy, parent) == std::vector<EntityRef>{y, z, x});
}

TEST_CASE("hierarchy.topology: despawn cascades depth-first through the subtree") {
    Registry registry;
    World world(registry);
    world.register_component<Tag>(component_class("Tag"));
    Hierarchy hierarchy(world);

    // parent -> (a -> (leaf), b); bystander keeps its component untouched.
    const EntityRef parent = world.spawn();
    const EntityRef a = world.spawn();
    const EntityRef leaf = world.spawn();
    const EntityRef b = world.spawn();
    const EntityRef bystander = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(parent));
    REQUIRE_FALSE(hierarchy.adopt(bystander));
    REQUIRE_FALSE(world.emplace(bystander, Tag{7}));
    REQUIRE_FALSE(hierarchy.queue_attach(a, parent));
    REQUIRE_FALSE(hierarchy.queue_attach(leaf, a));
    REQUIRE_FALSE(hierarchy.queue_attach(b, parent));
    REQUIRE_FALSE(world.flush_structural());
    CHECK(hierarchy.adopted_count() == 5);

    REQUIRE_FALSE(world.despawn(parent));
    CHECK_FALSE(world.alive(parent));
    CHECK_FALSE(world.alive(a));
    CHECK_FALSE(world.alive(leaf));
    CHECK_FALSE(world.alive(b));
    CHECK(world.alive(bystander));
    CHECK(hierarchy.adopted_count() == 1);
    CHECK(hierarchy.first_root() == bystander);
    CHECK(hierarchy.next_sibling_of(bystander).is_null());
    CHECK(world.try_get<Tag>(bystander)->value == 7);

    // Queued commands against cascade-killed entities drop deterministically.
    // (queue first, then cascade via a queued despawn of the root)
    const EntityRef p2 = world.spawn();
    const EntityRef c2 = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(p2));
    REQUIRE_FALSE(hierarchy.queue_attach(c2, p2));
    REQUIRE_FALSE(world.flush_structural());
    REQUIRE_FALSE(world.queue_despawn(p2));
    REQUIRE_FALSE(hierarchy.queue_detach(c2)); // c2 dies with p2 before this applies
    FlushStats stats;
    REQUIRE_FALSE(world.flush_structural(&stats));
    CHECK(stats.applied == 1);
    CHECK(stats.dropped == 1);
    CHECK_FALSE(world.alive(c2));
}

TEST_CASE("hierarchy.topology: owner boundaries resolve nearest ancestor-or-self") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    const EntityRef machine = world.spawn();
    const EntityRef state = world.spawn();
    const EntityRef hurtbox = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(machine));
    REQUIRE_FALSE(hierarchy.queue_attach(state, machine));
    REQUIRE_FALSE(hierarchy.queue_attach(hurtbox, state));
    REQUIRE_FALSE(world.flush_structural());

    CHECK(hierarchy.owner_of(hurtbox).is_null()); // no boundary yet
    REQUIRE_FALSE(hierarchy.set_owner(machine, true));
    CHECK(hierarchy.is_owner(machine));
    CHECK(hierarchy.owner_of(hurtbox) == machine);
    CHECK(hierarchy.owner_of(state) == machine);
    CHECK(hierarchy.owner_of(machine) == machine); // ancestor-or-self
}

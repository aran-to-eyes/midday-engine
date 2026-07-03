// ecs.world — stale-handle structured errors, the reflect registration
// bridge, and the deferred structural queue: refusal of direct mutation
// mid-iteration, queue-order flushing, the reparent slot, and drop
// semantics (core/ecs/world.h).

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/ecs/world.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using midday::base::Error;
using midday::base::Json;
using midday::base::Name;
using midday::ecs::EntityRef;
using midday::ecs::FlushStats;
using midday::ecs::World;
using midday::reflect::ClassDesc;
using midday::reflect::Registry;

namespace {

struct Health {
    float value = 100;
};

struct Armor {
    float value = 0;
};

ClassDesc component_class(const char* name) {
    ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

// The error code carried by an optional<Error> result, "<none>" when clear.
std::string code_of(const std::optional<Error>& error) {
    return error.has_value() ? error->code : std::string("<none>");
}

} // namespace

TEST_CASE("ecs.world: stale handles raise structured errors, never UB") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));

    const EntityRef e = world.spawn();
    REQUIRE_FALSE(world.emplace(e, Health{50}));
    REQUIRE_FALSE(world.despawn(e));

    // Every handle-taking mutator refuses the dead ref with ecs.stale_handle.
    const std::optional<Error> again = world.despawn(e);
    REQUIRE(again.has_value());
    if (again.has_value()) { // re-proven for the analyzer's dataflow model
        CHECK(again->code == "ecs.stale_handle");
        CHECK(again->details.find("index") != nullptr);
        CHECK(again->details.find("generation") != nullptr);
        const Json* current = again->details.find("current_generation");
        REQUIRE(current != nullptr);
        CHECK(current->as_int() == e.generation + 1);
    }

    CHECK(code_of(world.emplace(e, Health{1})) == "ecs.stale_handle");
    CHECK(code_of(world.set_active<Health>(e, false)) == "ecs.stale_handle");
    CHECK(code_of(world.queue_despawn(e)) == "ecs.stale_handle");
    CHECK(world.try_get<Health>(e) == nullptr);
    CHECK_FALSE(world.has<Health>(e));
    CHECK_FALSE(world.is_active<Health>(e).has_value());

    // Slot reuse (LIFO) bumps the generation: the old ref STAYS stale.
    const EntityRef reborn = world.spawn();
    REQUIRE(reborn.index == e.index);
    CHECK(world.alive(reborn));
    CHECK(code_of(world.despawn(e)) == "ecs.stale_handle");
}

TEST_CASE("ecs.world: component misuse yields structured errors") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));

    const EntityRef e = world.spawn();
    REQUIRE_FALSE(world.emplace(e, Health{50}));

    const std::optional<Error> dup = world.emplace(e, Health{60});
    REQUIRE(dup.has_value());
    if (dup.has_value()) { // re-proven for the analyzer's dataflow model
        CHECK(dup->code == "ecs.duplicate_component");
        const Json* component = dup->details.find("component");
        REQUIRE(component != nullptr);
        CHECK(component->as_string() == "Health");
    }
    REQUIRE(world.try_get<Health>(e) != nullptr);
    CHECK(world.try_get<Health>(e)->value == 50); // first value untouched

    world.register_component<Armor>(component_class("Armor"));
    const std::optional<Error> missing = world.set_active<Armor>(e, true);
    REQUIRE(missing.has_value());
    if (missing.has_value()) { // re-proven for the analyzer's dataflow model
        CHECK(missing->code == "ecs.missing_component");
        const Json* component = missing->details.find("component");
        REQUIRE(component != nullptr);
        CHECK(component->as_string() == "Armor");
    }
}

TEST_CASE("ecs.world: the reflect bridge registers every component class") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));
    world.register_component<Armor>(component_class("Armor"));

    // The registry — the source of engine_api.json — knows both classes.
    const auto* health = registry.find_class(Name("Health"));
    REQUIRE(health != nullptr);
    CHECK(health->desc.compat_hash != 0);
    CHECK(registry.find_class(Name("Armor")) != nullptr);
    CHECK(registry.classes().size() == 2);
}

TEST_CASE("ecs.world: direct structural mutation mid-iteration is refused") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));

    const EntityRef a = world.spawn();
    const EntityRef b = world.spawn();
    REQUIRE_FALSE(world.emplace(a, Health{1}));
    REQUIRE_FALSE(world.emplace(b, Health{2}));

    world.view<Health>().each([&](EntityRef, Health&) {
        Error spawn_error;
        CHECK(world.spawn(&spawn_error).is_null());
        CHECK(spawn_error.code == "ecs.structural_during_iteration");
        const Json* operation = spawn_error.details.find("operation");
        REQUIRE(operation != nullptr);
        CHECK(operation->as_string() == "spawn");

        CHECK(code_of(world.despawn(a)) == "ecs.structural_during_iteration");
        CHECK(code_of(world.emplace(b, Health{3})) == "ecs.structural_during_iteration");
        CHECK(code_of(world.flush_structural()) == "ecs.structural_during_iteration");

        // The sanctioned path queues instead.
        CHECK_FALSE(world.queue_despawn(a));
    });

    CHECK(world.alive(a)); // nothing applied yet
    REQUIRE_FALSE(world.flush_structural());
    CHECK_FALSE(world.alive(a));
    CHECK(world.alive(b));
}

TEST_CASE("ecs.world: queued commands apply at the flush, in queue order") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));

    // queue_spawn reserves the handle NOW; the entity is pending until flush.
    const EntityRef pending = world.queue_spawn();
    CHECK_FALSE(pending.is_null());
    CHECK_FALSE(world.alive(pending));
    CHECK(code_of(world.emplace(pending, Health{5})) == "ecs.entity_pending");

    const EntityRef victim = world.spawn();
    REQUIRE_FALSE(world.queue_despawn(victim));
    // Spawn-then-despawn of the SAME pending ref resolves in queue order.
    const EntityRef ephemeral = world.queue_spawn();
    REQUIRE_FALSE(world.queue_despawn(ephemeral));
    CHECK(world.pending_command_count() == 4);

    FlushStats stats;
    REQUIRE_FALSE(world.flush_structural(&stats));
    CHECK(stats.applied == 4);
    CHECK(stats.dropped == 0);
    CHECK(world.alive(pending));
    CHECK_FALSE(world.alive(victim));
    CHECK_FALSE(world.alive(ephemeral));
    CHECK(world.pending_command_count() == 0);
    REQUIRE_FALSE(world.emplace(pending, Health{5})); // live now: components attach
}

TEST_CASE("ecs.world: commands whose entity died in the queue window are dropped") {
    Registry registry;
    World world(registry);
    world.register_component<Health>(component_class("Health"));

    const EntityRef e = world.spawn();
    REQUIRE_FALSE(world.queue_despawn(e));
    REQUIRE_FALSE(world.queue_despawn(e)); // two systems, same conclusion

    FlushStats stats;
    REQUIRE_FALSE(world.flush_structural(&stats));
    CHECK(stats.applied == 1);
    CHECK(stats.dropped == 1); // deterministic no-op, not an error
}

TEST_CASE("ecs.world: the reparent slot orders commands for the hierarchy") {
    Registry registry;
    World world(registry);

    const EntityRef parent = world.spawn();
    const EntityRef child_a = world.spawn();
    const EntityRef child_b = world.spawn();
    REQUIRE_FALSE(world.queue_reparent(child_a, parent));
    REQUIRE_FALSE(world.queue_reparent(child_b, parent));
    REQUIRE_FALSE(world.queue_reparent(child_a, EntityRef{})); // back to root

    // No handler installed: structured refusal, NOTHING applied, queue intact.
    CHECK(code_of(world.flush_structural()) == "ecs.reparent_unhandled");
    CHECK(world.pending_command_count() == 3);

    // m0-scene-hierarchy's role: install the handler, then the flush drains
    // in queue order.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> applied;
    world.set_reparent_handler([&](EntityRef child, EntityRef new_parent) {
        applied.emplace_back(child.index, new_parent.index);
    });
    FlushStats stats;
    REQUIRE_FALSE(world.flush_structural(&stats));
    CHECK(stats.applied == 3);
    REQUIRE(applied.size() == 3);
    CHECK(applied[0] == std::pair{child_a.index, parent.index});
    CHECK(applied[1] == std::pair{child_b.index, parent.index});
    CHECK(applied[2] == std::pair{child_a.index, midday::ecs::kNullEntityIndex});
}

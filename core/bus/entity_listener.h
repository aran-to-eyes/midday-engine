// core/bus/entity_listener.h — the ECS bridge: subscribe a COMPONENT of an
// entity to a channel, safely.
//
// Why not store a listener pointer into the pool? Pool rows MOVE (swap-and-
// pop relocates surviving rows on every despawn, core/ecs/sparse_set.h), so
// any cached component address dangles silently while its entity still
// lives. The subscription therefore stores (EntityRef, thunk) and re-fetches
// the row through the World at every delivery — two paged sparse-set finds,
// no hashing, no cached pointers, correct by construction (D-BUILD-048).
//
// Lifecycle semantics at dispatch:
//   * entity ALIVE, component present and ACTIVE  -> delivered
//   * component missing or INACTIVE (state-toggled dormant, spec 4.1 —
//     dormant parts hear nothing; the no-zombie-hitbox rule) -> skipped,
//     subscription kept
//   * entity PENDING (queue_spawn window) -> skipped, subscription kept —
//     it starts hearing events once the structural flush makes it alive
//   * entity STALE (despawned; generation mismatch) -> AUTO-UNSUBSCRIBED:
//     the bus drops the subscription without ever touching component
//     memory. Lazy generation checks, not the World despawn-observer
//     socket: that socket has ONE slot and core/hierarchy owns it
//     (cascade despawn, D-BUILD-029).
//
// T is duck-typed: any component with
//     void on_event(bus::Bus&, const bus::EventView&)
// qualifies — no vtable in pool rows, no inheritance requirement.

#pragma once

#include "core/base/error.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/ecs/world.h"

#include <optional>

namespace midday::bus {

// The generated delivery thunk: one instantiation per component type; its
// ADDRESS is the (entity, key) subscription's type identity.
template <typename T>
bool component_thunk(ecs::World& world, ecs::EntityRef entity, Bus& bus, const EventView& event) {
    // Active-by-default semantics (spec 4.1): a dormant component row stays
    // subscribed but hears nothing.
    if (!world.is_active<T>(entity).value_or(false))
        return false;
    world.try_get<T>(entity)->on_event(bus, event); // non-null: active implies present
    return true;
}

template <typename T>
std::optional<base::Error> subscribe_component(Bus& bus, EventKey key, ecs::EntityRef entity) {
    return bus.subscribe_entity(key, entity, &component_thunk<T>);
}

template <typename T>
std::optional<base::Error> unsubscribe_component(Bus& bus, EventKey key, ecs::EntityRef entity) {
    return bus.unsubscribe_entity(key, entity, &component_thunk<T>);
}

} // namespace midday::bus

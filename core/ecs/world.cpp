// core/ecs/world.cpp — non-template World machinery: structured errors,
// entity lifecycle, and the deterministic structural flush.

#include "core/ecs/world.h"

#include "core/base/error.h"
#include "core/base/json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace midday::ecs {

namespace detail {

std::uint32_t next_component_type_id() {
    static std::uint32_t counter = 0;
    return counter++;
}

base::Error structural_locked_error(std::string_view operation) {
    base::Error error;
    error.code = "ecs.structural_during_iteration";
    error.message = "structural mutation during iteration; use the deferred queue (queue_*)";
    error.details.set("operation", base::Json(operation));
    return error;
}

base::Error missing_component_error(base::Name component, EntityRef ref) {
    base::Error error;
    error.code = "ecs.missing_component";
    error.message = "entity has no such component";
    error.details.set("component", base::Json(component.view()));
    error.details.set("index", base::Json(ref.index));
    return error;
}

base::Error duplicate_component_error(base::Name component, EntityRef ref) {
    base::Error error;
    error.code = "ecs.duplicate_component";
    error.message = "entity already has this component";
    error.details.set("component", base::Json(component.view()));
    error.details.set("index", base::Json(ref.index));
    return error;
}

void fail_component_reregistered(base::Name name) {
    std::fprintf(stderr,
                 "midday: fatal: ecs: component type registered twice (name '%s')\n",
                 std::string(name.view()).c_str());
    std::abort();
}

void fail_unregistered_pool() {
    std::fprintf(stderr,
                 "midday: fatal: ecs: pool access for an unregistered component type "
                 "(call World::register_component at boot first)\n");
    std::abort();
}

namespace {

base::Error stale_handle_error(EntityRef ref, const EntityTable& table) {
    base::Error error;
    error.code = "ecs.stale_handle";
    error.message = "entity handle does not name a live entity";
    error.details.set("index", base::Json(ref.index));
    error.details.set("generation", base::Json(ref.generation));
    if (ref.index < table.slot_count())
        error.details.set("current_generation", base::Json(table.generation_of(ref.index)));
    return error;
}

base::Error entity_pending_error(EntityRef ref) {
    base::Error error;
    error.code = "ecs.entity_pending";
    error.message = "entity is queue-reserved and not alive until the structural flush";
    error.details.set("index", base::Json(ref.index));
    error.details.set("generation", base::Json(ref.generation));
    return error;
}

base::Error reparent_unhandled_error() {
    base::Error error;
    error.code = "ecs.reparent_unhandled";
    error.message = "reparent commands queued but no reparent handler is installed; "
                    "nothing was applied and the queue is intact";
    return error;
}

} // namespace

} // namespace detail

std::optional<base::Error> World::check_alive(EntityRef ref) const {
    if (table_.is_alive(ref))
        return std::nullopt;
    if (table_.is_pending(ref))
        return detail::entity_pending_error(ref);
    return detail::stale_handle_error(ref, table_);
}

EntityRef World::spawn(base::Error* error) {
    if (iteration_depth_ != 0) {
        if (error != nullptr)
            *error = detail::structural_locked_error("spawn");
        return EntityRef{};
    }
    return table_.allocate(SlotState::kAlive);
}

std::optional<base::Error> World::despawn(EntityRef ref) {
    if (iteration_depth_ != 0)
        return detail::structural_locked_error("despawn");
    if (auto error = check_alive(ref))
        return error;
    despawn_now(ref);
    return std::nullopt;
}

void World::despawn_now(EntityRef ref) {
    for (PoolBase* pool : pool_order_)
        pool->erase(ref.index);
    table_.release(ref);
}

EntityRef World::queue_spawn() {
    const EntityRef ref = table_.allocate(SlotState::kPending);
    commands_.push_back(StructuralCommand{StructuralOp::kSpawn, ref, EntityRef{}});
    return ref;
}

std::optional<base::Error> World::queue_despawn(EntityRef ref) {
    // Alive AND pending refs queue fine (spawn-then-despawn within one tick
    // resolves in queue order at the flush); only stale refs are refused.
    if (!table_.is_current(ref))
        return detail::stale_handle_error(ref, table_);
    commands_.push_back(StructuralCommand{StructuralOp::kDespawn, ref, EntityRef{}});
    return std::nullopt;
}

std::optional<base::Error> World::queue_reparent(EntityRef child, EntityRef new_parent) {
    if (!table_.is_current(child))
        return detail::stale_handle_error(child, table_);
    if (!new_parent.is_null() && !table_.is_current(new_parent))
        return detail::stale_handle_error(new_parent, table_);
    commands_.push_back(StructuralCommand{StructuralOp::kReparent, child, new_parent});
    return std::nullopt;
}

std::optional<base::Error> World::flush_structural(FlushStats* stats) {
    if (iteration_depth_ != 0)
        return detail::structural_locked_error("flush_structural");

    // Configuration check BEFORE any application: a flush never half-applies.
    if (!reparent_handler_) {
        for (const StructuralCommand& command : commands_) {
            if (command.op == StructuralOp::kReparent)
                return detail::reparent_unhandled_error();
        }
    }

    FlushStats local;
    for (const StructuralCommand& command : commands_) {
        switch (command.op) {
        case StructuralOp::kSpawn:
            // Queue order guarantees the spawn precedes any despawn of the
            // same ref, so a pending entity is still pending here; the guard
            // keeps a future invalidation path from corrupting state.
            if (table_.is_pending(command.entity)) {
                table_.make_alive(command.entity);
                ++local.applied;
            } else {
                ++local.dropped;
            }
            break;
        case StructuralOp::kDespawn:
            if (table_.is_alive(command.entity)) {
                despawn_now(command.entity);
                ++local.applied;
            } else {
                ++local.dropped; // died between queue and flush: deterministic no-op
            }
            break;
        case StructuralOp::kReparent:
            if (table_.is_alive(command.entity) &&
                (command.parent.is_null() || table_.is_alive(command.parent))) {
                reparent_handler_(command.entity, command.parent);
                ++local.applied;
            } else {
                ++local.dropped;
            }
            break;
        }
    }
    commands_.clear();
    if (stats != nullptr)
        *stats = local;
    return std::nullopt;
}

} // namespace midday::ecs

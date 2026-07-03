// core/ecs/structural_queue.h — the deferred structural command vocabulary.
//
// Structure never mutates mid-iteration (the flecs deferred-ops rule,
// independently converged on by our phase-8 structural apply — see
// ENGINE_ARCHITECTURE_RESEARCH.md section 3). During iteration, systems
// QUEUE spawn/despawn/reparent through World::queue_*; the queue is applied
// at ONE deterministic flush point (World::flush_structural — the tick
// loop's structural-apply phase from m0-tick-loop on), strictly in queue
// order.
//
// Semantics fixed here:
//   * queue_spawn reserves the EntityRef IMMEDIATELY (slot state kPending):
//     references can be wired mid-iteration, but the entity is invisible to
//     queries and component APIs until the flush makes it alive.
//   * Commands are validated when queued (stale handles refused with
//     structured Errors) and RE-validated at flush: a command whose entity
//     died after queueing (e.g. two systems queued the same despawn) is
//     DROPPED, counted in FlushStats.dropped — deterministic no-op, not an
//     error, matching the deferred-ops model.
//   * kReparent is the SLOT for m0-scene-hierarchy: the queue carries and
//     orders the command; tree topology itself lives in core/hierarchy,
//     which installs the apply handler. Flushing reparent commands with no
//     handler installed is a structured error and applies NOTHING (the
//     queue survives, so installing the handler and re-flushing recovers).

#pragma once

#include "core/ecs/entity.h"

#include <cstdint>
#include <functional>

namespace midday::ecs {

enum class StructuralOp : std::uint8_t {
    kSpawn,    // make a pending (queue-reserved) entity alive
    kDespawn,  // release the entity and drop its rows from every pool
    kReparent, // delegate (child, new_parent) to the installed handler
};

struct StructuralCommand {
    StructuralOp op = StructuralOp::kSpawn;
    EntityRef entity;
    EntityRef parent; // kReparent only; null means "reparent to root"
};

struct FlushStats {
    std::uint32_t applied = 0;
    std::uint32_t dropped = 0; // commands whose entity died between queue and flush
};

// Installed by m0-scene-hierarchy; invoked at flush, in queue order, only
// for commands whose entity (and non-null parent) is alive.
using ReparentHandler = std::function<void(EntityRef child, EntityRef new_parent)>;

// Installed by m0-scene-hierarchy next to the reparent handler; invoked at
// the start of every despawn (before rows drop) so tree topology can unlink
// and cascade. See World::set_despawn_observer.
using DespawnObserver = std::function<void(EntityRef entity)>;

} // namespace midday::ecs

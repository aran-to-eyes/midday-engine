// core/loader/prefab_instantiate.h — the ONE prefab-materialization engine
// (m1-prefab-spawn): resolves a prefab's per-machine overrides and
// instantiates its machine subtrees (the enter chain, spec A.1 phase 8) onto
// an already-alive, already-placed entity. Shared by BOTH sanctioned prefab
// spawn paths so there is exactly one override-application + one
// instantiate-and-journal implementation:
//   * core/loader/spawn.cpp's spawn_scene — a `prefab:` SCENE entity, spawned
//     DIRECTLY (world.spawn(), boot path, unchanged since m0-yaml-loader-run).
//   * core/loader/prefab_spawn.h's PrefabSpawner — a script's runtime
//     `world.spawn(prefab, ...)`, spawned through the deferred queue and
//     realized at the tick's own structural-apply phase.
//
// Division of labor (deliberately NOT this file's job): reserving/spawning
// the root entity, placing it in the hierarchy (adopt/queue_attach) and
// setting its local transform, and deciding WHEN to flush the structural
// queue — every caller already owns exactly one deterministic flush point
// (spawn_scene's end-of-scene flush; PrefabSpawner::realize's end-of-tick
// flush) and this file's state-children ride THAT flush (queued via
// hierarchy::queue_attach, returned for the caller to place/flush) rather
// than inventing a second one.

#pragma once

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/ecs/entity.h"
#include "core/loader/loader.h"
#include "core/loader/override.h"
#include "core/math/xform.h"
#include "core/statechart/statechart.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// ecs::World, hierarchy::Hierarchy, and journal::Writer are already
// forward-declared by core/loader/loader.h (included above) — no need to
// repeat that block here.
namespace midday::loader {

// Applies `overrides` onto a COPY of every machine `entity_file` instances,
// split by machine NAME first (spec 4.2's `<machine>/<Region>/...` grammar —
// `override.h`'s split_overrides_for_machine) and combined with that
// instance's OWN entity-file-level overrides, exactly the precedent
// cli/verbs/scene.cpp's report_entity_machines reads (never a second
// override-application engine). `origin_file` is the diagnostic origin for a
// bad path (report_entity_machines's own `entity_file.path` convention).
struct ResolvedMachinesResult {
    std::vector<MachineFile> machines; // overrides applied; entity_file.machines order
    std::optional<base::Error> error;
};

ResolvedMachinesResult resolve_prefab_machines(const EntityFile& entity_file,
                                               const std::vector<OverrideEntry>& overrides,
                                               std::string_view origin_file);

// One state child a materialized machine spawned (the loader::spawn_scene
// StateChildDesc-under-a-state precedent): direct-spawned and queue_attach'd
// already — NOT flushed here, see header contract above.
struct PrefabChild {
    ecs::EntityRef ref;
    math::Transform at;
};

// One machine a materialize_prefab call instantiated: id + name + its state
// scripts, everything a caller needs to build a MachineSeat/ScriptSeat (the
// loader::SpawnResult shape spawn_scene already reports).
struct MaterializedMachine {
    statechart::MachineId id = statechart::kInvalidMachine;
    base::Name machine;
    std::vector<StateScriptRef> scripts;
};

struct MaterializeResult {
    std::vector<MaterializedMachine> machines;
    std::vector<PrefabChild> children;
    std::optional<base::Error> error; // engaged -> machines/children meaningless
};

// Instantiates every one of `machines` (already override-resolved) onto
// `root` — an entity the caller has ALREADY made alive, hierarchy-placed,
// and positioned. Journals ONE "prefab.spawn" record for `root` (citing
// `cause_id`) plus one per spawned state child (the scene.spawn precedent),
// all at `tick` (0 for the boot path, the current sim tick for the runtime
// path). A machine instantiate failure stops at the first one (nothing
// rolls back further than Statechart::instantiate's own atomic contract
// already guarantees) — the caller's existing "first failure wins" refusal
// discipline (spawn_scene) or tick-halting discipline (PrefabSpawner) takes
// it from there.
MaterializeResult materialize_prefab(ecs::World& world,
                                     hierarchy::Hierarchy& hierarchy,
                                     statechart::Statechart& chart,
                                     journal::Writer& journal,
                                     std::uint64_t tick,
                                     const std::vector<MachineFile>& machines,
                                     ecs::EntityRef root,
                                     std::string_view prefab_path,
                                     std::uint64_t cause_id);

} // namespace midday::loader

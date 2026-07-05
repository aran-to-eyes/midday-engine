// core/loader/prefab_spawn.h — world.spawn(prefab, {at, overrides}) /
// world.despawn(ref): the RUNTIME (mid-tick, script-facing) prefab spawn
// path (m1-prefab-spawn, spec section 7 "Lifetime"). The loader's OWN
// load-time path (spawn_scene, core/loader/spawn.cpp) stays direct
// (world.spawn()); THIS class is what a script's queued spawn rides —
// queue_spawn reserves the EntityRef immediately (kPending, exit-test #3),
// and PrefabSpawner::realize (installed as the tick's ONE structural-apply
// extension slot, core/tick/tick_loop.h) actually builds the prefab subtree
// once World::flush_structural has made the reservation real.
//
// PREFAB-ONLY enforcement: this is the only public entry point a script (or
// any non-core caller) can reach to bring an entity into existence — it
// takes a prefab-ref (a resolved *.entity.yaml path) and NEVER a
// caller-assembled component list; there is no other public method here
// that mutates the World. Combined with scripts/check_entity_api.py (which
// forbids World::spawn/queue_spawn/emplace outside core/ + tests), a
// caller's only route to a live entity is through here or the loader —
// never a bare-entity assembly API (exit-test #4).
//
// Validation happens TWICE, on purpose: spawn() resolves the prefab file and
// applies every override NOW (structured refusal at the call site, mirroring
// core/ecs/structural_queue.h's "commands are validated when queued"
// contract — a script never gets back a handle for a request destined to
// fail deep in the tick); realize() only ever does WORK that was already
// validated to succeed, given the entity survives to the flush.
//
// Despawn semantics: despawn(ref) queues through the SAME
// ecs::World::queue_despawn every other despawn rides (children/subtree
// cascade is core/hierarchy's existing despawn-observer, untouched by this
// node) — the ref stays alive until the flush, exactly like a direct
// queue_despawn call.

#pragma once

#include "core/base/error.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/journal/writer.h"
#include "core/loader/loader.h"
#include "core/loader/override.h"
#include "core/loader/prefab_instantiate.h"
#include "core/math/xform.h"
#include "core/reflect/registry.h"
#include "core/statechart/statechart.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ecs::World and hierarchy::Hierarchy are already forward-declared by
// core/loader/loader.h (included above) — no need to repeat that block here.
namespace midday::loader {

struct PrefabSpawnResult {
    ecs::EntityRef ref; // kPending until the next structural-apply phase
    std::optional<base::Error> error;
};

class PrefabSpawner {
public:
    // All collaborators must outlive the PrefabSpawner. `events`/
    // `components_vocab` are the SAME project vocabulary the scene's own
    // prefabs were resolved against (loader::EventsDecl, loader::
    // ComponentVocab) — a prefab a script names is held to the identical
    // strictness contract as one authored directly in a scene.
    PrefabSpawner(ecs::World& world,
                  hierarchy::Hierarchy& hierarchy,
                  statechart::Statechart& chart,
                  bus::Bus& bus,
                  journal::Writer& journal,
                  reflect::Registry& registry,
                  const EventsDecl& events,
                  ComponentVocab components_vocab = ComponentVocab{});

    PrefabSpawner(const PrefabSpawner&) = delete;
    PrefabSpawner& operator=(const PrefabSpawner&) = delete;
    PrefabSpawner(PrefabSpawner&&) = delete;
    PrefabSpawner& operator=(PrefabSpawner&&) = delete;
    ~PrefabSpawner() = default;

    // The script-facing spawn: loads (once; cached by path) and resolves
    // `prefab_path`'s machines against `overrides` NOW, reserves the handle
    // via ecs::World::queue_spawn, and defers the actual build to the next
    // realize() call. `at` becomes the entity's local translation (the
    // scene-format `prefab: ... at:` precedent, core/loader/scene_load.cpp —
    // a fresh identity transform, never composed with the prefab's own
    // `base:` Transform entry). Named spawn_prefab, never bare `spawn`
    // (scripts/check_entity_api.py treats ANY `.spawn(`/`->spawn(` call site
    // as suspect, any receiver — a second, distinct verb keeps every caller
    // of this method visibly PREFAB-scoped, never mistakable for the
    // bare-entity ecs::World::spawn the scanner actually polices).
    PrefabSpawnResult spawn_prefab(const std::string& prefab_path,
                                   const math::Vec3& at,
                                   const std::vector<OverrideEntry>& overrides = {});

    // The script-facing despawn: queues through ecs::World::queue_despawn
    // (never a second mutation path). The ref stays alive until the flush.
    std::optional<base::Error> despawn(ecs::EntityRef ref);

    [[nodiscard]] std::size_t pending_spawn_count() const { return pending_.size(); }

    [[nodiscard]] std::size_t pending_despawn_count() const { return pending_despawns_.size(); }

    // Install as TickLoop::set_structural_realizer. Called once per tick,
    // after World::flush_structural, before Hierarchy::propagate: fires
    // entity.despawned for every despawn this spawner queued and is now
    // provably dead, then adopts + materializes (core/loader/
    // prefab_instantiate.h) every pending spawn that survived to the flush,
    // firing entity.spawned once its machines' enter chains have run.
    std::optional<base::Error> realize(std::uint64_t phase_record_id);

private:
    struct PendingSpawn {
        ecs::EntityRef ref;
        std::string prefab_path;
        math::Vec3 at;
        std::vector<MachineFile> machines; // overrides already applied (spawn()-time)
    };

    // Loads (once) and caches `prefab_path`; nullptr + *error on failure.
    const EntityFile* catalog(const std::string& prefab_path, std::optional<base::Error>& error);

    ecs::World* world_;
    hierarchy::Hierarchy* hierarchy_;
    statechart::Statechart* chart_;
    bus::Bus* bus_;
    journal::Writer* journal_;
    reflect::Registry* registry_;
    const EventsDecl* events_;
    ComponentVocab components_vocab_;
    std::unordered_map<std::string, EntityFile> catalog_;
    std::vector<PendingSpawn> pending_;
    std::vector<ecs::EntityRef> pending_despawns_;
};

} // namespace midday::loader

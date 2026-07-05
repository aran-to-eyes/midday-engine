// core/loader/prefab_spawn.cpp — see prefab_spawn.h for the header contract
// this implements.

#include "core/loader/prefab_spawn.h"

#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"

#include <utility>

namespace midday::loader {

namespace {

base::Json ref_bits(ecs::EntityRef ref) {
    return {static_cast<std::int64_t>(ref.to_bits())};
}

base::Error prefab_not_found_error(const std::string& path, const base::Error& cause) {
    base::Error error;
    error.code = "prefab.not_found";
    error.message = "prefab '" + path + "': [" + cause.code + "] " + cause.message;
    error.details.set("path", path);
    error.details.set("cause", cause.to_json());
    return error;
}

} // namespace

PrefabSpawner::PrefabSpawner(ecs::World& world,
                             hierarchy::Hierarchy& hierarchy,
                             statechart::Statechart& chart,
                             bus::Bus& bus,
                             journal::Writer& journal,
                             reflect::Registry& registry,
                             const EventsDecl& events,
                             ComponentVocab components_vocab)
    : world_(&world), hierarchy_(&hierarchy), chart_(&chart), bus_(&bus), journal_(&journal),
      registry_(&registry), events_(&events), components_vocab_(std::move(components_vocab)) {}

const EntityFile* PrefabSpawner::catalog(const std::string& prefab_path,
                                         std::optional<base::Error>& error) {
    const auto found = catalog_.find(prefab_path);
    if (found != catalog_.end())
        return &found->second;
    EntityLoadResult loaded =
        load_entity_file(prefab_path, *registry_, *events_, components_vocab_, /*lenient=*/false);
    if (loaded.error.has_value()) {
        error = prefab_not_found_error(prefab_path, *loaded.error);
        return nullptr;
    }
    if (!loaded.entity.has_value()) { // defensive: loaders return one or the other
        error = base::Error{.code = "loader.bad_ref", .message = "entity load failed"};
        return nullptr;
    }
    const auto [it, inserted] = catalog_.emplace(prefab_path, std::move(*loaded.entity));
    (void)inserted;
    return &it->second;
}

PrefabSpawnResult PrefabSpawner::spawn_prefab(const std::string& prefab_path,
                                              const math::Vec3& at,
                                              const std::vector<OverrideEntry>& overrides) {
    PrefabSpawnResult out;
    const EntityFile* entity_file = catalog(prefab_path, out.error);
    if (out.error.has_value())
        return out;

    ResolvedMachinesResult resolved = resolve_prefab_machines(*entity_file, overrides, prefab_path);
    if (resolved.error.has_value()) {
        out.error = std::move(resolved.error);
        return out;
    }

    const ecs::EntityRef ref = world_->queue_spawn();
    pending_.push_back(PendingSpawn{ref, prefab_path, at, std::move(resolved.machines)});
    out.ref = ref;
    return out;
}

std::optional<base::Error> PrefabSpawner::despawn(ecs::EntityRef ref) {
    if (auto error = world_->queue_despawn(ref))
        return error;
    pending_despawns_.push_back(ref);
    return std::nullopt;
}

std::optional<base::Error> PrefabSpawner::realize(std::uint64_t phase_record_id) {
    const std::uint64_t tick = bus_->tick();

    // Despawn lifecycle events (spec: "Key: the despawned entity") — fired
    // for entities THIS spawner queued a despawn for, now that the flush has
    // made the removal real. Subtree exit-chain sequencing on despawn is a
    // pre-existing statechart/hierarchy gap this node does not newly own
    // (see prefab_spawn.h header note); the event itself is real.
    for (const ecs::EntityRef ref : pending_despawns_) {
        if (world_->alive(ref))
            continue; // defensive: queue_despawn only ever accepts current refs
        base::Json payload = base::Json::object();
        payload.set("entity", ref_bits(ref));
        (void)bus_->trigger(
            bus::EventKey::entity(ref), base::Name("entity.despawned"), payload, phase_record_id);
    }
    pending_despawns_.clear();

    struct PendingChild {
        ecs::EntityRef ref;
        math::Transform at;
    };

    std::vector<PendingChild> pending_children;

    for (PendingSpawn& spawn_request : pending_) {
        if (!world_->alive(spawn_request.ref))
            continue; // dropped: despawned this same tick, before this flush

        if (auto error = hierarchy_->adopt(spawn_request.ref))
            return error;
        math::Transform local;
        local.translation = spawn_request.at;
        if (auto error = hierarchy_->set_local(spawn_request.ref, local))
            return error;

        MaterializeResult materialized = materialize_prefab(*world_,
                                                            *hierarchy_,
                                                            *chart_,
                                                            *journal_,
                                                            tick,
                                                            spawn_request.machines,
                                                            spawn_request.ref,
                                                            spawn_request.prefab_path,
                                                            phase_record_id);
        if (materialized.error.has_value())
            return materialized.error;
        for (const PrefabChild& child : materialized.children)
            pending_children.push_back(PendingChild{child.ref, child.at});

        // "entity.spawned ... after its initial states entered" (builtin_events.cpp):
        // fired once every one of this root's machines has run its enter
        // chain. A top-level world.spawn() is always a root (no `parent`
        // param on the public surface) — parent reads null.
        base::Json spawned_payload = base::Json::object();
        spawned_payload.set("entity", ref_bits(spawn_request.ref));
        spawned_payload.set("parent", ref_bits(ecs::EntityRef{}));
        (void)bus_->trigger(bus::EventKey::entity(spawn_request.ref),
                            base::Name("entity.spawned"),
                            spawned_payload,
                            phase_record_id);
    }
    pending_.clear();

    if (!pending_children.empty()) {
        if (auto error = world_->flush_structural())
            return error;
        for (const PendingChild& child : pending_children)
            (void)hierarchy_->set_local(child.ref, child.at);
    }
    return std::nullopt;
}

} // namespace midday::loader

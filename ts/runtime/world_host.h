// ts/runtime/world_host.h — the script-facing world.spawn/despawn seat
// (m1-prefab-spawn): the ONE native primitive TS's `world.spawn(prefab,
// {at, overrides})` / `world.despawn(ref, opts?)` (ts/lib/component.ts) can
// call. The despawn seat takes 2 or 3 args (M2 0B track D): the optional
// third is `opts.after` in seconds — the despawn-linger deadline the
// spawner's linger queue ceilings into ticks (prefab_spawn.h D4 contract).
//
// This file is OUTSIDE core/ — scripts/check_entity_api.py would rightly
// refuse a direct ecs::World::queue_spawn/queue_despawn call here. It never
// makes one: every mutation delegates to loader::PrefabSpawner (core/loader/
// prefab_spawn.h), the sanctioned, core-owned seam that actually queues the
// structural command — PREFAB-ONLY spawning's real enforcement point (a
// script can only name a prefab FILE, never assemble components).
//
// Mirrors ts/runtime/component_host.h's shape (the entity/emit seat) rather
// than extending it: component_host.h's own header note explicitly defers
// the real structural-apply despawn wiring to "m1-prefab-spawn territory,
// out of this node's scope" — a separate class keeps that boundary honest;
// wiring the two seats together into a live `midday run` composition is a
// follow-up integration task (component_host.h's own primitives are not
// wired into cli/verbs/run.cpp yet either).

#pragma once

#include "core/base/json.h"
#include "core/loader/prefab_spawn.h"
#include "ts/runtime/script_runtime.h"

#include <string_view>

namespace midday::script {

class WorldHost {
public:
    // The seam contract (mirrors component_host.h's kStatusFn-style pins):
    // ts/lib/component.ts calls these by exactly these names.
    static constexpr std::string_view kSpawnFn = "__midday_world_spawn";
    static constexpr std::string_view kDespawnFn = "__midday_world_despawn";

    // `spawner` must outlive this seat.
    WorldHost(ScriptRuntime& runtime, loader::PrefabSpawner& spawner);

    WorldHost(const WorldHost&) = delete;
    WorldHost& operator=(const WorldHost&) = delete;
    WorldHost(WorldHost&&) = delete;
    WorldHost& operator=(WorldHost&&) = delete;
    ~WorldHost() = default;

private:
    [[nodiscard]] HostResult spawn(const base::Json::Array& args);
    [[nodiscard]] HostResult despawn(const base::Json::Array& args);

    loader::PrefabSpawner* spawner_;
};

} // namespace midday::script

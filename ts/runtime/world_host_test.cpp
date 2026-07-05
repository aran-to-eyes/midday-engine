// script.world_host doctests (m1-prefab-spawn): the TS-facing world.spawn(
// prefab, {at, overrides}) / world.despawn(ref) boundary — a real script,
// through the real toolchain, calling the real native primitives
// (ts/runtime/world_host.h), which delegate to the SAME loader::
// PrefabSpawner core/loader/prefab_spawn_test.cpp exercises directly. This
// file's job is narrower: prove the marshaling (path/at/overrides args,
// the returned EntityRef shape) is correct end to end — the engine-level
// exit tests (100-prefab mid-tick, despawn-mid-query, alive-after-phase-8)
// live in core/loader/prefab_spawn_test.cpp.

#include "core/base/file_io.h"
#include "core/loader/prefab_spawn.h"
#include "core/loader/prefab_test_support.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"
#include "ts/runtime/component_host.h"
#include "ts/runtime/script_runtime.h"
#include "ts/runtime/world_host.h"
#include "ts/toolchain/toolchain.h"

#include <filesystem>
#include <string>

using namespace midday;
using midday::ecs::EntityRef;
using midday::loader::EventsDecl;
using midday::loader::PrefabSpawner;
using midday::loader::test::write_goblin_prefab;
using midday::script::ComponentHost;
using midday::script::ScriptRuntime;
using midday::script::Toolchain;
using midday::script::ToolchainConfig;
using midday::script::WorldHost;
using midday::statechart::test::ChartFixture;

namespace {

ToolchainConfig fresh_toolchain(const std::string& name) {
    ToolchainConfig config;
    config.cache_dir = ".midday-cache/selftest/world_host_" + name;
    std::filesystem::remove_all(config.cache_dir);
    return config;
}

// ChartFixture always spawns + adopts exactly one entity (`host`, index 0)
// before test code runs anything else (core/statechart/test_support.h) —
// so the FIRST entity a PrefabSpawner ever queues in a fresh fixture is
// deterministically index 1, generation 0 (LIFO slot allocation over an
// empty free list, core/ecs/entity.h). Scripts below assert this from the
// INSIDE (throw on mismatch) rather than trust it silently.
constexpr std::uint32_t kFirstSpawnedIndex = 1;

// The full composition both tests below need: a ChartFixture wired to a
// PrefabSpawner as the tick's structural realizer, a goblin prefab on disk,
// and the two script-facing host seats (WorldHost is this node's own;
// ComponentHost is the sibling seat `ref.alive` needs — see world_host.h's
// header note on why the two stay separate classes).
struct WorldHostFixture {
    ChartFixture fix;
    EventsDecl events;
    PrefabSpawner spawner;
    std::string prefab_path;
    Toolchain toolchain;
    ScriptRuntime runtime;
    WorldHost host;
    ComponentHost component_host;

    explicit WorldHostFixture(const std::string& name)
        : spawner(
              fix.world, fix.hierarchy, fix.chart(), fix.bus(), fix.writer(), fix.registry, events),
          prefab_path(write_goblin_prefab(fix.dir)), toolchain(fresh_toolchain(name)),
          host(runtime, spawner), component_host(runtime, fix.world, fix.bus(), &fix.hierarchy) {
        fix.loop().set_structural_realizer(
            [this](std::uint64_t phase_record_id) { return spawner.realize(phase_record_id); });
    }
};

} // namespace

TEST_CASE("script.world_host: world.spawn(prefab, {at, overrides}) queues a pending handle "
          "that goes alive at phase 8") {
    WorldHostFixture wf("spawn");

    testkit::TempDir scripts_dir{"world-host-spawn"};
    const std::string path = scripts_dir.file("spawner.ts");
    const std::string source =
        "import {world} from 'midday/component'\n"
        "const ref = world.spawn('" +
        wf.prefab_path +
        "', {at: {x: 1, y: 2, z: 3}})\n"
        "if (ref.alive) throw new Error('expected a PENDING handle right after spawn()')\n"
        "if (ref.index !== " +
        std::to_string(kFirstSpawnedIndex) +
        " || ref.generation !== 0)\n"
        "    throw new Error('unexpected handle ' + ref.index + '#' + ref.generation)\n";
    REQUIRE_FALSE(base::write_file(path, source, "test.io").has_value());

    Toolchain::LoadOutcome loaded = wf.toolchain.load_module(wf.runtime, path);
    REQUIRE_MESSAGE(!loaded.error.has_value(),
                    (loaded.error ? loaded.error->message : std::string()));

    const EntityRef spawned{kFirstSpawnedIndex, 0};
    CHECK_FALSE(wf.fix.world.alive(spawned)); // still pending: the tick has not run yet
    CHECK(wf.spawner.pending_spawn_count() == 1);

    REQUIRE_FALSE(wf.fix.loop().tick().has_value());
    CHECK(wf.fix.world.alive(spawned));
    const math::Transform* local = wf.fix.hierarchy.local_of(spawned);
    REQUIRE(local != nullptr);
    CHECK(local->translation.x == 1.0F);
    CHECK(local->translation.y == 2.0F);
    CHECK(local->translation.z == 3.0F);
}

TEST_CASE("script.world_host: world.despawn(ref) queues; the handle stays alive until phase 8") {
    WorldHostFixture wf("despawn");

    // A live entity, spawned through the SAME PrefabSpawner but not through
    // the script (this test's own subject is the despawn boundary).
    loader::PrefabSpawnResult spawned = wf.spawner.spawn_prefab(wf.prefab_path, math::Vec3{});
    REQUIRE_FALSE(spawned.error.has_value());
    REQUIRE_FALSE(wf.fix.loop().tick().has_value());
    REQUIRE(wf.fix.world.alive(spawned.ref));
    REQUIRE(spawned.ref.index == kFirstSpawnedIndex);

    testkit::TempDir scripts_dir{"world-host-despawn"};
    const std::string path = scripts_dir.file("despawner.ts");
    const std::string source = "import {world, EntityRef} from 'midday/component'\n"
                               "const ref = new EntityRef(" +
                               std::to_string(spawned.ref.index) + ", " +
                               std::to_string(spawned.ref.generation) +
                               ")\n"
                               "world.despawn(ref)\n"
                               "if (!ref.alive) throw new Error('despawn must not apply "
                               "synchronously — it queues for phase 8')\n";
    REQUIRE_FALSE(base::write_file(path, source, "test.io").has_value());

    Toolchain::LoadOutcome loaded = wf.toolchain.load_module(wf.runtime, path);
    REQUIRE_MESSAGE(!loaded.error.has_value(),
                    (loaded.error ? loaded.error->message : std::string()));
    CHECK(wf.fix.world.alive(spawned.ref)); // still alive: queued, not applied

    REQUIRE_FALSE(wf.fix.loop().tick().has_value());
    CHECK_FALSE(wf.fix.world.alive(spawned.ref));
}

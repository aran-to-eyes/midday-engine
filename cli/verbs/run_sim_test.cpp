// cli.run_sim doctests (M2 node 0A) — the RunSim member-order FENCE. The
// pledge: `loaded` (the scene) outlives spawner/component_host/world_host,
// because PrefabSpawner aliases the scene's EventsDecl (prefab_spawn.h).
// Four layers, weakest-to-strongest:
//   1. offsetof static_asserts — a reorder is a compile error in EVERY lane.
//   2. address-order runtime check — portable backstop, no offsetof pragma.
//   3. an env+ASan-gated DEATH WITNESS: a real RunSim whose scene dies
//      EARLY (the exact bug the order prevents) performs a genuine
//      spawn_prefab deep read -> heap-use-after-free. ci.yml's
//      sanitizer-linux lane INVERTS it: the child run MUST die red — a
//      permanent, re-proven falsification, not a one-time note.
//   4. a green wired-teardown fixture: every route 0A wires is exercised
//      once against a fully-wired RunSim, then destroyed NORMALLY — under
//      the sanitizer lane this keeps real teardown ASan-covered forever.
// A runtime red on the UNMODIFIED type is impossible without an inserted
// read (nothing dereferences the scene during a correct teardown) — layers
// 1+3 together are the honest replacement for the pledge's literal
// "revert order -> ASan red" wording (fusion 2026-07-12, unanimous).

#include "cli/verbs/run_sim.h"
#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/loader/prefab_spawn.h"
#include "core/math/xform.h"
#include "core/reflect/builtin_events.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"
#include "ts/toolchain/toolchain.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

using namespace midday;
using midday::cli::detail::RunSim;
using midday::testkit::unwrap;

// ---- layer 1: the compile-time fence (every lane, every build) ------------
// offsetof on a non-standard-layout type is conditionally-supported
// (C++20 [support.types.layout]/1) — Clang/GCC accept it behind
// -Winvalid-offsetof, MSVC accepts this shape silently; the CI compiler
// matrix is the validation. Reordering run_sim.h's lifetime tail must be a
// compile error, not a review comment.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(RunSim, runtime) < offsetof(RunSim, instance_host),
              "run_sim.h: instance_host owns QuickJS seats — the runtime must outlive it");
static_assert(offsetof(RunSim, instance_host) < offsetof(RunSim, chart),
              "run_sim.h: instance_host is a chart-held ComponentHooks implementation (M2 0B "
              "D2) — declare it BEFORE `chart` (the `scripts` placement), NEVER in the "
              "post-`loaded` tail");
static_assert(offsetof(RunSim, chart) < offsetof(RunSim, loaded),
              "run_sim.h lifetime tail: `loaded` is the tail's head — keep it after `chart`");
static_assert(offsetof(RunSim, loaded) < offsetof(RunSim, spawner),
              "run_sim.h lifetime tail broken: PrefabSpawner aliases the scene's EventsDecl — "
              "`loaded` must be declared BEFORE `spawner` so the scene outlives it (#13)");
static_assert(offsetof(RunSim, spawner) < offsetof(RunSim, component_host),
              "run_sim.h lifetime tail broken: hosts must die before the spawner they ride");
static_assert(offsetof(RunSim, component_host) < offsetof(RunSim, world_host),
              "run_sim.h lifetime tail broken: world_host holds PrefabSpawner* — keep it last");
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// MSVC's /fsanitize=address defines NEITHER probe macro (__has_feature is
// Clang's, __SANITIZE_ADDRESS__ is GCC's), so the witness arms only on
// clang/gcc ASan builds — sanitizer-linux is the home lane; it cannot arm
// on MSVC.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MIDDAY_RUN_SIM_TEST_ASAN 1
#endif
#endif
#if !defined(MIDDAY_RUN_SIM_TEST_ASAN) && defined(__SANITIZE_ADDRESS__)
#define MIDDAY_RUN_SIM_TEST_ASAN 1
#endif

namespace {

// The fence corpus: one declared event, one prefab whose machine LISTENS on
// that declared event — so PrefabSpawner::catalog()'s load_entity_file walk
// must deep-read the scene-owned EventsDecl (the exact aliased data the
// member order protects). Returns the prefab path in generic (forward-slash)
// form: it is embedded verbatim into TS source (D-BUILD-113 precedent).
std::string write_probe_corpus(const testkit::TempDir& dir) {
    REQUIRE_FALSE(base::write_file(dir.file("probe.events.yaml"),
                                   "format: 1\n"
                                   "events:\n"
                                   "  probe.ping: {payload: {}}\n"
                                   "  probe.spark: {payload: {}}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("sparker.machine.yaml"),
                                   "format: 1\n"
                                   "machine: sparker\n"
                                   "regions:\n"
                                   "  main:\n"
                                   "    initial: Idle\n"
                                   "    states:\n"
                                   "      Idle:\n"
                                   "        on:\n"
                                   "          - {event: probe.spark, goto: Lit}\n"
                                   "      Lit: {}\n",
                                   "t")
                      .has_value());
    const std::string prefab_path =
        std::filesystem::path(dir.file("sparker.entity.yaml")).generic_string();
    REQUIRE_FALSE(base::write_file(prefab_path,
                                   "format: 1\n"
                                   "entity: Sparker\n"
                                   "machines:\n"
                                   "  - instance: {path: sparker.machine.yaml}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("probe.scene.yaml"),
                                   "format: 1\n"
                                   "scene: probe\n"
                                   "events: [probe.events.yaml]\n"
                                   "entities:\n"
                                   "  - entity: Host\n"
                                   "    components:\n"
                                   "      - Transform: {}\n",
                                   "t")
                      .has_value());
    return prefab_path;
}

// Wires `sim`'s non-scene collaborators in run_verb's exact program order
// (cli/verbs/run.cpp) up to and including the #13a spawner/realizer wiring
// — the composition under fence, never a lookalike. `scene` is the caller's
// so the witness can hand in a HEAP-owned scene (see the witness note); the
// runtime + host seats (run_verb's scripts conditional) are also the
// caller's to add — the witness does not need them.
void wire_composition(RunSim& sim, const testkit::TempDir& dir, loader::SceneFile& scene) {
    journal::WriterConfig config;
    config.engine_version = "selftest";
    journal::WriterOpenResult opened = journal::Writer::create(dir.file("fence.mrj"), config);
    REQUIRE_FALSE(opened.error.has_value());
    sim.writer.emplace(std::move(unwrap(opened.writer)));

    sim.bus.emplace(sim.world, sim.registry, *sim.writer);
    sim.loop.emplace(sim.world, sim.hierarchy, *sim.bus, *sim.writer);
    sim.chart.emplace(sim.world, sim.hierarchy, *sim.bus, *sim.writer, *sim.loop);
    REQUIRE_FALSE(loader::register_scene_events(scene, sim.registry).has_value());

    loader::SpawnResult spawned =
        loader::spawn_scene(scene, sim.world, sim.hierarchy, *sim.chart, nullptr, *sim.writer, 0);
    REQUIRE_FALSE(spawned.error.has_value());
    REQUIRE(spawned.stats.entities == 1); // Host = index 0, generation 0

    // The two-phase structural extension (M2 0B track D, D4): despawns run
    // exit chains + queue at prepare (pre-flush), spawns/reaps realize
    // post-flush. This is the wiring run.cpp itself must carry once the 0B
    // integration lands (spawner gains the loop tick source + BOTH slots;
    // ~RunSim clears both — the teardown contract below fences it).
    sim.spawner.emplace(sim.world,
                        sim.hierarchy,
                        *sim.chart,
                        *sim.bus,
                        *sim.writer,
                        sim.registry,
                        scene.events,
                        loader::ComponentVocab{},
                        &*sim.loop);
    sim.loop->set_structural_preparer(
        [&spawner = *sim.spawner](std::uint64_t tick, std::uint64_t phase_record_id) {
            return spawner.prepare(tick, phase_record_id);
        });
    sim.loop->set_structural_realizer([&spawner = *sim.spawner](std::uint64_t phase_record_id) {
        return spawner.realize(phase_record_id);
    });
}

// run_verb's own scene placement: owned by sim.loaded (run_sim.h's tail).
void wire_like_run_verb(RunSim& sim, const testkit::TempDir& dir) {
    reflect::register_builtin_events(sim.registry);
    loader::SceneLoadResult& loaded =
        sim.loaded.emplace(loader::load_scene(dir.file("probe.scene.yaml"), sim.registry));
    REQUIRE_FALSE(loaded.error.has_value());
    wire_composition(sim, dir, unwrap(loaded.scene));
}

} // namespace

// ---- layer 2: address-order backstop (portable, pragma-free) --------------
// Deliberately redundant with layer 1: this survives layer 1 being deleted
// or ifdef'd out (the offsetof pragma dance is the fragile part).
TEST_CASE("cli.run_sim: lifetime tail is declared in destruction-contract order") {
    RunSim sim;
    CHECK(static_cast<const void*>(&sim.runtime) < static_cast<const void*>(&sim.instance_host));
    CHECK(static_cast<const void*>(&sim.instance_host) < static_cast<const void*>(&sim.chart));
    CHECK(static_cast<const void*>(&sim.chart) < static_cast<const void*>(&sim.loaded));
    CHECK(static_cast<const void*>(&sim.loaded) < static_cast<const void*>(&sim.spawner));
    CHECK(static_cast<const void*>(&sim.spawner) < static_cast<const void*>(&sim.component_host));
    CHECK(static_cast<const void*>(&sim.component_host) <
          static_cast<const void*>(&sim.world_host));
}

// ---- layer 3: the sanitizer death witness ----------------------------------
// No-op unless MIDDAY_RUNSIM_UAF_CHILD=1 AND the build is ASan-instrumented:
// the body reproduces the exact bug the member order forbids (scene freed
// while the spawner still aliases its EventsDecl), so it MUST only run where
// ASan converts it into a contained, asserted red. ci.yml sanitizer-linux
// runs it as a child process and asserts the death + the
// heap-use-after-free report (inversion — the red IS the pass). If that
// red ever follows a deliberate refactor that makes PrefabSpawner own/copy
// its EventsDecl, retire this witness consciously WITH that refactor — the
// member order itself stays carried by layers 1+2.
TEST_CASE("cli.run_sim: teardown-uaf-witness-child dies under ASan when the scene dies early") {
    // std::getenv is standard and read-only; MSVC's C4996 wants _dupenv_s,
    // whose allocation dance buys nothing in a test. push/disable/pop, the
    // writer_test.cpp precedent.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* gate = std::getenv("MIDDAY_RUNSIM_UAF_CHILD");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (gate == nullptr || std::strcmp(gate, "1") != 0)
        return; // not the witness child: green no-op in every normal suite run
#if !defined(MIDDAY_RUN_SIM_TEST_ASAN)
    MESSAGE("MIDDAY_RUNSIM_UAF_CHILD=1 ignored: this build is not ASan-instrumented");
    return; // running the UAF without ASan would be genuine, unobserved UB
#else
    testkit::TempDir dir{"run-sim-uaf"};
    const std::string prefab_path = write_probe_corpus(dir);
    RunSim sim;
    reflect::register_builtin_events(sim.registry);

    // The scene is HEAP-boxed here, NOT placed in sim.loaded: `loaded` is an
    // inline optional, so resetting it destroys the SceneFile into storage
    // that stays legally alive (RunSim itself) — libc++'s ~vector even
    // leaves the destroyed EventsDecl reading as EMPTY (end=begin), so no
    // sanitizer on any platform can observe that read (probe-proven,
    // 2026-07-12). The lifetime violation modeled is IDENTICAL — the scene
    // dies while sim.spawner still aliases its EventsDecl (exactly what
    // run_sim.h's member order forbids) — but on the heap ASan poisons the
    // freed SceneFile, header included, on every stdlib.
    auto loaded = std::make_unique<loader::SceneLoadResult>(
        loader::load_scene(dir.file("probe.scene.yaml"), sim.registry));
    REQUIRE_FALSE(loaded->error.has_value());
    wire_composition(sim, dir, unwrap(loaded->scene));

    // THE deliberate wrong lifetime: the scene dies before the spawner.
    loaded.reset();

    // Genuine deep read, no test scaffolding: the prefab path is UNCACHED,
    // so catalog() -> load_entity_file -> the machine vocabulary closure
    // iterates the freed EventsDecl. ASan reports heap-use-after-free with
    // catalog/load_entity_file frames and aborts.
    loader::PrefabSpawnResult witnessed =
        unwrap(sim.spawner).spawn_prefab(prefab_path, math::Vec3{});
    (void)witnessed;
    FAIL("the witness child SURVIVED a read through a freed scene — the ASan fence is broken");
#endif
}

// ---- layer 4: the green wired-teardown fixture ------------------------------
// Every route 0A wires into `midday run`, exercised ONCE against the real
// composition, then torn down normally. __midday_trigger_named is
// deliberately NOT exercised: registered-unexercised, zero corpus callers
// (LEDGER 0A debt; doctest + onEvent land 0B).
TEST_CASE("cli.run_sim: wired routes run once against a real RunSim; teardown is clean") {
    testkit::TempDir dir{"run-sim-green"};
    const std::string prefab_path = write_probe_corpus(dir);
    RunSim sim;
    wire_like_run_verb(sim, dir);

    // run_verb's scripts-conditional wiring (run.cpp): runtime + both seats.
    script::ToolchainConfig tool_config;
    tool_config.cache_dir = ".midday-cache/selftest/run_sim_green";
    std::filesystem::remove_all(tool_config.cache_dir);
    sim.toolchain.emplace(std::move(tool_config));
    sim.runtime.emplace();
    sim.component_host.emplace(*sim.runtime, sim.world, unwrap(sim.bus), &sim.hierarchy);
    sim.world_host.emplace(*sim.runtime, unwrap(sim.spawner));

    // run_verb's --components wiring (M2 0B integration): the instance host
    // rides the same teardown fixture so its chart/spawner seams stay
    // ASan-covered — emplaced + wired exactly like run.cpp, even though this
    // corpus authors no script components (dormant routes torn down clean).
    sim.instance_host.emplace(*sim.runtime,
                              *sim.toolchain,
                              sim.world,
                              unwrap(sim.bus),
                              unwrap(sim.writer),
                              sim.registry,
                              *sim.component_host);
    unwrap(sim.spawner)
        .set_spawn_options(loader::SpawnOptions{&*sim.instance_host, /*defer_initial_entry=*/true});
    unwrap(sim.spawner).set_despawn_hooks(*sim.instance_host);

    // status + root + trigger_entity + world.spawn, one call each, asserted
    // from INSIDE the script (throw -> load error -> red).
    const std::string spawn_ts = dir.file("routes.ts");
    REQUIRE_FALSE(base::write_file(spawn_ts,
                                   "import {world, events, EntityRef} from 'midday/component'\n"
                                   "const host = new EntityRef(0, 0)\n"
                                   "if (!host.alive) throw new Error('scene host must be alive')\n"
                                   "const r = host.root()\n"
                                   "if (r.index !== 0 || r.generation !== 0)\n"
                                   "    throw new Error('unexpected root ' + r.index)\n"
                                   "events.trigger('probe.ping', {}, {key: host})\n"
                                   "const ref = world.spawn('" +
                                       prefab_path +
                                       "', {at: {x: 1, y: 2, z: 3}})\n"
                                       "if (ref.alive) throw new Error('spawn queues until "
                                       "phase 8 — must be pending here')\n"
                                       "if (ref.index !== 1 || ref.generation !== 0)\n"
                                       "    throw new Error('unexpected handle ' + ref.index)\n",
                                   "t")
                      .has_value());
    script::Toolchain::LoadOutcome loaded_routes =
        sim.toolchain->load_module(*sim.runtime, spawn_ts);
    REQUIRE_MESSAGE(!loaded_routes.error.has_value(),
                    (loaded_routes.error ? loaded_routes.error->message : std::string()));
    CHECK(unwrap(sim.spawner).pending_spawn_count() == 1);

    // step_one() drives the realize route (phase 8: flush + realizer).
    REQUIRE_FALSE(sim.step_one().has_value());
    const ecs::EntityRef spawned{1, 0};
    CHECK(sim.world.alive(spawned));

    const std::string despawn_ts = dir.file("despawn.ts");
    REQUIRE_FALSE(base::write_file(despawn_ts,
                                   "import {world, EntityRef} from 'midday/component'\n"
                                   "const ref = new EntityRef(1, 0)\n"
                                   "world.despawn(ref)\n"
                                   "if (!ref.alive) throw new Error('despawn queues until "
                                   "phase 8 — must still be alive here')\n",
                                   "t")
                      .has_value());
    script::Toolchain::LoadOutcome loaded_despawn =
        sim.toolchain->load_module(*sim.runtime, despawn_ts);
    REQUIRE_MESSAGE(!loaded_despawn.error.has_value(),
                    (loaded_despawn.error ? loaded_despawn.error->message : std::string()));
    CHECK(unwrap(sim.spawner).pending_despawn_count() == 1);

    REQUIRE_FALSE(sim.step_one().has_value());
    CHECK_FALSE(sim.world.alive(spawned));
    // Scope exit destroys the fully-wired RunSim in declaration-reverse
    // order — the sanitizer lane re-proves this teardown clean every run.
}

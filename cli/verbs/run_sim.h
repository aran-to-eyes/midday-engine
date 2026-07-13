// cli/verbs/run_sim.h — the canonical `midday run` sim composition (M2 node
// 0A, plan seam #1). INTERNAL detail of the run verb: include this ONLY from
// cli/verbs/run.cpp and cli/verbs/run_sim_test.cpp (the member-order fence) —
// it is not public API and never reaches engine_api.json.
//
// Destruction order = reverse declaration — that ordering IS the teardown
// contract (M2_INBOX #13): hosts -> spawner -> scene(`loaded`) -> chart ->
// physics -> instance_host -> scripts/runtime/toolchain -> pack -> loop ->
// bus -> writer -> hierarchy -> world -> registry. PrefabSpawner holds
// `const EventsDecl*` into `loaded`'s SceneFile (prefab_spawn.h), so
// `loaded` MUST outlive the spawner and the tail hosts; `instance_host`
// (like `scripts`) is a chart-held hooks implementation, so it must outlive
// `chart` (M2 0B, D2). run_sim_test.cpp fences the order three ways
// (offsetof static_asserts, an address-order runtime check, and an ASan
// death witness in the sanitizer lane).

#pragma once

#include "cli/verbs/run_assert.h"
#include "core/base/error.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/loader/loader.h"
#include "core/loader/prefab_spawn.h"
#include "core/physics/physics_server.h"
#include "core/reflect/registry.h"
#include "core/statechart/statechart.h"
#include "core/tick/tick_loop.h"
#include "ts/runtime/component_host.h"
#include "ts/runtime/component_instance_host.h"
#include "ts/runtime/script_runtime.h"
#include "ts/runtime/state_script.h"
#include "ts/runtime/world_host.h"
#include "ts/toolchain/toolchain.h"

#include <cassert>
#include <memory>
#include <optional>

namespace midday::cli::detail {

// The chart detaches its hooks before physics dies; the script host outlives
// the chart per the StateHooks lifetime contract; the assert pack detaches
// its driver hook/subscription before the loop and bus die.
struct RunSim {
    reflect::Registry registry;
    ecs::World world{registry};
    hierarchy::Hierarchy hierarchy{world};
    std::optional<journal::Writer> writer;
    std::optional<bus::Bus> bus;
    std::optional<tick::TickLoop> loop;
    std::unique_ptr<RunAssertPack> pack;
    std::optional<script::Toolchain> toolchain;
    std::optional<script::ScriptRuntime> runtime;
    std::optional<script::StateScriptHost> scripts;
    // The TS component-instance host (M2 0B, #12b): like `scripts`, it
    // implements chart-held hook interfaces (statechart::ComponentHooks),
    // so it MUST outlive `chart` — declared BEFORE it, never in the
    // post-`loaded` tail (the spawner only borrows it as DespawnHooks and
    // dies first; ~ComponentInstanceHost touches ONLY `bus` — the teardown
    // fence: it unsubscribes its still-subscribed seat listeners, and `bus`
    // is declared above so it outlives the host; the tail hosts dying
    // earlier is fine). Fenced in run_sim_test.cpp.
    std::optional<script::ComponentInstanceHost> instance_host;
    std::unique_ptr<physics::PhysicsServer> physics;
    std::optional<statechart::Statechart> chart;
    // Lifetime tail — reverse destruction IS the contract (#13):
    // hosts -> spawner -> scene(loaded); any future member that aliases
    // the scene MUST be declared after `loaded`.
    std::optional<loader::SceneLoadResult> loaded;
    std::optional<loader::PrefabSpawner> spawner;
    std::optional<script::ComponentHost> component_host;
    std::optional<script::WorldHost> world_host;

    RunSim() = default;
    RunSim(const RunSim&) = delete;
    RunSim& operator=(const RunSim&) = delete;
    RunSim(RunSim&&) = delete;
    RunSim& operator=(RunSim&&) = delete;

    // Teardown hygiene (council 0A): TickLoop's realizer_ captures &*spawner
    // (the #13a wiring), and `spawner` dies BEFORE `loop` — reverse
    // declaration order, the very contract above. Without this, the stored
    // std::function would hold a dangling reference between ~spawner and
    // ~loop; nothing can call it there (realize only runs inside tick()),
    // but this node exists to kill exactly that class of latent teardown
    // dangling. Empty realizer is the documented unset state (tick_loop.h).
    // M2 0B track D: the extension is now TWO-PHASE (tick_loop.h D4) — the
    // preparer_ slot captures the same &*spawner once run.cpp wires the
    // despawn-linger half, so BOTH slots clear here, symmetrically.
    ~RunSim() {
        if (loop.has_value()) {
            loop->set_structural_preparer({});
            loop->set_structural_realizer({});
        }
    }

    // THE single tick executor (plan seam #1): batch, bridge, testkit, and
    // replay all drive this one function — no second executor may ever
    // exist. Caller contract: `loop` is emplaced before the first step
    // (run_verb's wiring order) — the assert arms the Debug lanes (dev,
    // sanitizer) with that precondition; Release keeps the documented
    // contract. The suppression stays because the assert does not satisfy
    // the tidy check, and a runtime error path no caller could ever reach
    // is still wrong.
    [[nodiscard]] std::optional<base::Error> step_one() {
        assert(loop.has_value() && "step_one: wire the composition (loop) before stepping");
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return loop->tick();
    }
};

} // namespace midday::cli::detail

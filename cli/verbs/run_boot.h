// cli/verbs/run_boot.h — the run verb's TS-tier boot (M2 0B integration).
// INTERNAL detail of `midday run`: include this ONLY from cli/verbs/run.cpp
// (the run_sim.h precedent) — it is not public API and never reaches
// engine_api.json.
//
// One boot, two call sites: the --components path boots the tier BEFORE
// spawn (the loader dispatcher needs a live materializer during
// spawn_scene, D2), the m0 path boots it after spawn when state scripts
// exist. Hoisted so the two sites cannot drift.

#pragma once

#include "cli/verb.h"
#include "cli/verbs/run_sim.h"
#include "core/base/name.h"
#include "core/math/rng.h"

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace midday::cli::detail {

// __midday_rng(stream): seeded, NAMED Philox child streams for state
// scripts — the SIM profile's deterministic replacement for the poisoned
// Math.random. Derivation: run seed -> child("scripts") -> child(<stream>);
// each named stream persists for the run, so per-tick draws advance a pure
// (seed, stream, draw-count) function of the executed program. The CLI owns
// this ambient sim service until the API tier formalizes RNG bindings.
inline void register_script_rng(script::ScriptRuntime& runtime, std::uint64_t seed) {
    struct Streams {
        math::RngStream root;
        std::map<std::string, math::RngStream> named;
    };

    auto streams = std::make_shared<Streams>(
        Streams{.root = math::RngStream(seed).child(base::Name("scripts")), .named = {}});
    runtime.register_host_fn("__midday_rng", [streams](const Json::Array& args) {
        script::HostResult result;
        if (args.size() != 1 || !args[0].is_string()) {
            result.error = Error{.code = "script.rng_args",
                                 .message = "__midday_rng expects (stream: string)"};
            return result;
        }
        const std::string& name = args[0].as_string();
        auto [it, fresh] = streams->named.try_emplace(name, streams->root.child(base::Name(name)));
        (void)fresh;
        result.value = Json(it->second.uniform_double());
        return result;
    });
}

// Toolchain (content-hash cache), the SIM runtime (poisoned clocks, gas
// metering), the rng seam, and the entity/emit primitives seat — with the
// registry wired so the trigger seats speak the authoring->wire payload
// adapter (component_host.h set_registry). Caller contract: writer and bus
// are emplaced; call at most once per RunSim.
inline void boot_ts_tier(RunSim& sim, const VerbArgs& args, std::int64_t seed) {
    // Caller contract (run_verb's wiring order): the bus is emplaced before
    // any TS boot — the assert arms the Debug lanes with that precondition
    // (the run_sim.h step_one discipline); a runtime error path no caller
    // could ever reach is still wrong, hence the suppression.
    assert(sim.bus.has_value() && "boot_ts_tier: emplace the bus before booting the TS tier");
    script::ToolchainConfig tool_config;
    if (args.present("cache-dir"))
        tool_config.cache_dir = args.get_string("cache-dir");
    sim.toolchain.emplace(std::move(tool_config));
    sim.runtime.emplace();
    register_script_rng(*sim.runtime, static_cast<std::uint64_t>(seed));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    sim.component_host.emplace(*sim.runtime, sim.world, *sim.bus, &sim.hierarchy);
    sim.component_host->set_registry(sim.registry);
}

} // namespace midday::cli::detail

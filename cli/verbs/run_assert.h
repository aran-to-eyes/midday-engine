// cli/verbs/run_assert.h — registered assertion packs for `midday run
// --assert case=<name>` (m0-appendix-a-determinism). A pack rides a live
// headless run as its missing-content stand-in (driver systems injected at
// the A.1 phases the spec assigns them) and, after the journal closes,
// walks the recorded causality stream plus its own live probes into named
// boolean verdicts the envelope reports (`.assertions.<name>` — the
// MILESTONE_0 item-21 exit test jq's them).
//
// The flagship pack, "appendix_a_golden", is MIDDAY_ENGINE_SPEC.md Appendix
// A.3 executed end to end from the authored examples/appendix_a corpus:
// the pack plays the player + damage system that land with m1 content
// (D-BUILD-081 recorded the gap; D-BUILD-083 records the stand-in), then
// asserts every named tick, transition, void, hook order, span close, and
// cause chain of the normative trace.

#pragma once

#include "cli/envelope.h"
#include "core/base/error.h"
#include "core/base/name.h"
#include "core/ecs/entity.h"
#include "core/loader/loader.h"
#include "core/statechart/component_hooks.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::bus {
class Bus;
}

namespace midday::physics {
struct PhysicsStats;
}

namespace midday::ecs {
class World;
}

namespace midday::hierarchy {
class Hierarchy;
}

namespace midday::journal {
class Writer;
}

namespace midday::reflect {
class Registry;
}

namespace midday::statechart {
class Statechart;
}

namespace midday::tick {
class TickLoop;
}

namespace midday::cli {

class RunAssertPack {
public:
    RunAssertPack() = default;
    RunAssertPack(const RunAssertPack&) = delete;
    RunAssertPack& operator=(const RunAssertPack&) = delete;
    RunAssertPack(RunAssertPack&&) = delete;
    RunAssertPack& operator=(RunAssertPack&&) = delete;
    virtual ~RunAssertPack() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;

    // Register the pack's driver hooks. Called BEFORE the Statechart
    // attaches its phase hooks, so pack systems run first within their
    // phase (A.1 phase 5: pre-update systems, then onFixedUpdate hooks).
    // The collaborators must outlive the pack; the pack detaches in its
    // destructor (before the loop/bus die — RunSim member order).
    // `registry` is the run's event vocabulary (builtins + scene events) —
    // what a pack needs to DECODE-verify canonical payload bytes against
    // their schemas (D5: replay reads bytes, never the projection).
    virtual std::optional<Error> attach(ecs::World& world,
                                        hierarchy::Hierarchy& hierarchy,
                                        bus::Bus& bus,
                                        tick::TickLoop& loop,
                                        journal::Writer& writer,
                                        const reflect::Registry& registry) = 0;

    // Resolve the pack's scene expectations against the spawned world
    // (named entities, machine seats) and journal the pack's presence
    // (FLIGHT "assert.case" citing `cause_id` — a driven run says so in
    // its own causality stream).
    virtual std::optional<Error> bind(statechart::Statechart& chart,
                                      const loader::SpawnResult& spawned,
                                      std::uint64_t cause_id) = 0;

    // Live run measurements the host hands to evaluate(): counters no
    // journal walk can reconstruct (the run host owns the script runtime
    // and the physics server; packs never do). gc_alloc_* bracket the TICK
    // WINDOW — module build/bind allocation is excluded by construction.
    struct RunProbes {
        std::uint64_t ticks = 0;
        std::uint64_t gc_alloc_before_ticks = 0;
        std::uint64_t gc_alloc_after_ticks = 0;
        const physics::PhysicsStats* physics = nullptr; // null: scene has no physics
    };

    // Post-close: walk the recorded bundle + live probes into the named
    // verdicts. `assertions` reports every named value (the envelope
    // carries it verbatim); `failed` lists the names that did not hold.
    // `exercised` is the item-25 surface (`.exercised.*` in the envelope):
    // packs without an exercised contract leave it null.
    struct Verdict {
        Json assertions = Json::object();
        Json exercised;
        std::vector<std::string> failed;
        std::optional<Error> error; // bundle unreadable etc. (infrastructure)
    };

    virtual Verdict
    evaluate(statechart::Statechart& chart, const std::string& bundle, const RunProbes& probes) = 0;
};

// The corpus actors every driven pack binds: the named entity's machine
// seat (+ its host ref) and one named marker entity (SceneEntity tag walk).
// Absence reads as kInvalidMachine / a null ref — the pack turns that into
// its own run.assert_scene refusal. Hoisted at M2 0B on the second-consumer
// rule (appendix_a_golden + component_event_lifecycle bind identically).
struct BoundActors {
    statechart::MachineId machine = statechart::kInvalidMachine;
    ecs::EntityRef host;   // the machine entity's host ref
    ecs::EntityRef marker; // the named marker entity
};

BoundActors locate_actors(ecs::World& world,
                          const loader::SpawnResult& spawned,
                          base::Name machine_entity,
                          base::Name marker_entity);

// The pack registry (manifest style, cli/verbs/registry.cpp precedent):
// nullptr when `name` is unknown — the caller reports assert_pack_names().
std::unique_ptr<RunAssertPack> make_assert_pack(std::string_view name);
std::string assert_pack_names();

// The "determinism_kata" pack (run_assert_kata.cpp — registry factory).
std::unique_ptr<RunAssertPack> make_determinism_kata_pack();

// The "component_event_lifecycle" pack (run_assert_lifecycle.cpp — the M2
// 0B D6 golden: typed hydration, the 7-line exit chain, canonical payload
// bytes, exact-tick despawn reap).
std::unique_ptr<RunAssertPack> make_component_event_lifecycle_pack();

} // namespace midday::cli

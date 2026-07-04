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
#include "core/loader/loader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::bus {
class Bus;
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
    virtual std::optional<Error> attach(ecs::World& world,
                                        hierarchy::Hierarchy& hierarchy,
                                        bus::Bus& bus,
                                        tick::TickLoop& loop,
                                        journal::Writer& writer) = 0;

    // Resolve the pack's scene expectations against the spawned world
    // (named entities, machine seats) and journal the pack's presence
    // (FLIGHT "assert.case" citing `cause_id` — a driven run says so in
    // its own causality stream).
    virtual std::optional<Error> bind(statechart::Statechart& chart,
                                      const loader::SpawnResult& spawned,
                                      std::uint64_t cause_id) = 0;

    // Post-close: walk the recorded bundle + live probes into the named
    // verdicts. `assertions` reports every named value (the envelope
    // carries it verbatim); `failed` lists the names that did not hold.
    struct Verdict {
        Json assertions = Json::object();
        std::vector<std::string> failed;
        std::optional<Error> error; // bundle unreadable etc. (infrastructure)
    };

    virtual Verdict evaluate(statechart::Statechart& chart, const std::string& bundle) = 0;
};

// The pack registry (manifest style, cli/verbs/registry.cpp precedent):
// nullptr when `name` is unknown — the caller reports assert_pack_names().
std::unique_ptr<RunAssertPack> make_assert_pack(std::string_view name);
std::string assert_pack_names();

} // namespace midday::cli

// cli/verbs/run_assert_kata.cpp — the "determinism_kata" pack
// (m0-determinism-spike; MILESTONE_0 item 25, GRAFT Zenith D024). The kata
// scene is SELF-DRIVING (examples/spikes/determinism.scene.yaml), so unlike
// the flagship pack this one injects nothing: it rides along, then turns
// the recorded run plus the host's live probes into the four `.exercised.*`
// axes the determinism lane must see MOVE before it trusts any
// byte-compare — a gutted or half-wired scene fails the run (exit 1), it
// never silently passes an empty compare.
//
// Activity floors derive from the AUTHORED corpus (kata.machine.yaml
// header, 60 Hz, 600-tick canonical run = 20 drive periods), pinned near
// HALF the designed activity: real regressions (a dead region, a script
// that stopped allocating, physics not stepping) crash through them while
// boundary effects (settled debris, partial last period) never graze them.

#include "cli/verbs/run_assert.h"
#include "cli/verbs/run_assert_walk.h"
#include "core/journal/writer.h"
#include "core/physics/physics_server.h"
#include "core/statechart/statechart.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace midday::cli {
namespace {

using journal::Record;

// ---- the authored activity floors (design math, never tuned to output) ------
constexpr std::uint64_t kMinTransitions = 70;                // >= 7 per 30-tick period x 20
constexpr std::uint64_t kMinSpanEdges = 12;                  // 1 Surge open + close per period
constexpr std::uint64_t kMinContactTriggers = 4;             // agent + three debris land
constexpr std::uint64_t kMinGcChurnBytes = 128ULL * 1024ULL; // ~480 churn bursts move MBs
constexpr std::uint64_t kMinChurnEvents = 240;               // 24 kata.churn emits per period
constexpr std::size_t kMinDistinctSums = 8;                  // seeded draws vary the payloads

class DeterminismKataPack final : public RunAssertPack {
public:
    [[nodiscard]] std::string_view name() const override { return "determinism_kata"; }

    std::optional<Error> attach(ecs::World& /*world*/,
                                hierarchy::Hierarchy& /*hierarchy*/,
                                bus::Bus& /*bus*/,
                                tick::TickLoop& /*loop*/,
                                journal::Writer& writer,
                                const reflect::Registry& /*registry*/) override {
        writer_ = &writer;
        return std::nullopt;
    }

    std::optional<Error> bind(statechart::Statechart& /*chart*/,
                              const loader::SpawnResult& spawned,
                              std::uint64_t cause_id) override {
        bool seated = false;
        for (const loader::MachineSeat& seat : spawned.machines)
            seated = seated || seat.entity == base::Name("Agent");
        if (!seated || spawned.stats.bodies == 0) {
            Error error{.code = "run.assert_scene",
                        .message = "determinism_kata expects the examples/spikes corpus: "
                                   "entity 'Agent' carrying the kata machine plus physics bodies"};
            return error;
        }
        return assertwalk::journal_case_presence(*writer_, name(), cause_id);
    }

    Verdict evaluate(statechart::Statechart& chart,
                     const std::string& bundle,
                     const RunProbes& probes) override;

private:
    struct Facts; // journal-walk collector (below)

    journal::Writer* writer_ = nullptr;
};

// One streaming pass: count every record kind the four axes cite, and keep
// the distinct kata.churn sums — seeded RNG variation, journaled.
struct DeterminismKataPack::Facts {
    std::uint64_t transitions = 0;
    std::uint64_t span_opens = 0;
    std::uint64_t span_closes = 0;
    std::uint64_t contact_triggers = 0;
    std::uint64_t churn_events = 0;
    std::set<double> churn_sums;

    void collect(const Record& record) {
        if (record.kind == "statechart.transition") {
            ++transitions;
            return;
        }
        if (record.kind == "sequence.span_open") {
            ++span_opens;
            return;
        }
        if (record.kind == "sequence.span_close") {
            ++span_closes;
            return;
        }
        if (assertwalk::is_event_trigger(record, "contact.began")) {
            ++contact_triggers;
            return;
        }
        if (!assertwalk::is_event_trigger(record, "kata.churn"))
            return;
        ++churn_events;
        // Trigger records nest the EVENT payload under "payload" (the outer
        // object is the bus's dispatch envelope: event, key, subscribers).
        const Json* body = record.payload.find("payload");
        const Json* sum = body != nullptr && body->is_object() ? body->find("sum") : nullptr;
        if (sum != nullptr && sum->is_double())
            churn_sums.insert(sum->as_double());
        else if (sum != nullptr && sum->is_int())
            churn_sums.insert(static_cast<double>(sum->as_int()));
    }
};

RunAssertPack::Verdict DeterminismKataPack::evaluate(statechart::Statechart& /*chart*/,
                                                     const std::string& bundle,
                                                     const RunProbes& probes) {
    Verdict verdict;
    Facts facts;
    if (auto error = assertwalk::walk_bundle(bundle, facts)) {
        verdict.error = std::move(error);
        return verdict;
    }

    // ---- the four exercised axes (exact item-25 spellings) ------------------
    // ts_gc_churn: the cumulative JS alloc counter moved across the TICK
    // window (module build/bind is bracketed out by the host) — per-tick
    // script allocation really happened, at agent scale.
    const std::uint64_t gc_delta = probes.gc_alloc_after_ticks - probes.gc_alloc_before_ticks;
    const bool ts_gc_churn = gc_delta >= kMinGcChurnBytes;
    // jolt_step: physics stepped EVERY tick and its contacts reached the
    // journal through the bus (phase-6 dispatch — not merely an idle world).
    const bool jolt_step = probes.physics != nullptr && probes.ticks > 0 &&
                           probes.physics->steps >= probes.ticks &&
                           facts.contact_triggers >= kMinContactTriggers;
    const bool statechart_transitions = facts.transitions >= kMinTransitions;
    const bool sequence_spans =
        facts.span_opens >= kMinSpanEdges && facts.span_closes >= kMinSpanEdges;
    // The fifth, non-exercised verdict: seeded RNG streams flowed through
    // the scripts into journaled payloads, and they VARIED (a constant
    // stream would journal one distinct sum).
    const bool rng_streams_flowed =
        facts.churn_events >= kMinChurnEvents && facts.churn_sums.size() >= kMinDistinctSums;

    struct Named {
        const char* name;
        bool passed;
    };

    const Named exercised[] = {
        {"ts_gc_churn", ts_gc_churn},
        {"jolt_step", jolt_step},
        {"statechart_transitions", statechart_transitions},
        {"sequence_spans", sequence_spans},
    };

    verdict.exercised = Json::object();
    for (const Named& axis : exercised) {
        verdict.exercised.set(axis.name, axis.passed);
        if (!axis.passed)
            verdict.failed.emplace_back(axis.name);
    }
    verdict.assertions.set("rng_streams_flowed", rng_streams_flowed);
    if (!rng_streams_flowed)
        verdict.failed.emplace_back("rng_streams_flowed");

    // Diagnostics: the raw counts behind every verdict, so a red lane names
    // the dead axis without a rerun.
    verdict.assertions.set("transitions", static_cast<std::int64_t>(facts.transitions));
    verdict.assertions.set("span_opens", static_cast<std::int64_t>(facts.span_opens));
    verdict.assertions.set("span_closes", static_cast<std::int64_t>(facts.span_closes));
    verdict.assertions.set("contact_triggers", static_cast<std::int64_t>(facts.contact_triggers));
    verdict.assertions.set("churn_events", static_cast<std::int64_t>(facts.churn_events));
    verdict.assertions.set("distinct_churn_sums",
                           static_cast<std::int64_t>(facts.churn_sums.size()));
    verdict.assertions.set("gc_alloc_delta_bytes", static_cast<std::int64_t>(gc_delta));
    verdict.assertions.set(
        "physics_steps",
        static_cast<std::int64_t>(probes.physics != nullptr ? probes.physics->steps : 0));
    return verdict;
}

} // namespace

std::unique_ptr<RunAssertPack> make_determinism_kata_pack() {
    return std::make_unique<DeterminismKataPack>();
}

} // namespace midday::cli

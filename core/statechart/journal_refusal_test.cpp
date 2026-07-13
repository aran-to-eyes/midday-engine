// statechart.journal_refusal — "no record, no effect" at the chart (M2 0B
// council fix G3, the C1 class at its last unchecked site): a poisoned
// writer refusing the statechart.transition record aborts the transition
// BEFORE the region stamp, the exit/enter chains, and the loser voiding —
// zero mutation; a hook record refused MID-chain skips exactly that hook's
// invocation (state script and component slots alike) while the structural
// transition completes — never a half-exited machine, never a JS hook
// running with record id 0. The poisoning idiom is despawn_journal_test's:
// citing a never-consumed cause is journal.cause_unknown, the sticky error.

#include "core/statechart/component_hooks.h"
#include "core/statechart/test_support.h"

#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Json;
using midday::base::Name;

namespace {

// Sticky-poisons the fixture's writer: the never-consumed cause 9999 is
// journal.cause_unknown, and record() returns 0 from here on.
void poison_writer(ChartFixture& fix) {
    CHECK(fix.writer().record(
              fix.bus().tick(), journal::Tier::Flight, "poison", 9999, Json::object()) == 0);
    REQUIRE(fix.writer().status().has_value());
}

} // namespace

TEST_CASE("statechart.journal_refusal: a refused statechart.transition record aborts BEFORE the "
          "stamp and the chains — the region never moves, no hook runs, no loser voids") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks plain(log);
    RecordingHooks poisoner(log);
    // The FIRST region's enter hook poisons the writer mid-dispatch: the
    // enclosing event.trigger record succeeded, r1's transition journaled,
    // and r2's transition record is the first refusal — exactly the
    // reachable mid-cascade window.
    poisoner.enter_action = [&fix](Statechart&, const StateHookContext&) { poison_writer(fix); };

    const MachineId id = fix.spawn_machine(
        machine("twin",
                {region("r1", "A1", {state("A1", {pair("evt", "B1")}), state("B1")}),
                 region("r2", "A2", {state("A2", {pair("evt", "B2")}), state("B2")})}));
    Statechart& chart = fix.chart();
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("r1"), Name("A1"), plain).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("r1"), Name("B1"), poisoner).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("r2"), Name("A2"), plain).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("r2"), Name("B2"), plain).has_value());

    // The trigger's own record precedes the poison, so dispatch runs: r1
    // transitions (and poisons from B1's enter), r2's record then refuses.
    REQUIRE_FALSE(fix.trigger("evt").error.has_value());

    // r1 moved; r2 aborted with ZERO mutation — no stamp, no chains, no
    // stats, and its region still answers the initial state.
    const std::vector<std::string> expected = {"exit:A1", "enter:B1"};
    CHECK(log == expected);
    CHECK(chart.stats().transitions == 1);
    CHECK(chart.stats().voided == 0);
    CHECK(chart.in_state(id, Name("r1"), Name("B1")));
    CHECK(chart.in_state(id, Name("r2"), Name("A2")));
    CHECK(chart.active_state(id, Name("r2")) == Name("A2"));

    // The poisoned writer surfaces loudly at the very next effect: the bus
    // refuses the next trigger record ("no record, no dispatch").
    bus::TriggerResult refused = fix.trigger("evt");
    CHECK(unwrap(refused.error).code == "bus.journal_refused");
    // A poisoned bundle gets no readback: the test ends without finish().
}

TEST_CASE("statechart.journal_refusal: a hook record refused MID-chain skips exactly that "
          "invocation — component hooks and state scripts stay silent, the structural "
          "transition still completes (no half-exited machine)") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks plain(log);
    RecordingHooks poisoner(log);
    RecordingComponentHooks components(log);
    // Alive's own onExit runs FIRST in the exit chain (A.2.1 exit 1) and
    // poisons: every LATER hook record — Armed's script exit, both states'
    // component exit-3 slots, Dead's enter — refuses.
    poisoner.exit_action = [&fix](Statechart&, const StateHookContext&) { poison_writer(fix); };

    // The D6 nested shape: Alive { Armed } with components on both levels.
    StateDesc armed = state("Armed");
    StateDesc alive = state("Alive", {pair("golden.kill", "Dead")});
    alive.substates.push_back(armed);
    alive.initial = Name("Armed");
    MachineDesc desc = machine("probe", {region("life", "Alive", {alive, state("Dead")})});

    InstantiateOptions defer;
    defer.defer_initial_entry = true;
    InstantiateResult made = fix.chart().instantiate(desc, fix.host, 0, defer);
    REQUIRE_FALSE(made.error.has_value());
    const MachineId id = made.machine;
    Statechart& chart = fix.chart();
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name("Alive"), poisoner).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name("Armed"), plain).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name("Dead"), plain).has_value());
    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Alive"), Name("ParentA"), components)
            .has_value());
    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Armed"), Name("ChildA"), components)
            .has_value());
    REQUIRE_FALSE(chart.start_initial_entry(id).has_value());
    CHECK(chart.stats().component_hook_calls == 2); // the healthy entry chain
    log.clear();

    REQUIRE_FALSE(fix.trigger("golden.kill").error.has_value());

    // Alive's onExit ran (its record preceded the poison); EVERYTHING after
    // it skipped: Armed's script exit, both component_exit slots, Dead's
    // enter — no invocation without its record.
    const std::vector<std::string> expected = {"exit:Alive"};
    CHECK(log == expected);
    CHECK(chart.stats().component_hook_calls == 2); // unchanged: exits skipped
    CHECK(chart.stats().hook_calls == 3);           // 2 entry hooks + Alive's exit

    // The structural transition COMPLETED — the abort point never leaves a
    // half-exited machine: Dead is active, the Alive chain fully retired.
    CHECK(chart.stats().transitions == 1);
    CHECK(chart.in_state(id, Name("life"), Name("Dead")));
    CHECK_FALSE(chart.in_state(id, Name("life"), Name("Alive")));
    CHECK_FALSE(chart.in_state(id, Name("life"), Name("Armed")));
    // A poisoned bundle gets no readback: the test ends without finish().
}

// statechart.component_hooks — the A.2.1 exit-3 / enter-2 slots get their
// real invocation target (M2 0B, #12b; core/statechart/component_hooks.h):
// component onEnter runs in ATTACH order after the subtree activates and
// before the substate enters; component onExit runs in REVERSE attach order
// after the substate exits and before the subtree deactivates. Applied over
// the D6 golden's nested shape (parent state with two components over a
// child state with two components) the observable exit order is
//   parent script onExit -> child script onExit -> ChildB -> ChildA
//   -> ParentB -> ParentA -> deactivate
// — the exact 7-line chain the component_event_lifecycle golden pins.
// Plus the chart-instantiation SPLIT (topology-create vs
// initial-entry-start): hooks seated between instantiate(defer) and
// start_initial_entry() DO fire on the initial chains — the gpt D2 critical
// requirement (a late synthetic onEnter is unacceptable).

#include "core/statechart/component_hooks.h"
#include "core/statechart/test_support.h"

#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Name;
using midday::journal::Record;

namespace {

// The D6 golden's machine shape: life region, Alive { Armed } initial with
// components on both levels, Dead as the kill target.
MachineDesc lifecycle_machine() {
    StateDesc armed = state("Armed");
    StateDesc alive = state("Alive", {pair("golden.kill", "Dead")});
    alive.substates.push_back(armed);
    alive.initial = Name("Armed");
    return machine("probe_brain", {region("life", "Alive", {alive, state("Dead")})});
}

} // namespace

TEST_CASE("statechart.component_hooks: enter attach order, exit REVERSE attach order, "
          "interleaved with state scripts per A.2.1") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks scripts(log);
    RecordingComponentHooks components(log);

    InstantiateOptions defer;
    defer.defer_initial_entry = true;
    InstantiateResult made = fix.chart().instantiate(lifecycle_machine(), fix.host, 0, defer);
    REQUIRE_FALSE(made.error.has_value());
    const MachineId id = made.machine;
    Statechart& chart = fix.chart();

    // Seat everything BEFORE initial entry (the split's whole point):
    // scripts on all three states, components in authored/attach order.
    for (const char* name : {"Alive", "Armed", "Dead"})
        REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name(name), scripts).has_value());
    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Alive"), Name("ParentExitA"), components)
            .has_value());
    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Alive"), Name("ParentExitB"), components)
            .has_value());
    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Armed"), Name("ChildExitA"), components)
            .has_value());
    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Armed"), Name("ChildExitB"), components)
            .has_value());
    REQUIRE_FALSE(chart.start_initial_entry(id).has_value());

    // Initial ENTRY: enter-1 subtree, enter-2 components (attach order),
    // enter-3 substate, enter-4 script — outer first, script LAST per level.
    const std::vector<std::string> entry_expected = {
        "c-enter:Alive/ParentExitA",
        "c-enter:Alive/ParentExitB",
        "c-enter:Armed/ChildExitA",
        "c-enter:Armed/ChildExitB",
        "enter:Armed",
        "enter:Alive",
    };
    CHECK(log == entry_expected);
    CHECK(chart.in_state(id, Name("life"), Name("Armed")));
    CHECK(chart.stats().component_hook_calls == 4);

    // The kill EXIT: brain first, then substates deepest-completing, each
    // level's components in REVERSE attach order (exit-3), then Dead enters.
    log.clear();
    REQUIRE_FALSE(fix.trigger("golden.kill").error.has_value());
    const std::vector<std::string> exit_expected = {
        "exit:Alive",
        "exit:Armed",
        "c-exit:Armed/ChildExitB",
        "c-exit:Armed/ChildExitA",
        "c-exit:Alive/ParentExitB",
        "c-exit:Alive/ParentExitA",
        "enter:Dead",
    };
    CHECK(log == exit_expected);
    CHECK(chart.in_state(id, Name("life"), Name("Dead")));

    // Journal: component hook records are statechart.hook records with a
    // component field, hook component_enter/component_exit, citing the
    // instantiate record (entry) / the transition record (exit).
    std::vector<Record> records = fix.finish();
    const std::vector<Record> instantiated = of_kind(records, "statechart.instantiate");
    const std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE(instantiated.size() == 1);
    REQUIRE(transitions.size() == 1);
    const std::vector<Record> hooks = of_kind(records, "statechart.hook");
    std::vector<std::string> spelled;
    for (const Record& record : hooks) {
        std::string line = field(record.payload, "hook").as_string() + ":" +
                           field(record.payload, "state").as_string();
        if (const base::Json* component = record.payload.find("component"))
            line += "/" + component->as_string();
        spelled.push_back(line);
    }
    const std::vector<std::string> journal_expected = {
        "component_enter:Alive/ParentExitA",
        "component_enter:Alive/ParentExitB",
        "component_enter:Armed/ChildExitA",
        "component_enter:Armed/ChildExitB",
        "enter:Armed",
        "enter:Alive",
        "exit:Alive",
        "exit:Armed",
        "component_exit:Armed/ChildExitB",
        "component_exit:Armed/ChildExitA",
        "component_exit:Alive/ParentExitB",
        "component_exit:Alive/ParentExitA",
        "enter:Dead",
    };
    CHECK(spelled == journal_expected);
    for (std::size_t i = 0; i < hooks.size(); ++i) {
        const std::uint64_t owner = i < 6 ? instantiated[0].id : transitions[0].id;
        CHECK(hooks[i].cause_id == owner);
    }
    // Exit peers carry the transition target on component records too.
    CHECK(field(hooks[8].payload, "peer").as_string() == "Dead");
}

TEST_CASE("statechart.component_hooks: registration refusals — duplicates and unknown seats") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingComponentHooks components(log);
    const MachineId id = fix.spawn_machine(lifecycle_machine());
    Statechart& chart = fix.chart();

    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Alive"), Name("Twice"), components)
            .has_value());
    CHECK(code_of(chart.add_component_hooks(
              id, Name("life"), Name("Alive"), Name("Twice"), components)) ==
          "statechart.duplicate_component");
    // The SAME component name on a DIFFERENT state is its own seat.
    REQUIRE_FALSE(
        chart.add_component_hooks(id, Name("life"), Name("Armed"), Name("Twice"), components)
            .has_value());

    CHECK(code_of(chart.add_component_hooks(
              id + 7, Name("life"), Name("Alive"), Name("X"), components)) ==
          "statechart.unknown_machine");
    CHECK(code_of(
              chart.add_component_hooks(id, Name("nope"), Name("Alive"), Name("X"), components)) ==
          "statechart.unknown_region");
    CHECK(
        code_of(chart.add_component_hooks(id, Name("life"), Name("nope"), Name("X"), components)) ==
        "statechart.unknown_state");
    CHECK(code_of(chart.add_component_hooks(id, Name("life"), Name("Alive"), Name(), components)) ==
          "statechart.bad_component");
    (void)fix.finish();
}

TEST_CASE("statechart.split: deferred initial entry seats hooks first; the eager path and "
          "double-starts refuse correctly") {
    ChartFixture fix;
    Statechart& chart = fix.chart();

    // Eager instantiate (every pre-0B caller): entry already ran.
    const MachineId eager = fix.spawn_machine(lifecycle_machine());
    CHECK(code_of(chart.start_initial_entry(eager)) == "statechart.already_entered");

    // Deferred: topology exists (introspection works), but nothing entered
    // and no hook records exist until start_initial_entry.
    InstantiateOptions defer;
    defer.defer_initial_entry = true;
    InstantiateResult made = fix.chart().instantiate(lifecycle_machine(), fix.host, 0, defer);
    REQUIRE_FALSE(made.error.has_value());
    const MachineId id = made.machine;
    CHECK_FALSE(chart.machine_root(id).is_null());
    // The split defers the CHAINS, not the configuration: the initial chain
    // is already the active configuration at topology time (introspection
    // answers; delivery is gated until entry — the G4 case below), only the
    // A.2.1 enter chains — hooks, sheets — wait for start_initial_entry.
    CHECK(chart.in_state(id, Name("life"), Name("Alive")));
    CHECK(chart.active_state(id, Name("life")) == Name("Alive"));

    std::vector<std::string> log;
    RecordingHooks scripts(log);
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name("Alive"), scripts).has_value());
    REQUIRE_FALSE(chart.start_initial_entry(id).has_value());
    CHECK(chart.in_state(id, Name("life"), Name("Alive")));
    CHECK(chart.in_state(id, Name("life"), Name("Armed")));
    // The seated script's onEnter FIRED at initial entry — the split's
    // falsifiable core (eager instantiate can never show this line).
    CHECK(log == std::vector<std::string>{"enter:Alive"});

    CHECK(code_of(chart.start_initial_entry(id)) == "statechart.already_entered");
    CHECK(code_of(chart.start_initial_entry(id + 9)) == "statechart.unknown_machine");
    (void)fix.finish();
}

TEST_CASE("statechart.split: the deferred window is DEAF (council fix G4) — an event emitted "
          "between topology-create and start_initial_entry never transitions, and the entry "
          "chain still runs in full") {
    ChartFixture fix;
    Statechart& chart = fix.chart();
    std::vector<std::string> log;
    RecordingHooks scripts(log);

    InstantiateOptions defer;
    defer.defer_initial_entry = true;
    InstantiateResult made = fix.chart().instantiate(lifecycle_machine(), fix.host, 0, defer);
    REQUIRE_FALSE(made.error.has_value());
    const MachineId id = made.machine;
    for (const char* name : {"Alive", "Armed", "Dead"})
        REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name(name), scripts).has_value());

    // The deferred window: a TS component constructor / module top level may
    // legally emit during materialization (ComponentHost permits triggers
    // outside any hook). The subscribed-but-unentered machine must be deaf —
    // a transition here would stamp the region and permanently rob
    // run_initial_entry of its enter chain (the D2 invariant break).
    REQUIRE_FALSE(fix.trigger("golden.kill").error.has_value());
    CHECK(chart.stats().transitions == 0);
    CHECK(chart.stats().hook_calls == 0);
    CHECK(log.empty());
    CHECK(chart.active_state(id, Name("life")) == Name("Alive")); // configuration untouched

    // The entry chain is NOT skipped: both levels enter, hooks fire.
    REQUIRE_FALSE(chart.start_initial_entry(id).has_value());
    const std::vector<std::string> entry_expected = {"enter:Armed", "enter:Alive"};
    CHECK(log == entry_expected);
    CHECK(chart.in_state(id, Name("life"), Name("Armed")));

    // After entry the gate is open: the same event transitions normally.
    log.clear();
    REQUIRE_FALSE(fix.trigger("golden.kill").error.has_value());
    CHECK(chart.in_state(id, Name("life"), Name("Dead")));
    CHECK(chart.stats().transitions == 1);

    // Journal: the deaf window left NO chart trace — no transition, no
    // voided candidate; the one transition record is the post-entry kill.
    std::vector<Record> records = fix.finish();
    CHECK(of_kind(records, "statechart.transition").size() == 1);
    CHECK(of_kind(records, "statechart.voided").empty());
}

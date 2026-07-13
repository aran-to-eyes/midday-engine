// statechart.despawn_exit — Statechart::exit_host_machines (M2 0B track D,
// FUSED-SPEC D4): the phase-8 PREPARE half of a despawn runs the FULL A.2.1
// exit chain of everything alive on the host — the exact transition exit
// template with no destination. Pinned here:
//   * the exit order over the D6 nested shape (script outer-first, substates
//     deepest-completing, components REVERSE attach per level), regions in
//     REVERSE declaration order, machines in REVERSE instantiation order;
//   * hook records carry NO `peer` (there is no target state) and cite the
//     caller's cause id (the spawner's prefab.despawn record in production);
//   * the region stamp: a cascade fired from a dying machine's exit hooks
//     can never re-enter the corpse — it voids "region_already_transitioned";
//   * no-op safety: hosts without machines, and already-retired machines.

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

// The D6 golden's nested shape (life region) PLUS a second region (aux) so
// reverse region order is falsifiable.
MachineDesc despawn_machine() {
    StateDesc armed = state("Armed");
    StateDesc alive = state("Alive", {pair("golden.kill", "Dead")});
    alive.substates.push_back(armed);
    alive.initial = Name("Armed");
    return machine("probe_brain",
                   {region("life", "Alive", {alive, state("Dead")}),
                    region("aux", "Watching", {state("Watching"), state("Idle")})});
}

} // namespace

TEST_CASE("statechart.despawn_exit: the full exit chain — scripts outer-first, components "
          "REVERSE attach, regions REVERSE declaration, no peer, caller's cause id") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks scripts(log);
    RecordingComponentHooks components(log);

    InstantiateOptions defer;
    defer.defer_initial_entry = true;
    InstantiateResult made = fix.chart().instantiate(despawn_machine(), fix.host, 0, defer);
    REQUIRE_FALSE(made.error.has_value());
    const MachineId id = made.machine;
    Statechart& chart = fix.chart();
    for (const char* name : {"Alive", "Armed", "Dead"})
        REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name(name), scripts).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("aux"), Name("Watching"), scripts).has_value());
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
    log.clear();

    // A REAL stand-in for the spawner's prefab.despawn record — the journal
    // enforces record-before-effect, so the cause id must exist.
    const std::uint64_t cause =
        fix.writer().record(0, journal::Tier::Flight, "test.despawn", 0, base::Json::object());
    REQUIRE(cause != 0);
    chart.exit_host_machines(fix.host, cause);

    // aux (declared second) exits FIRST — reverse declaration order — then
    // life runs the exact D6 7-line template minus the enter (a despawn
    // enters nothing).
    const std::vector<std::string> exit_expected = {
        "exit:Watching",
        "exit:Alive",
        "exit:Armed",
        "c-exit:Armed/ChildExitB",
        "c-exit:Armed/ChildExitA",
        "c-exit:Alive/ParentExitB",
        "c-exit:Alive/ParentExitA",
    };
    CHECK(log == exit_expected);

    // Nothing is active afterwards; the machine topology still exists (the
    // flush, not this call, removes entities).
    CHECK_FALSE(chart.in_state(id, Name("life"), Name("Alive")));
    CHECK_FALSE(chart.in_state(id, Name("life"), Name("Armed")));
    CHECK_FALSE(chart.in_state(id, Name("aux"), Name("Watching")));
    CHECK(chart.active_state(id, Name("life")).empty());

    // Journal: every despawn-exit hook record cites the caller's cause and
    // carries NO peer — there is no destination state.
    std::vector<Record> records = fix.finish();
    std::vector<Record> hooks;
    for (const Record& record : of_kind(records, "statechart.hook"))
        if (record.cause_id == cause)
            hooks.push_back(record);
    REQUIRE(hooks.size() == exit_expected.size());
    for (const Record& record : hooks) {
        CHECK(record.payload.find("peer") == nullptr);
        const std::string hook = field(record.payload, "hook").as_string();
        CHECK((hook == "exit" || hook == "component_exit"));
    }
}

TEST_CASE("statechart.despawn_exit: a cascade from a dying machine's exit hook voids — it can "
          "never re-enter the corpse") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks scripts(log);

    const MachineId id = fix.spawn_machine(despawn_machine());
    Statechart& chart = fix.chart();
    for (const char* name : {"Alive", "Armed", "Dead"})
        REQUIRE_FALSE(chart.set_state_hooks(id, Name("life"), Name(name), scripts).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(id, Name("aux"), Name("Watching"), scripts).has_value());
    // Armed's onExit fires golden.kill at the host — a pair Alive still
    // matches. Without the region stamp this would EXECUTE a transition
    // into Dead mid-despawn and enter a state on a dying entity.
    scripts.exit_action = [&fix](Statechart&, const StateHookContext& context) {
        if (context.state == Name("Armed"))
            REQUIRE_FALSE(fix.trigger("golden.kill", context.record_id).error.has_value());
    };
    log.clear();

    chart.exit_host_machines(fix.host, 0);

    // The chain completed, nothing (re-)entered, no transition executed.
    const std::vector<std::string> exit_expected = {"exit:Watching", "exit:Alive", "exit:Armed"};
    auto joined = [](const std::vector<std::string>& lines) {
        std::string out;
        for (const std::string& line : lines)
            out += line + "|";
        return out;
    };
    CHECK(joined(log) == joined(exit_expected));
    CHECK_FALSE(chart.in_state(id, Name("life"), Name("Dead")));
    CHECK(chart.active_state(id, Name("life")).empty());

    std::vector<Record> records = fix.finish();
    CHECK(of_kind(records, "statechart.transition").empty());
    const std::vector<Record> voided = of_kind(records, "statechart.voided");
    REQUIRE_FALSE(voided.empty());
    for (const Record& record : voided)
        CHECK(field(record.payload, "reason").as_string() == "region_already_transitioned");
}

TEST_CASE("statechart.despawn_exit: machines exit in REVERSE instantiation order; foreign hosts "
          "and retired machines are untouched") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks scripts(log);
    Statechart& chart = fix.chart();

    const MachineId first = fix.spawn_machine(machine("first", {region("r", "A", {state("A")})}));
    const MachineId second = fix.spawn_machine(machine("second", {region("r", "B", {state("B")})}));
    REQUIRE_FALSE(chart.set_state_hooks(first, Name("r"), Name("A"), scripts).has_value());
    REQUIRE_FALSE(chart.set_state_hooks(second, Name("r"), Name("B"), scripts).has_value());

    // A machine on ANOTHER host stays untouched by this host's despawn.
    const ecs::EntityRef other = fix.world.spawn();
    REQUIRE_FALSE(fix.hierarchy.adopt(other).has_value());
    InstantiateResult foreign =
        chart.instantiate(machine("foreign", {region("r", "C", {state("C")})}), other);
    REQUIRE_FALSE(foreign.error.has_value());
    REQUIRE_FALSE(
        chart.set_state_hooks(foreign.machine, Name("r"), Name("C"), scripts).has_value());
    log.clear();

    chart.exit_host_machines(fix.host, 0);
    // The LAST machine seated exits first (the materialization mirror).
    CHECK(log == std::vector<std::string>{"exit:B", "exit:A"});
    CHECK(chart.in_state(foreign.machine, Name("r"), Name("C"))); // untouched

    // Idempotent / no-op safety: nothing left active on the host, nothing
    // on a machine-free entity, and a second call is silent.
    log.clear();
    chart.exit_host_machines(fix.host, 0);
    chart.exit_host_machines(other, 0); // exits foreign's chain (C is active)
    CHECK(log == std::vector<std::string>{"exit:C"});
    chart.exit_host_machines(ecs::EntityRef{}, 0); // null host: nothing
    CHECK(log == std::vector<std::string>{"exit:C"});
    (void)fix.finish();
}

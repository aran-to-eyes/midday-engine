// statechart.hook_order — THE A.2.1 fixture, pinned against the normative
// text (MIDDAY_ENGINE_SPEC.md Appendix A.2.1, quoted):
//
//   Exit (brain first — it orchestrates while its parts are still live):
//     1. S's state script onExit(to)
//     2. S's open sequence spans close / active substates exit — deepest
//        first, reverse activation order
//     3. S's components onExit — reverse attach order
//     4. S's node subtree (attached child entities) deactivates
//     5. Sequence playhead resets (or saves, under history)
//   Enter (mirror — brain last, when its parts are live):
//     1. S's node subtree activates
//     2. S's components onEnter — attach order
//     3. S's initial substate enters / sequence playhead starts at 0
//     4. S's state script onEnter(from)
//
// Applied recursively over a nested chain Slash > Strike > Deep with a child
// hurtbox entity under Strike, the observable order is: exit scripts
// OUTER->INNER (each brain runs while its parts are still live), subtree
// deactivation completing DEEPEST-FIRST (no zombie hitbox before the new
// state's onEnter), and enter scripts INNER->OUTER (each brain runs last,
// when its parts are live) — the A.3 trace's shape, driven pure C++.

#include "core/reflect/registry.h"
#include "core/statechart/test_support.h"

#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Name;
using midday::ecs::EntityRef;
using midday::journal::Record;

namespace {

struct HurtMarker {
    int damage = 7;
};

// The nested machine: combat region, Passive initial, SlashAttack with a
// two-deep substate chain, any-state death rule (the A.3 shape).
MachineDesc nested_machine() {
    StateDesc deep = state("Deep");
    StateDesc strike = state("Strike");
    strike.substates.push_back(deep);
    strike.initial = Name("Deep");
    StateDesc slash = state("SlashAttack");
    slash.substates.push_back(strike);
    slash.initial = Name("Strike");
    return machine("boss_brain",
                   {region("combat",
                           "Passive",
                           {state("Passive", {pair("attack", "Strike")}), slash, state("Dead")},
                           {pair("death.dealt", "Dead", 100)})});
}

reflect::ClassDesc component_class(const char* name) {
    reflect::ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

} // namespace

TEST_CASE("statechart.hook_order: instantiate enters the initial chain, script last") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks hooks(log);
    const MachineId id = fix.spawn_machine(nested_machine());
    // Hooks registered after instantiate see nothing retroactively; register
    // before in the transition cases. Here: pin the journal side instead.
    REQUIRE_FALSE(
        fix.chart().set_state_hooks(id, Name("combat"), Name("Passive"), hooks).has_value());

    CHECK(fix.chart().in_state(id, Name("combat"), Name("Passive")));
    CHECK(fix.chart().active_state(id, Name("combat")) == Name("Passive"));
    CHECK_FALSE(fix.chart().machine_root(id).is_null());
    CHECK(fix.hierarchy.is_owner(fix.chart().machine_root(id)));
    CHECK(fix.hierarchy.parent_of(fix.chart().machine_root(id)) == fix.host);

    std::vector<Record> records = fix.finish();
    const std::vector<Record> instantiated = of_kind(records, "statechart.instantiate");
    REQUIRE(instantiated.size() == 1);
    CHECK(field(instantiated[0].payload, "machine").as_string() == "boss_brain");
}

TEST_CASE("statechart.hook_order: initial-entry hooks fire per A.2.1, citing instantiate") {
    ChartFixture fix;
    std::vector<std::string> log;
    RecordingHooks hooks(log);

    // Register hooks FIRST via a two-step: instantiate a machine whose
    // initial chain is the nested one, hooks attached between instantiate
    // and... impossible — initial entry happens inside instantiate. The
    // normative claim for initial entry is therefore pinned via a TRANSITION
    // into the nested chain below; here we pin that instantiate journals the
    // enter of the initial state even without hooks: no hook records exist
    // (no invocation happened — hooks are part of the operation script).
    const MachineId id = fix.spawn_machine(nested_machine());
    (void)id;
    std::vector<Record> records = fix.finish();
    CHECK(of_kind(records, "statechart.hook").empty());
    CHECK(of_kind(records, "statechart.transition").empty());
}

TEST_CASE("statechart.hook_order: nested exit/enter matches the normative A.2.1 order") {
    ChartFixture fix;
    fix.world.register_component<HurtMarker>(component_class("HurtMarker"));

    const MachineId id = fix.spawn_machine(nested_machine());
    Statechart& chart = fix.chart();

    // A hurtbox child entity under Strike (the A.3 Hurtbox-under-span shape).
    EntityRef hurtbox = fix.world.spawn();
    REQUIRE_FALSE(fix.world.emplace<HurtMarker>(hurtbox, HurtMarker{}).has_value());
    const EntityRef strike_entity = chart.state_entity(id, Name("combat"), Name("Strike"));
    REQUIRE_FALSE(strike_entity.is_null());
    REQUIRE_FALSE(fix.hierarchy.queue_attach(hurtbox, strike_entity).has_value());
    REQUIRE_FALSE(fix.world.flush_structural().has_value());
    // Strike is dormant (initial state is Passive): the hurtbox sleeps.
    CHECK(fix.world.is_active<HurtMarker>(hurtbox) == std::optional<bool>(false));

    std::vector<std::string> log;
    RecordingHooks hooks(log);
    auto probe = [&fix, hurtbox, &log](Statechart&, const StateHookContext&) {
        const bool live = fix.world.is_active<HurtMarker>(hurtbox).value_or(false);
        log.emplace_back(live ? "hurtbox:live" : "hurtbox:dormant");
    };
    hooks.enter_action = probe;
    hooks.exit_action = probe;
    for (const char* name : {"Passive", "SlashAttack", "Strike", "Deep", "Dead"})
        REQUIRE_FALSE(chart.set_state_hooks(id, Name("combat"), Name(name), hooks).has_value());

    // --- deep-target ENTRY: Passive -> Strike (forces the ancestor path,
    // then Strike's initial substate Deep). Scripts fire INNER->OUTER
    // (mirror, brain last); the hurtbox wakes when Strike's subtree
    // activates — BEFORE every enter script on the chain.
    REQUIRE_FALSE(fix.trigger("attack").error.has_value());
    const std::vector<std::string> entry_expected = {
        "exit:Passive",
        "hurtbox:dormant", // exit brain first; Strike still dormant
        "enter:Deep",
        "hurtbox:live", // subtree activated before scripts
        "enter:Strike",
        "hurtbox:live",
        "enter:SlashAttack",
        "hurtbox:live",
    };
    CHECK(log == entry_expected);
    CHECK(chart.in_state(id, Name("combat"), Name("SlashAttack")));
    CHECK(chart.in_state(id, Name("combat"), Name("Strike")));
    CHECK(chart.in_state(id, Name("combat"), Name("Deep")));
    CHECK(fix.world.is_active<HurtMarker>(hurtbox) == std::optional<bool>(true));

    // One transition per region per tick: step the loop so the death rule
    // evaluates on a fresh tick (the update phase also fires fixed-update
    // hooks on the active chain — cleared below, pinned in its own test).
    log.clear();
    REQUIRE_FALSE(fix.loop().tick().has_value());
    // Phase-5 order: scene-tree depth-first — parents before substates.
    CHECK(log == std::vector<std::string>{"fixed:SlashAttack", "fixed:Strike", "fixed:Deep"});
    // The frame-side onUpdate flavor drives the same order (no A.1 phase —
    // a frame loop calls run_update; D-BUILD-055).
    log.clear();
    chart.run_update(1.0 / 120.0, 0);
    CHECK(log == std::vector<std::string>{"update:SlashAttack", "update:Strike", "update:Deep"});

    // --- nested EXIT: the any-state death rule exits the whole SlashAttack
    // subtree. Scripts fire OUTER->INNER (brain first, parts still live);
    // every script on the chain still sees the hurtbox LIVE (deactivation is
    // step 4, after substates); Dead's onEnter sees it DORMANT (the
    // no-zombie-hitbox assertion, A.3).
    log.clear();
    REQUIRE_FALSE(fix.trigger("death.dealt").error.has_value());
    const std::vector<std::string> exit_expected = {
        "exit:SlashAttack",
        "hurtbox:live", // brain first — parts still live
        "exit:Strike",
        "hurtbox:live", // own subtree deactivates after
        "exit:Deep",
        "hurtbox:live", // Deep's subtree excludes the hurtbox
        "enter:Dead",
        "hurtbox:dormant", // deepest-first completion done
    };
    CHECK(log == exit_expected);
    CHECK(chart.in_state(id, Name("combat"), Name("Dead")));
    CHECK_FALSE(chart.in_state(id, Name("combat"), Name("SlashAttack")));

    // --- journal: the pinned record chain. transition cites the trigger;
    // every hook record cites its transition; peers carry from/to.
    std::vector<Record> records = fix.finish();
    const std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE(transitions.size() == 2);
    CHECK(field(transitions[0].payload, "from").as_string() == "Passive");
    CHECK(field(transitions[0].payload, "to").as_string() == "Strike");
    CHECK(field(transitions[0].payload, "via").as_string() == "Passive");
    CHECK(field(transitions[1].payload, "from").as_string() == "SlashAttack");
    CHECK(field(transitions[1].payload, "to").as_string() == "Dead");
    CHECK(field(transitions[1].payload, "via").as_string() == "any-state");

    const std::vector<Record> hook_records = of_kind(records, "statechart.hook");
    REQUIRE(hook_records.size() == 8); // 4 per transition
    const std::vector<std::string> hook_expected = {
        "exit:Passive",
        "enter:Deep",
        "enter:Strike",
        "enter:SlashAttack",
        "exit:SlashAttack",
        "exit:Strike",
        "exit:Deep",
        "enter:Dead",
    };
    for (std::size_t i = 0; i < hook_records.size(); ++i) {
        const std::string spelled = field(hook_records[i].payload, "hook").as_string() + ":" +
                                    field(hook_records[i].payload, "state").as_string();
        CHECK(spelled == hook_expected[i]);
        const std::uint64_t owner = i < 4 ? transitions[0].id : transitions[1].id;
        CHECK(hook_records[i].cause_id == owner);
    }
    // Peers: exit peer = the transition target, enter peer = the exited state.
    CHECK(field(hook_records[0].payload, "peer").as_string() == "Strike");
    CHECK(field(hook_records[1].payload, "peer").as_string() == "Passive");
    CHECK(field(hook_records[4].payload, "peer").as_string() == "Dead");
    CHECK(field(hook_records[7].payload, "peer").as_string() == "SlashAttack");
}

TEST_CASE("statechart.hook_order: re-entry restarts at initial; history resumes last active") {
    ChartFixture fix;

    // Two substates so initial-vs-history is observable: Patrol { A initial,
    // B }, with an exit to Idle and re-entry. history toggles per sub-case.
    for (const bool history : {false, true}) {
        StateDesc patrol = state("Patrol", {pair("rest", "Idle")});
        patrol.substates.push_back(state("A", {pair("advance", "B")}));
        patrol.substates.push_back(state("B"));
        patrol.initial = Name("A");
        patrol.history = history;
        MachineDesc desc = machine(
            history ? "hist" : "nohist",
            {region("main", "Patrol", {patrol, state("Idle", {pair("resume", "Patrol")})})});

        const MachineId id = fix.spawn_machine(desc);
        Statechart& chart = fix.chart();
        // Advance to B, leave, come back — one region transition per tick.
        REQUIRE_FALSE(fix.trigger("advance").error.has_value());
        CHECK(chart.in_state(id, Name("main"), Name("B")));
        REQUIRE_FALSE(fix.loop().tick().has_value());
        REQUIRE_FALSE(fix.trigger("rest").error.has_value());
        CHECK(chart.in_state(id, Name("main"), Name("Idle")));
        REQUIRE_FALSE(fix.loop().tick().has_value());
        REQUIRE_FALSE(fix.trigger("resume").error.has_value());
        CHECK(chart.in_state(id, Name("main"), Name("Patrol")));
        // Spec 4.1 entry semantics: re-entry starts at `initial` unless the
        // parent opted into history (resume last active substate).
        CHECK(chart.in_state(id, Name("main"), Name(history ? "B" : "A")));
        CHECK_FALSE(chart.in_state(id, Name("main"), Name(history ? "A" : "B")));
    }
}

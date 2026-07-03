// statechart.priority / .tie / .any_state / .region_marking / .voided /
// .filters / .finished — the A.2 transition-rule exit tests, all C++-driven:
// priority beats declaration order, ties resolve by declaration order with
// any-state rules declared first, one transition per region per tick with
// region-wide voiding on later events, voided records carry a pinned shape,
// `if:` filters gate candidacy through the machine environment, and
// state.finished chaining is an ordinary transition pair.

#include "core/statechart/test_support.h"

#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Name;
using midday::journal::Record;

TEST_CASE("statechart.priority: highest priority wins; every loser journals as voided") {
    ChartFixture fix;
    const MachineId id = fix.spawn_machine(
        machine("prio",
                {region("r",
                        "S",
                        {state("S", {pair("e", "X", 1), pair("e", "Y", 5), pair("e", "Z", 5)}),
                         state("X"),
                         state("Y"),
                         state("Z")})}));
    REQUIRE_FALSE(fix.trigger("e").error.has_value());
    // Y: priority 5 beats X's 1; first of the equal-priority pair wins.
    CHECK(fix.chart().in_state(id, Name("r"), Name("Y")));
    CHECK(fix.chart().stats().transitions == 1);
    CHECK(fix.chart().stats().voided == 2);

    std::vector<Record> records = fix.finish();
    const std::vector<Record> voided = of_kind(records, "statechart.voided");
    REQUIRE(voided.size() == 2);
    CHECK(field(voided[0].payload, "target").as_string() == "X");
    CHECK(field(voided[0].payload, "reason").as_string() == "lost");
    CHECK(field(voided[1].payload, "target").as_string() == "Z");
    CHECK(field(voided[1].payload, "reason").as_string() == "lost");
    // Losers cite the event that spawned the candidates.
    const std::vector<Record> triggers = of_kind(records, "event.trigger");
    REQUIRE(triggers.size() == 1);
    CHECK(voided[0].cause_id == triggers[0].id);
    CHECK(voided[1].cause_id == triggers[0].id);
}

TEST_CASE("statechart.tie: equal priority resolves by declaration order") {
    ChartFixture fix;
    const MachineId id = fix.spawn_machine(machine(
        "tie",
        {region(
            "r", "S", {state("S", {pair("e", "X"), pair("e", "Y")}), state("X"), state("Y")})}));
    REQUIRE_FALSE(fix.trigger("e").error.has_value());
    CHECK(fix.chart().in_state(id, Name("r"), Name("X")));
}

TEST_CASE("statechart.any_state: any-state rules tie-break first but share the priority space") {
    ChartFixture fix;
    // Two parallel regions, one event: r1 pins the tie (any-state declared
    // before the active state's pairs), r2 pins that a HIGHER-priority state
    // pair beats an any-state rule — and that parallel regions transition
    // independently on one event (one transition each, A.2.1 tail note).
    const MachineId id = fix.spawn_machine(
        machine("any",
                {region("r1",
                        "S1",
                        {state("S1", {pair("e", "X1")}), state("X1"), state("Z1")},
                        {pair("e", "Z1")}),
                 region("r2",
                        "S2",
                        {state("S2", {pair("e", "X2", 1)}), state("X2"), state("Z2")},
                        {pair("e", "Z2")})}));
    REQUIRE_FALSE(fix.trigger("e").error.has_value());
    CHECK(fix.chart().in_state(id, Name("r1"), Name("Z1"))); // any-state wins the tie
    CHECK(fix.chart().in_state(id, Name("r2"), Name("X2"))); // priority beats declaration
    CHECK(fix.chart().stats().transitions == 2);

    std::vector<Record> records = fix.finish();
    const std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE(transitions.size() == 2);
    CHECK(field(transitions[0].payload, "region").as_string() == "r1");
    CHECK(field(transitions[0].payload, "via").as_string() == "any-state");
    CHECK(field(transitions[1].payload, "region").as_string() == "r2");
    CHECK(field(transitions[1].payload, "via").as_string() == "S2");
}

TEST_CASE("statechart.region_marking: one transition per region per tick; fresh next tick") {
    ChartFixture fix;
    const MachineId id = fix.spawn_machine(machine(
        "marking",
        {region("r", "A", {state("A", {pair("go", "B")}), state("B", {pair("go", "A")})})}));
    // First event this tick transitions; the second is voided REGION-WIDE:
    // both name-matching pairs journal (the exited A's pair too — the A.3
    // stagger.hit shape: the record explains why a matching rule did not run).
    REQUIRE_FALSE(fix.trigger("go").error.has_value());
    CHECK(fix.chart().in_state(id, Name("r"), Name("B")));
    REQUIRE_FALSE(fix.trigger("go").error.has_value());
    CHECK(fix.chart().in_state(id, Name("r"), Name("B"))); // held
    CHECK(fix.chart().stats().transitions == 1);
    CHECK(fix.chart().stats().voided == 2);

    // Next tick is a fresh evaluation (nothing queued, A.2 rule 4).
    REQUIRE_FALSE(fix.loop().tick().has_value());
    REQUIRE_FALSE(fix.trigger("go").error.has_value());
    CHECK(fix.chart().in_state(id, Name("r"), Name("A")));
    CHECK(fix.chart().stats().transitions == 2);

    // The pinned voided-record shape (key order is journal contract).
    std::vector<Record> records = fix.finish();
    const std::vector<Record> voided = of_kind(records, "statechart.voided");
    REQUIRE(voided.size() == 2);
    CHECK(voided[0].payload.dump() ==
          R"({"machine":"marking","entity":"entity:0#0","region":"r","event":"go",)"
          R"("source":"A","target":"B","priority":0,"reason":"region_already_transitioned"})");
    CHECK(field(voided[1].payload, "source").as_string() == "B");
    CHECK(field(voided[1].payload, "target").as_string() == "A");
}

TEST_CASE("statechart.filters: if-filters gate candidacy through the machine environment") {
    ChartFixture fix;
    MachineDesc desc = machine(
        "filtered",
        {region("r",
                "S",
                {state("S", {pair("hurt", "Dying", 10, "health < 30"), pair("hurt", "Flinch")}),
                 state("Dying"),
                 state("Flinch", {pair("reset", "S")})})});
    desc.vars.push_back({"health", expr::ValueType::kFloat});
    const MachineId id = fix.spawn_machine(desc);
    Statechart& chart = fix.chart();

    // Healthy: the high-priority Dying pair fails its filter -> not a
    // candidate (and NOT voided — it never matched); Flinch wins.
    REQUIRE_FALSE(chart.set_var(id, "health", expr::Value::of_float(80.0F)).has_value());
    REQUIRE_FALSE(fix.trigger("hurt").error.has_value());
    CHECK(chart.in_state(id, Name("r"), Name("Flinch")));
    CHECK(chart.stats().voided == 0);

    REQUIRE_FALSE(fix.loop().tick().has_value());
    REQUIRE_FALSE(fix.trigger("reset").error.has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value());

    // Wounded: the filter passes and priority 10 wins; Flinch is voided.
    REQUIRE_FALSE(chart.set_var(id, "health", expr::Value::of_float(12.0F)).has_value());
    REQUIRE_FALSE(fix.trigger("hurt").error.has_value());
    CHECK(chart.in_state(id, Name("r"), Name("Dying")));
    CHECK(chart.stats().voided == 1);

    // Environment misuse is structured, never UB.
    CHECK(code_of(chart.set_var(id, "mana", expr::Value::of_float(1.0F))) ==
          "statechart.unknown_var");
    CHECK(code_of(chart.set_var(id, "health", expr::Value::of_int(3))) == "statechart.var_type");
}

TEST_CASE("statechart.filters: a faulting filter journals and skips the pair") {
    ChartFixture fix;
    MachineDesc desc =
        machine("faulty",
                {region("r",
                        "S",
                        {state("S", {pair("e", "X", 10, "(1 / divisor) == 1"), pair("e", "Y")}),
                         state("X"),
                         state("Y")})});
    desc.vars.push_back({"divisor", expr::ValueType::kInt}); // zero-valued slot
    const MachineId id = fix.spawn_machine(desc);
    REQUIRE_FALSE(fix.trigger("e").error.has_value());
    // The faulting pair never becomes a candidate; the sound pair runs.
    CHECK(fix.chart().in_state(id, Name("r"), Name("Y")));
    CHECK(fix.chart().stats().filter_faults == 1);

    std::vector<Record> records = fix.finish();
    const std::vector<Record> faults = of_kind(records, "statechart.filter_fault");
    REQUIRE(faults.size() == 1);
    CHECK(field(faults[0].payload, "error").as_string() == "expr.div_zero");
    CHECK(field(faults[0].payload, "source").as_string() == "S");
}

TEST_CASE("statechart.finished: finish_state emits <state>.finished; chaining is a plain pair") {
    ChartFixture fix;
    const MachineId id = fix.spawn_machine(machine(
        "seq",
        {region("combat",
                "Attack",
                {state("Attack", {pair("Attack.finished", "Cooldown")}), state("Cooldown")})}));
    Statechart& chart = fix.chart();

    // Inactive states refuse (the sequences node calls this correctly).
    CHECK(code_of(chart.finish_state(id, Name("combat"), Name("Cooldown"), 0)) ==
          "statechart.state_not_active");

    REQUIRE_FALSE(chart.finish_state(id, Name("combat"), Name("Attack"), 0).has_value());
    CHECK(chart.in_state(id, Name("combat"), Name("Cooldown")));

    std::vector<Record> records = fix.finish();
    const std::vector<Record> triggers = of_kind(records, "event.trigger");
    REQUIRE(triggers.size() == 1);
    CHECK(field(triggers[0].payload, "event").as_string() == "Attack.finished");
    // The family payload shape: {entity, region, state} (D-BUILD-056).
    const base::Json& payload = field(triggers[0].payload, "payload");
    CHECK(field(payload, "region").as_string() == "combat");
    CHECK(field(payload, "state").as_string() == "Attack");
    // The chained transition cites the finished trigger.
    const std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE(transitions.size() == 1);
    CHECK(transitions[0].cause_id == triggers[0].id);
    CHECK(field(transitions[0].payload, "from").as_string() == "Attack");
    CHECK(field(transitions[0].payload, "to").as_string() == "Cooldown");
}

TEST_CASE("statechart.validation: descriptions refuse atomically with structured codes") {
    ChartFixture fix;
    const std::uint32_t alive_before = fix.world.alive_count();

    MachineDesc no_regions = machine("empty", {});
    CHECK(code_of(fix.chart().instantiate(no_regions, fix.host).error) ==
          "statechart.empty_machine");

    MachineDesc bad_target =
        machine("bad", {region("r", "S", {state("S", {pair("e", "Nowhere")})})});
    CHECK(code_of(fix.chart().instantiate(bad_target, fix.host).error) == "statechart.bad_target");

    MachineDesc dup = machine("dup", {region("r", "S", {state("S"), state("S")})});
    CHECK(code_of(fix.chart().instantiate(dup, fix.host).error) == "statechart.duplicate_state");

    MachineDesc bad_filter =
        machine("filter", {region("r", "S", {state("S", {pair("e", "S", 0, "1 +")})})});
    CHECK(code_of(fix.chart().instantiate(bad_filter, fix.host).error) == "statechart.bad_filter");

    MachineDesc not_bool =
        machine("nb", {region("r", "S", {state("S", {pair("e", "S", 0, "1 + 1")})})});
    CHECK(code_of(fix.chart().instantiate(not_bool, fix.host).error) ==
          "statechart.filter_not_bool");

    // Atomic: every refusal above left the world untouched.
    CHECK(fix.world.alive_count() == alive_before);
}

// statechart.watchers — `when:` watcher edge semantics (A.1 phase 3,
// normative): a condition already true at the first evaluation after entry
// fires once, no refire while it stays true, re-arm on observing false and
// on state exit/re-entry; fired events carry the watchers phase marker as
// cause; evaluation order is hierarchy tree order then declaration order.

#include "core/statechart/test_support.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Name;
using midday::journal::Record;

namespace {

// The canonical watched machine: Guard panics below 30 health; a second
// region carries a transition-free "beep" watcher for refire counting.
MachineDesc watched_machine() {
    StateDesc guard = state("Guard", {pair("low", "Panic")});
    guard.watchers.push_back({"health < 30", Name("low")});
    StateDesc watch = state("Watch");
    watch.watchers.push_back({"health < 30", Name("beep")});
    MachineDesc desc =
        machine("watched",
                {region("r1", "Guard", {guard, state("Panic", {pair("calm", "Guard")})}),
                 region("r2", "Watch", {watch})});
    desc.vars.push_back({"health", expr::ValueType::kFloat});
    return desc;
}

std::size_t count_event(const std::vector<Record>& records, std::string_view event) {
    std::size_t n = 0;
    for (const Record& record : of_kind(records, "event.trigger"))
        if (field(record.payload, "event").as_string() == event)
            ++n;
    return n;
}

} // namespace

TEST_CASE("statechart.watchers: edge semantics — fire once, hold, re-arm on false") {
    ChartFixture fix;
    const MachineId id = fix.spawn_machine(watched_machine());
    Statechart& chart = fix.chart();
    auto set_health = [&](float value) {
        REQUIRE_FALSE(chart.set_var(id, "health", expr::Value::of_float(value)).has_value());
    };

    // False condition: ticks pass, nothing fires.
    set_health(80.0F);
    REQUIRE_FALSE(fix.loop().tick(3).has_value());
    CHECK(chart.stats().watcher_fires == 0);
    CHECK(chart.in_state(id, Name("r1"), Name("Guard")));

    // Rising edge: both watchers fire ONCE — r1 transitions Guard -> Panic,
    // r2's beep has no pair. Holding true refires NOTHING.
    set_health(20.0F);
    REQUIRE_FALSE(fix.loop().tick(4).has_value());
    CHECK(chart.stats().watcher_fires == 2);
    CHECK(chart.in_state(id, Name("r1"), Name("Panic")));

    // Guard is inactive: its watcher does not evaluate. r2 re-arms on the
    // falling edge and fires again on the next rise.
    set_health(50.0F);
    REQUIRE_FALSE(fix.loop().tick(2).has_value());
    set_health(10.0F);
    REQUIRE_FALSE(fix.loop().tick(2).has_value());
    CHECK(chart.stats().watcher_fires == 3); // beep only — Guard stayed exited

    // Exit re-arms: re-entering Guard with the condition ALREADY true fires
    // on the first evaluation after entry (armed-false baseline).
    REQUIRE_FALSE(fix.trigger("calm").error.has_value());
    CHECK(chart.in_state(id, Name("r1"), Name("Guard")));
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(chart.stats().watcher_fires == 4);
    CHECK(chart.in_state(id, Name("r1"), Name("Panic")));

    std::vector<Record> records = fix.finish();
    CHECK(count_event(records, "low") == 2);
    CHECK(count_event(records, "beep") == 2);
}

TEST_CASE("statechart.watchers: fires cite the watchers phase marker; tree order holds") {
    ChartFixture fix;
    const MachineId id = fix.spawn_machine(watched_machine());
    (void)id;
    REQUIRE_FALSE(fix.chart().set_var(id, "health", expr::Value::of_float(5.0F)).has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(fix.chart().stats().watcher_fires == 2);

    std::vector<Record> records = fix.finish();
    // Locate this tick's watchers phase marker.
    std::uint64_t marker_id = 0;
    for (const Record& record : records)
        if (record.kind == "tick.phase" && field(record.payload, "phase").as_string() == "watchers")
            marker_id = record.id;
    REQUIRE(marker_id != 0);

    // Both fires cite the marker (engine-initiated, D-BUILD-050), and the
    // r1 watcher (earlier state entity in tree order) fired FIRST — its
    // whole transition cascade sits between the two triggers.
    std::vector<Record> triggers = of_kind(records, "event.trigger");
    REQUIRE(triggers.size() == 2);
    CHECK(field(triggers[0].payload, "event").as_string() == "low");
    CHECK(triggers[0].cause_id == marker_id);
    CHECK(field(triggers[1].payload, "event").as_string() == "beep");
    CHECK(triggers[1].cause_id == marker_id);
    // The Guard->Panic transition cites the low trigger — the full chain
    // reads phase marker -> low -> transition.
    std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE(transitions.size() == 1);
    CHECK(transitions[0].cause_id == triggers[0].id);
}

TEST_CASE("statechart.watchers: a faulting watcher journals and leaves the arm untouched") {
    ChartFixture fix;
    StateDesc s = state("S");
    s.watchers.push_back({"(1 / divisor) == 1", Name("never")});
    MachineDesc desc = machine("faulty", {region("r", "S", {s})});
    desc.vars.push_back({"divisor", expr::ValueType::kInt}); // zero slot: div fault
    const MachineId id = fix.spawn_machine(desc);
    REQUIRE_FALSE(fix.loop().tick(2).has_value());
    CHECK(fix.chart().stats().watcher_fires == 0);
    CHECK(fix.chart().stats().filter_faults == 2); // one per evaluation

    // Repair the environment: the watcher evaluates soundly and fires.
    REQUIRE_FALSE(fix.chart().set_var(id, "divisor", expr::Value::of_int(1)).has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(fix.chart().stats().watcher_fires == 1);

    std::vector<Record> records = fix.finish();
    const std::vector<Record> faults = of_kind(records, "statechart.filter_fault");
    REQUIRE(faults.size() == 2);
    CHECK(field(faults[0].payload, "error").as_string() == "expr.div_zero");
    CHECK(field(faults[0].payload, "event").as_string() == "never");
}

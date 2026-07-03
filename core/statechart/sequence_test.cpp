// core/statechart/sequence_test.cpp — sequence.* selftests (m0-sequences):
// the pinned rounding-rule KATs, THE canonical tick-lock fixture (1.2 s at
// 60 Hz: t=0.30 trigger at EXACTLY tick 18, span [0.40, 0.80] open 24 /
// close 48, finished 72 — all journal-walked), loop[n] wrap semantics, hold,
// and atomic instantiate validation. Interruption/history/determinism live
// in sequence_interrupt_test.cpp.

#include "core/statechart/test_support.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Json;
using midday::base::Name;
using midday::journal::Record;

namespace {

TriggerTrackDesc trigger_at(double time, std::string_view event) {
    TriggerTrackDesc track;
    track.time = time;
    track.event = Name(event);
    return track;
}

SpanTrackDesc span_over(std::string_view name, double begin, double end) {
    SpanTrackDesc track;
    track.name = Name(name);
    track.begin = begin;
    track.end = end;
    return track;
}

// THE canonical sheet: 1.2 s, swoosh at 0.30, hitbox span [0.40, 0.80].
SequenceDesc slash_sheet() {
    SequenceDesc sheet;
    sheet.duration = 1.2;
    sheet.triggers.push_back(trigger_at(0.30, "attack.swoosh"));
    sheet.spans.push_back(span_over("hitbox", 0.40, 0.80));
    return sheet;
}

MachineDesc boss_machine(SequenceDesc sheet) {
    StateDesc slash = state("SlashAttack", {pair("SlashAttack.finished", "Passive")});
    slash.sequence = std::move(sheet);
    return machine("boss", {region("combat", "SlashAttack", {slash, state("Passive")})});
}

// event.trigger records for one event name, in journal order.
std::vector<Record> trigger_records(const std::vector<Record>& records, std::string_view event) {
    std::vector<Record> out;
    for (const Record& record : of_kind(records, "event.trigger"))
        if (field(record.payload, "event").as_string() == event)
            out.push_back(record);
    return out;
}

// The tick.phase marker id for `phase` at `tick` (0 = not found).
std::uint64_t
phase_marker_id(const std::vector<Record>& records, std::uint64_t tick, std::string_view phase) {
    for (const Record& record : records)
        if (record.kind == "tick.phase" && record.tick == tick &&
            field(record.payload, "phase").as_string() == phase)
            return record.id;
    return 0;
}

} // namespace

TEST_CASE("sequence.tick_lock: tick = llround(seconds * rate) — the pinned rounding-rule KATs") {
    // One IEEE double multiply, then nearest-with-ties-away-from-zero.
    CHECK(time_to_tick(0.30, 60) == 18);               // the canonical trigger
    CHECK(time_to_tick(0.40, 60) == 24);               // the canonical span opening
    CHECK(time_to_tick(0.80, 60) == 48);               // the canonical span closing
    CHECK(time_to_tick(1.2, 60) == 72);                // the canonical duration
    CHECK(time_to_tick(0.025, 60) == 2);               // product exactly 1.5: tie away from zero
    CHECK(time_to_tick(0.0125, 60) == 1);              // 0.75 -> nearest
    CHECK(time_to_tick(0.30, 30) == 9);                // rate-dependent, same rule
    CHECK(time_to_tick(0.0, 60) == 0);                 // origin
    CHECK(time_to_tick(1.0 / 3.0, 60) == 20);          // non-representable seconds
    CHECK(time_to_tick(53.0 + 1.0 / 3.0, 60) == 3200); // the A.3 trace tick
}

TEST_CASE("sequence.canonical: 1.2 s at 60 Hz — trigger tick 18, span open 24 / close 48, "
          "finished 72, all journal-walked") {
    ChartFixture fix;
    const MachineId boss = fix.spawn_machine(boss_machine(slash_sheet()));
    REQUIRE_FALSE(fix.loop().run_to_tick(80).has_value());

    // finished chained into Passive through an ordinary Transition pair.
    CHECK(fix.chart().active_state(boss, Name("combat")) == Name("Passive"));
    const StatechartStats& stats = fix.chart().stats();
    CHECK(stats.sequence_triggers == 1);
    CHECK(stats.span_opens == 1);
    CHECK(stats.span_closes == 1);
    CHECK(stats.sequence_finishes == 1);
    CHECK(stats.sequence_loops == 0);

    const std::vector<Record> records = fix.finish();

    // Trigger track: EXACTLY tick 18, caused by tick 18's sequences marker.
    const std::vector<Record> swoosh = trigger_records(records, "attack.swoosh");
    REQUIRE(swoosh.size() == 1);
    CHECK(swoosh[0].tick == 18);
    REQUIRE(phase_marker_id(records, 18, "sequences") != 0);
    CHECK(swoosh[0].cause_id == phase_marker_id(records, 18, "sequences"));

    // Span boundaries: open 24 / close 48; the bus events cite the records.
    const std::vector<Record> opens = of_kind(records, "sequence.span_open");
    REQUIRE(opens.size() == 1);
    CHECK(opens[0].tick == 24);
    CHECK(field(opens[0].payload, "span").as_string() == "hitbox");
    CHECK(field(opens[0].payload, "state").as_string() == "SlashAttack");
    CHECK(field(opens[0].payload, "playhead").as_int() == 24);
    CHECK(field(opens[0].payload, "via").as_string() == "playhead");
    CHECK(opens[0].cause_id == phase_marker_id(records, 24, "sequences"));

    const std::vector<Record> closes = of_kind(records, "sequence.span_close");
    REQUIRE(closes.size() == 1);
    CHECK(closes[0].tick == 48);
    CHECK(field(closes[0].payload, "playhead").as_int() == 48);
    CHECK(field(closes[0].payload, "via").as_string() == "playhead");

    const std::vector<Record> opened = trigger_records(records, "hitbox.opened");
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].tick == 24);
    CHECK(opened[0].cause_id == opens[0].id);
    const std::vector<Record> closed = trigger_records(records, "hitbox.closed");
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].tick == 48);
    CHECK(closed[0].cause_id == closes[0].id);

    // The sheet end: "<state>.finished" at tick 72 (the statechart emission
    // path), and the transition it chains rides the same tick.
    const std::vector<Record> finished = trigger_records(records, "SlashAttack.finished");
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].tick == 72);
    CHECK(finished[0].cause_id == phase_marker_id(records, 72, "sequences"));
    const std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE(transitions.size() == 1);
    CHECK(transitions[0].tick == 72);
    CHECK(field(transitions[0].payload, "from").as_string() == "SlashAttack");
    CHECK(field(transitions[0].payload, "to").as_string() == "Passive");
    CHECK(transitions[0].cause_id == finished[0].id);
}

TEST_CASE("sequence.loop: loop[2] — local tick 0 fires at entry AND at the wrap tick; "
          "finished after the final pass") {
    // 0.2 s sheet (12 ticks), triggers at 0.0 and 0.1: local ticks 0 and 6.
    SequenceDesc sheet;
    sheet.duration = 0.2;
    sheet.triggers.push_back(trigger_at(0.0, "beat.zero"));
    sheet.triggers.push_back(trigger_at(0.1, "beat.half"));
    sheet.end = SequenceEnd::kLoop;
    sheet.loop_count = 2;
    ChartFixture fix;
    fix.spawn_machine(boss_machine(std::move(sheet))); // enters at bus tick 0
    REQUIRE_FALSE(fix.loop().run_to_tick(30).has_value());

    const StatechartStats& stats = fix.chart().stats();
    CHECK(stats.sequence_loops == 1);
    CHECK(stats.sequence_finishes == 1);
    const std::vector<Record> records = fix.finish();

    // Pass 1: entry fires local 0 inside the enter chain (bus tick 0, cause
    // = the instantiate record); the wrap tick fires pass 1's end AND pass
    // 2's local 0 in the same phase.
    std::vector<std::uint64_t> zero_ticks;
    for (const Record& record : trigger_records(records, "beat.zero"))
        zero_ticks.push_back(record.tick);
    CHECK(zero_ticks == std::vector<std::uint64_t>{0, 12});
    std::vector<std::uint64_t> half_ticks;
    for (const Record& record : trigger_records(records, "beat.half"))
        half_ticks.push_back(record.tick);
    CHECK(half_ticks == std::vector<std::uint64_t>{6, 18});

    const std::vector<Record> loops = of_kind(records, "sequence.loop");
    REQUIRE(loops.size() == 1);
    CHECK(loops[0].tick == 12);
    CHECK(field(loops[0].payload, "pass").as_int() == 1);
    const std::vector<Record> finished = trigger_records(records, "SlashAttack.finished");
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].tick == 24); // 2 passes x 12 ticks
}

TEST_CASE("sequence.hold: the playhead pins at the end — items fire once, no finished, "
          "the state stays active") {
    SequenceDesc sheet;
    sheet.duration = 0.5;                                 // 30 ticks
    sheet.triggers.push_back(trigger_at(0.5, "wind.up")); // exactly at the end
    sheet.spans.push_back(span_over("charge", 0.0, 0.5)); // covers the sheet
    sheet.end = SequenceEnd::kHold;
    ChartFixture fix;
    const MachineId boss = fix.spawn_machine(boss_machine(std::move(sheet)));
    REQUIRE_FALSE(fix.loop().run_to_tick(60).has_value());

    CHECK(fix.chart().active_state(boss, Name("combat")) == Name("SlashAttack"));
    const StatechartStats& stats = fix.chart().stats();
    CHECK(stats.sequence_finishes == 0);
    CHECK(stats.transitions == 0);
    const std::vector<Record> records = fix.finish();

    // The covering span opened at entry (local 0 = bus tick 0) and closed
    // at the end tick; the end-tick trigger fired exactly once.
    const std::vector<Record> opens = of_kind(records, "sequence.span_open");
    REQUIRE(opens.size() == 1);
    CHECK(opens[0].tick == 0);
    const std::vector<Record> closes = of_kind(records, "sequence.span_close");
    REQUIRE(closes.size() == 1);
    CHECK(closes[0].tick == 30);
    const std::vector<Record> wind = trigger_records(records, "wind.up");
    REQUIRE(wind.size() == 1);
    CHECK(wind[0].tick == 30);
    CHECK(trigger_records(records, "SlashAttack.finished").empty());
}

TEST_CASE("sequence.validation: malformed sheets refuse atomically with structured codes") {
    ChartFixture fix;
    auto refuse = [&fix](SequenceDesc sheet, std::string_view code) {
        InstantiateResult result =
            fix.chart().instantiate(boss_machine(std::move(sheet)), fix.host);
        REQUIRE(result.error.has_value());
        CHECK(result.error->code == code);
        CHECK(result.machine == kInvalidMachine);
    };
    SequenceDesc good = slash_sheet();

    SequenceDesc bad = good;
    bad.duration = 0.0;
    refuse(std::move(bad), "statechart.bad_sequence"); // zero duration

    bad = good;
    bad.duration = 0.004; // rounds to 0 ticks at 60 Hz
    refuse(std::move(bad), "statechart.bad_sequence");

    bad = good;
    bad.triggers[0].time = 1.3; // beyond the sheet
    refuse(std::move(bad), "statechart.bad_sequence");

    bad = good;
    bad.triggers[0].time = -0.1;
    refuse(std::move(bad), "statechart.bad_sequence");

    bad = good;
    bad.triggers[0].event = Name();
    refuse(std::move(bad), "statechart.bad_sequence");

    bad = good;
    bad.triggers[0].payload = Json("not-an-object");
    refuse(std::move(bad), "statechart.bad_sequence");

    bad = good;
    bad.spans[0].begin = 0.9; // begin after end
    refuse(std::move(bad), "statechart.bad_sequence");

    bad = good;
    bad.spans[0].end = 1.3; // beyond the sheet
    refuse(std::move(bad), "statechart.bad_sequence");

    bad = good;
    bad.spans.push_back(span_over("hitbox", 0.1, 0.2)); // duplicate name
    refuse(std::move(bad), "statechart.duplicate_span");

    bad = good;
    bad.loop_count = 2; // loop_count without end: loop
    refuse(std::move(bad), "statechart.bad_sequence");

    // Nothing instantiated by any refusal; the good sheet still passes.
    CHECK(fix.chart().stats().machines == 0);
    fix.spawn_machine(boss_machine(std::move(good)));
    CHECK(fix.chart().stats().machines == 1);
}

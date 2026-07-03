// core/statechart/sequence_interrupt_test.cpp — mid-span interruption
// (A.2.1: open spans close INSIDE the exit chain, after the state script's
// onExit and before the active substate exits, in reverse open order;
// playhead resets — or saves and resumes under history, with covering spans
// re-opening inside the enter chain), plus the sequence dual-run
// byte-compare (AGENTS.md rule 5: two independent runs, never a self-diff).

#include "core/journal/test_support.h" // slurp
#include "core/statechart/test_support.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Name;
using midday::journal::Record;
using midday::journal::test::slurp;

namespace {

// The canonical sheet again: 1.2 s, swoosh 0.30, hitbox span [0.40, 0.80].
SequenceDesc slash_sheet() {
    SequenceDesc sheet;
    sheet.duration = 1.2;
    TriggerTrackDesc swoosh;
    swoosh.time = 0.30;
    swoosh.event = Name("attack.swoosh");
    sheet.triggers.push_back(swoosh);
    SpanTrackDesc hitbox;
    hitbox.name = Name("hitbox");
    hitbox.begin = 0.40;
    hitbox.end = 0.80;
    sheet.spans.push_back(hitbox);
    return sheet;
}

// SlashAttack owns the sheet AND a substate (Windup) so the journal shows
// WHERE in the exit chain the span closed relative to script and substate.
MachineDesc interrupt_machine(bool history) {
    StateDesc slash =
        state("SlashAttack",
              {pair("SlashAttack.finished", "Passive"), pair("stagger.hit", "Staggered", 10)});
    slash.substates.push_back(state("Windup"));
    slash.initial = Name("Windup");
    slash.history = history;
    slash.sequence = slash_sheet();
    return machine("boss",
                   {region("combat",
                           "SlashAttack",
                           {std::move(slash),
                            state("Passive"),
                            state("Staggered", {pair("recover", "SlashAttack")})})});
}

std::vector<Record> trigger_records(const std::vector<Record>& records, std::string_view event) {
    std::vector<Record> out;
    for (const Record& record : of_kind(records, "event.trigger"))
        if (field(record.payload, "event").as_string() == event)
            out.push_back(record);
    return out;
}

// The first "statechart.hook" record for (state, hook); REQUIREs it exists.
Record hook_record(const std::vector<Record>& records,
                   std::string_view state_name,
                   std::string_view hook) {
    for (const Record& record : of_kind(records, "statechart.hook"))
        if (field(record.payload, "state").as_string() == state_name &&
            field(record.payload, "hook").as_string() == hook)
            return record;
    REQUIRE(false);
    return {};
}

// Fixture driving one interruption at tick 30 and one re-entry at tick 40.
// Returns the finished journal; hooks sit on SlashAttack and Windup so the
// exit/enter chains journal their steps.
std::vector<Record> drive_interruption(ChartFixture& fix, bool history) {
    const MachineId boss = fix.spawn_machine(interrupt_machine(history));
    std::vector<std::string> log;
    RecordingHooks hooks(log);
    for (const char* name : {"SlashAttack", "Windup"})
        REQUIRE_FALSE(
            fix.chart().set_state_hooks(boss, Name("combat"), Name(name), hooks).has_value());

    REQUIRE_FALSE(fix.loop().run_to_tick(30).has_value()); // span open since 24
    REQUIRE_FALSE(fix.trigger("stagger.hit").error.has_value());
    CHECK(fix.chart().active_state(boss, Name("combat")) == Name("Staggered"));
    REQUIRE_FALSE(fix.loop().run_to_tick(40).has_value());
    REQUIRE_FALSE(fix.trigger("recover").error.has_value());
    CHECK(fix.chart().in_state(boss, Name("combat"), Name("SlashAttack")));
    REQUIRE_FALSE(fix.loop().run_to_tick(120).has_value());
    return fix.finish();
}

} // namespace

TEST_CASE("sequence.interrupt: the open span closes INSIDE the exit chain — after the state "
          "script's onExit, before the substate exits — and the playhead resets") {
    ChartFixture fix;
    const std::vector<Record> records = drive_interruption(fix, /*history=*/false);

    // The interruption transition at tick 30.
    const std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE_FALSE(transitions.empty());
    const Record& stagger = transitions[0];
    CHECK(stagger.tick == 30);
    CHECK(field(stagger.payload, "to").as_string() == "Staggered");

    // Journal ids are monotonic: the id order IS the execution order.
    // A.2.1 exit: 1 script onExit -> 2 spans close (before the substate
    // exits) -> ... ; the close cites the transition (cause chain).
    const std::vector<Record> closes = of_kind(records, "sequence.span_close");
    REQUIRE_FALSE(closes.empty());
    const Record& close = closes[0];
    CHECK(close.tick == 30);
    CHECK(field(close.payload, "via").as_string() == "exit");
    CHECK(field(close.payload, "playhead").as_int() == 30);
    CHECK(close.cause_id == stagger.id);
    const Record slash_exit = hook_record(records, "SlashAttack", "exit");
    const Record windup_exit = hook_record(records, "Windup", "exit");
    // Two closed events total: the interruption (tick 30) and the fresh
    // rerun's own boundary close (tick 40 + 48); the first is the exit one.
    const std::vector<Record> closed_events = trigger_records(records, "hitbox.closed");
    REQUIRE(closed_events.size() == 2);
    CHECK(closed_events[0].tick == 30);
    CHECK(closed_events[1].tick == 88);
    CHECK(stagger.id < slash_exit.id);
    CHECK(slash_exit.id < close.id);             // script first, parts still live
    CHECK(close.id < closed_events[0].id);       // record before effect
    CHECK(closed_events[0].id < windup_exit.id); // span closed BEFORE the substate exit

    // No history: re-entry at tick 40 restarts the sheet from 0 — the span
    // re-opens on the fresh schedule (40 + 24) and the sheet finishes at
    // 40 + 72; the swoosh refires at 40 + 18.
    const std::vector<Record> opens = of_kind(records, "sequence.span_open");
    REQUIRE(opens.size() == 2);
    CHECK(opens[0].tick == 24);
    CHECK(opens[1].tick == 64);
    CHECK(field(opens[1].payload, "via").as_string() == "playhead");
    const std::vector<Record> swoosh = trigger_records(records, "attack.swoosh");
    REQUIRE(swoosh.size() == 2);
    CHECK(swoosh[0].tick == 18);
    CHECK(swoosh[1].tick == 58);
    const std::vector<Record> finished = trigger_records(records, "SlashAttack.finished");
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].tick == 112);
}

TEST_CASE("sequence.interrupt_history: the saved playhead resumes — the covering span "
          "re-opens inside the enter chain and the sheet finishes early") {
    ChartFixture fix;
    const std::vector<Record> records = drive_interruption(fix, /*history=*/true);

    // Exit at tick 30 saved playhead 30 (span open since 24 covers it).
    // Re-entry at tick 40: the span re-opens INSIDE the enter chain —
    // after the substate entered, before the state script's onEnter — via
    // "resume", citing the recover transition.
    const std::vector<Record> opens = of_kind(records, "sequence.span_open");
    REQUIRE(opens.size() == 2);
    const Record& reopen = opens[1];
    CHECK(reopen.tick == 40);
    CHECK(field(reopen.payload, "via").as_string() == "resume");
    CHECK(field(reopen.payload, "playhead").as_int() == 30);
    const std::vector<Record> transitions = of_kind(records, "statechart.transition");
    REQUIRE(transitions.size() >= 2);
    const Record& recover = transitions[1];
    CHECK(field(recover.payload, "to").as_string() == "SlashAttack");
    CHECK(reopen.cause_id == recover.id);
    // The re-entry's Windup enter precedes the re-open; the state script's
    // onEnter follows it (A.2.1 enter 3 then 4). These are the ONLY enter
    // hook records: hooks were registered after instantiate, and later
    // registration sees nothing retroactively.
    const Record windup_enter = hook_record(records, "Windup", "enter");
    const Record slash_enter = hook_record(records, "SlashAttack", "enter");
    CHECK(windup_enter.tick == 40);
    CHECK(windup_enter.id < reopen.id); // substate entered first
    CHECK(reopen.id < slash_enter.id);  // brain last, parts live

    // Resumed schedule: close at 40 + (48 - 30), finished at 40 + (72 - 30);
    // the swoosh (local 18 < saved 30) does NOT refire.
    const std::vector<Record> closes = of_kind(records, "sequence.span_close");
    REQUIRE(closes.size() == 2);
    CHECK(closes[1].tick == 58);
    CHECK(field(closes[1].payload, "via").as_string() == "playhead");
    CHECK(trigger_records(records, "attack.swoosh").size() == 1);
    const std::vector<Record> finished = trigger_records(records, "SlashAttack.finished");
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].tick == 82);
}

TEST_CASE("sequence.determinism: two independent interruption/loop runs byte-compare equal") {
    auto drive = [](ChartFixture& fix) -> std::vector<Record> {
        fix.spawn_machine(interrupt_machine(true));
        SequenceDesc pulse;
        pulse.duration = 0.2;
        TriggerTrackDesc beat;
        beat.event = Name("pulse.beat");
        beat.time = 0.1;
        pulse.triggers.push_back(beat);
        pulse.end = SequenceEnd::kLoop;
        pulse.loop_count = 0; // forever
        StateDesc pulsing = state("Pulsing");
        pulsing.sequence = pulse;
        fix.spawn_machine(machine("metronome", {region("m", "Pulsing", {pulsing})}));

        for (std::uint64_t tick = 1; tick <= 150; ++tick) {
            if (tick % 37 == 0)
                REQUIRE_FALSE(fix.trigger("stagger.hit").error.has_value());
            if (tick % 37 == 11)
                REQUIRE_FALSE(fix.trigger("recover").error.has_value());
            REQUIRE_FALSE(fix.loop().tick().has_value());
        }
        CHECK(fix.chart().stats().sequence_loops > 5); // the scenario really ran
        return fix.finish();
    };

    ChartFixture run_a;
    ChartFixture run_b;
    const std::vector<Record> a = drive(run_a);
    const std::vector<Record> b = drive(run_b);

    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(midday::journal::to_jsonl(a[i]) == midday::journal::to_jsonl(b[i]));

    const std::string stream_a = slurp(run_a.bundle_path() + "/journal.jsonl.zst");
    const std::string stream_b = slurp(run_b.bundle_path() + "/journal.jsonl.zst");
    REQUIRE_FALSE(stream_a.empty());
    CHECK(stream_a == stream_b);
}

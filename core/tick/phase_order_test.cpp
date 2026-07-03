// tick.phases / tick.determinism — the m0-tick-loop exit tests: a 600-tick
// headless run journals the Appendix A.1 nine-marker cycle in EXACT order
// every tick (the journal is walked, every marker asserted), and two
// independently constructed same-build runs produce byte-identical journal
// streams (two bundles, compressed bytes compared — never a self-diff).

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/journal/record.h"
#include "core/journal/test_support.h" // slurp (byte-compare helper)
#include "core/tick/test_support.h"
#include "core/tick/tick_loop.h"
#include "doctest/doctest.h"

#include <cstdint>
#include <string>
#include <vector>

using midday::base::Json;
using midday::base::Name;
using midday::bus::EventKey;
using midday::journal::Record;
using midday::journal::test::slurp;
using midday::tick::kPhaseCount;
using midday::tick::kPhaseNames;
using midday::tick::Phase;
using midday::tick::PhaseContext;
using midday::tick::TickLoop;
using midday::tick::test::field;
using midday::tick::test::RecordingHook;
using midday::tick::test::TickFixture;

TEST_CASE("tick.phases: 600 ticks journal the exact A.1 nine-marker cycle, every tick") {
    TickFixture fix;
    REQUIRE_FALSE(fix.loop().run_to_tick(600).has_value());
    CHECK(fix.loop().current_tick() == 600);
    CHECK(fix.loop().stats().ticks == 600);

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == 600 * kPhaseCount);
    for (std::uint64_t tick = 1; tick <= 600; ++tick) {
        const std::uint64_t base = (tick - 1) * kPhaseCount;
        const std::uint64_t tick_begin_id = records[base].id;
        for (std::uint64_t i = 0; i < kPhaseCount; ++i) {
            const Record& marker = records[base + i];
            REQUIRE(marker.kind == "tick.phase");
            REQUIRE(marker.tick == tick);
            REQUIRE(marker.id == base + i + 1); // markers are the ONLY records here
            // The phase spelling at position i IS the enum order — the cycle.
            REQUIRE(field(marker.payload, "phase").as_string() == kPhaseNames[i]);
            // tick-begin is the tick's root; the other eight cite it.
            REQUIRE(marker.cause_id == (i == 0 ? 0 : tick_begin_id));
        }
    }
}

namespace {

// The shared dual-run script: hooks on three phases, an engine-initiated
// bus event chained from the update phase's marker, and a periodic injected
// input — enough traffic to make byte-equality a real claim.
std::vector<Record> drive_scripted_run(TickFixture& fix) {
    std::vector<std::string> log;
    RecordingHook watcher("w", log);
    RecordingHook updater("u", log);
    RecordingHook poster("p", log);
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kWatchers, watcher).has_value());
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kUpdate, updater).has_value());
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kPost, poster).has_value());

    const EventKey key = EventKey::named(Name("global"));
    midday::bus::Bus& bus = fix.bus();
    updater.action = [&bus, key](TickLoop&, const PhaseContext& context) {
        if (context.tick % 7 != 0)
            return;
        Json payload = Json::object();
        payload.set("tick", static_cast<std::int64_t>(context.tick));
        // Engine-initiated effect: caused by THIS phase's marker.
        REQUIRE_FALSE(bus.trigger(key, Name("boss.custom"), payload, context.phase_record_id)
                          .error.has_value());
    };

    for (std::uint64_t tick = 1; tick <= 600; ++tick) {
        if (tick % 10 == 1) {
            Json payload = Json::object();
            payload.set("n", static_cast<std::int64_t>(tick));
            REQUIRE_FALSE(fix.loop().inject_input(key, Name("player.custom"), payload));
        }
        REQUIRE_FALSE(fix.loop().tick().has_value());
    }
    return fix.finish();
}

} // namespace

TEST_CASE("tick.determinism: two independently driven 600-tick runs byte-compare equal") {
    TickFixture run_a;
    TickFixture run_b;
    std::vector<Record> a = drive_scripted_run(run_a);
    std::vector<Record> b = drive_scripted_run(run_b);

    // Record-level identity first (readable failure), then the raw
    // compressed journal streams, byte for byte (the CI-lane claim).
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(midday::journal::to_jsonl(a[i]) == midday::journal::to_jsonl(b[i]));

    const std::string stream_a = slurp(run_a.bundle_path() + "/journal.jsonl.zst");
    const std::string stream_b = slurp(run_b.bundle_path() + "/journal.jsonl.zst");
    REQUIRE_FALSE(stream_a.empty());
    CHECK(stream_a == stream_b);
}

TEST_CASE("tick.phases: injected inputs and hook effects sit INSIDE the marker cycle") {
    TickFixture fix;
    const EventKey key = EventKey::named(Name("global"));
    Json payload = Json::object();
    payload.set("n", 1);
    REQUIRE_FALSE(fix.loop().inject_input(key, Name("player.custom"), payload));
    REQUIRE_FALSE(fix.loop().tick().has_value());

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == kPhaseCount + 1);
    // The trigger lands directly after the input marker, as a ROOT record.
    CHECK(field(records[1].payload, "phase").as_string() == "input");
    CHECK(records[2].kind == "event.trigger");
    CHECK(records[2].cause_id == 0);
    CHECK(field(records[2].payload, "event").as_string() == "player.custom");
    // And the cycle continues undisturbed.
    CHECK(field(records[3].payload, "phase").as_string() == "watchers");
}

// tick.stepping / tick.catchup / tick.frame_packet — deterministic batch
// stepping (tick(n) / run_to_tick), the real-time accumulator with LOUD
// max-catch-up clamping, the render-interpolation alpha, and the
// double-buffered frame-packet seam (packet N readable while N+1 is being
// written; tick numbers correct).

#include "core/tick/frame_packet.h"
#include "core/tick/test_support.h"
#include "core/tick/tick_loop.h"
#include "testkit/doctest.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using midday::tick::AdvanceResult;
using midday::tick::FramePacket;
using midday::tick::FramePacketBuffer;
using midday::tick::Phase;
using midday::tick::PhaseContext;
using midday::tick::TickLoop;
using midday::tick::TickLoopConfig;
using midday::tick::test::RecordingHook;
using midday::tick::test::TickFixture;

TEST_CASE("tick.stepping: fixed dt, batch counts, run_to_tick is idempotent past the target") {
    TickFixture fix;
    CHECK(fix.loop().dt_seconds() == 1.0 / 60.0);
    CHECK(fix.loop().current_tick() == 0);

    REQUIRE_FALSE(fix.loop().tick(5).has_value());
    CHECK(fix.loop().current_tick() == 5);
    REQUIRE_FALSE(fix.loop().run_to_tick(8).has_value());
    CHECK(fix.loop().current_tick() == 8);
    REQUIRE_FALSE(fix.loop().run_to_tick(3).has_value()); // already past: no-op
    CHECK(fix.loop().current_tick() == 8);
    CHECK(fix.loop().stats().ticks == 8);
}

TEST_CASE("tick.catchup: the accumulator steps whole ticks and clamps loudly at the budget") {
    TickLoopConfig config;
    config.max_catchup_steps = 4;
    TickFixture fix(config);
    const double dt = fix.loop().dt_seconds();

    // A huge stall: one full second owed, budget is 4 ticks — the surplus
    // is DROPPED and counted, never silently absorbed.
    AdvanceResult big = fix.loop().advance(1.0);
    REQUIRE_FALSE(big.error.has_value());
    CHECK(big.ticks_run == 4);
    CHECK(std::abs(big.alpha) < 1e-9); // budget consumed exactly
    CHECK(fix.loop().current_tick() == 4);
    CHECK(fix.loop().stats().catchup_clamps == 1);
    CHECK(fix.loop().stats().dropped_seconds == doctest::Approx(1.0 - 4.0 * dt));

    // Normal pacing: half a tick accumulates, no step; another full tick's
    // worth steps once and leaves the half behind as the interpolation alpha.
    AdvanceResult half = fix.loop().advance(0.5 * dt);
    CHECK(half.ticks_run == 0);
    CHECK(half.alpha == doctest::Approx(0.5));
    CHECK(fix.loop().interpolation_alpha() == doctest::Approx(0.5));
    AdvanceResult one = fix.loop().advance(dt);
    CHECK(one.ticks_run == 1);
    CHECK(one.alpha == doctest::Approx(0.5));
    CHECK(fix.loop().current_tick() == 5);

    // Negative elapsed time is a caller bug, treated as zero.
    AdvanceResult negative = fix.loop().advance(-1.0);
    CHECK(negative.ticks_run == 0);
    CHECK(negative.alpha == doctest::Approx(0.5));
    CHECK(fix.loop().stats().catchup_clamps == 1); // unchanged
}

TEST_CASE("tick.frame_packet: the buffer publishes N and keeps it readable while N+1 is written") {
    FramePacketBuffer buffer;
    CHECK(buffer.front() == nullptr); // nothing published yet
    CHECK(buffer.published_count() == 0);

    FramePacket& first = buffer.begin_write();
    first.tick = 41;
    buffer.publish();
    const FramePacket* reader = buffer.front();
    REQUIRE(reader != nullptr);
    CHECK(reader->tick == 41);
    CHECK(reader->sequence == 1);

    // Writing N+1 must not touch the reader's packet — different slot.
    FramePacket& second = buffer.begin_write();
    CHECK(&second != reader);
    second.tick = 42;
    CHECK(buffer.front() == reader); // still N, mid-write
    CHECK(buffer.front()->tick == 41);
    buffer.publish();
    REQUIRE(buffer.front() != nullptr);
    CHECK(buffer.front()->tick == 42);
    CHECK(buffer.front()->sequence == 2);
    CHECK(buffer.published_count() == 2);
}

TEST_CASE("tick.frame_packet: the loop captures one packet per tick at tick-end") {
    TickFixture fix;
    CHECK(fix.loop().frame_packets().front() == nullptr);

    REQUIRE_FALSE(fix.loop().tick().has_value());
    const FramePacket* packet = fix.loop().frame_packets().front();
    REQUIRE(packet != nullptr);
    CHECK(packet->tick == 1);
    CHECK(packet->dt_seconds == fix.loop().dt_seconds());
    CHECK(packet->transform_snapshot == 1);
    CHECK(packet->sequence == 1);

    // During tick 2's open phases the sim is mid-write: the render side
    // still reads tick 1's packet (the seam's whole point).
    std::vector<std::string> log;
    RecordingHook probe("R", log);
    std::uint64_t seen_front_tick = 0;
    probe.action = [&](TickLoop& loop, const PhaseContext&) {
        seen_front_tick = loop.frame_packets().front()->tick;
    };
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kUpdate, probe).has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(seen_front_tick == 1);
    CHECK(fix.loop().frame_packets().front()->tick == 2);
    CHECK(fix.loop().frame_packets().published_count() == 2);
}

TEST_CASE("tick.frame_packet: alpha_hint carries the leftover accumulator of advance()") {
    TickFixture fix;
    const double dt = fix.loop().dt_seconds();
    AdvanceResult result = fix.loop().advance(1.5 * dt);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.ticks_run == 1);
    const FramePacket* packet = fix.loop().frame_packets().front();
    REQUIRE(packet != nullptr);
    CHECK(packet->alpha_hint == doctest::Approx(result.alpha));
    CHECK(packet->alpha_hint == doctest::Approx(0.5));
}

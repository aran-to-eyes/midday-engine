// core/tick/test_support.h — shared fixtures for the tick.* selftests ONLY
// (compiled into midday_tick_tests, never into the library). The canonical
// sim composition lives in testkit/sim_fixture.h (hoisted on its second
// consumer, core/physics); this header keeps the tick spellings plus the
// tick-specific recording hook.

#pragma once

#include "core/tick/tick_loop.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/sim_fixture.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace midday::tick::test {

using testkit::code_of;
using testkit::field;
using testkit::unwrap;
using TickFixture = testkit::SimFixture;

// A hook that appends "<tag>@<phase>:<tick>" to a shared log and optionally
// runs an action (the mid-tick misuse / cascade probe).
struct RecordingHook : PhaseHook {
    std::string tag;
    std::vector<std::string>* log = nullptr;
    std::function<void(TickLoop&, const PhaseContext&)> action;

    RecordingHook(std::string tag_in, std::vector<std::string>& log_in)
        : tag(std::move(tag_in)), log(&log_in) {}

    void on_phase(TickLoop& loop, const PhaseContext& context) override {
        log->push_back(tag + "@" + std::string(to_string(context.phase)) + ":" +
                       std::to_string(context.tick));
        if (action)
            action(loop, context);
    }
};

} // namespace midday::tick::test

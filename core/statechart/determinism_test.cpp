// statechart.determinism — two INDEPENDENTLY constructed runs of the same
// scripted machine scenario (transitions, cascading hooks, watchers,
// state.finished chaining, injected inputs) produce byte-identical journal
// streams. Two bundles compared, never a self-diff (AGENTS.md rule 5).

#include "core/journal/test_support.h" // slurp
#include "core/statechart/test_support.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Json;
using midday::base::Name;
using midday::journal::Record;
using midday::journal::test::slurp;

namespace {

// The scripted scenario: a ping-pong machine whose enter hooks emit an echo
// cascade, plus the watched machine (watcher edges + finished chaining).
std::vector<Record> drive_scripted_run(ChartFixture& fix) {
    const MachineId pingpong = fix.spawn_machine(machine(
        "pingpong",
        {region("r", "A", {state("A", {pair("go", "B")}), state("B", {pair("go", "A")})})}));

    StateDesc guard = state("Guard", {pair("low", "Panic")});
    guard.watchers.push_back({"health < 30", Name("low")});
    MachineDesc watched =
        machine("watched",
                {region("w", "Guard", {guard, state("Panic", {pair("calm", "Guard")})}),
                 region("seq",
                        "Attack",
                        {state("Attack", {pair("Attack.finished", "Cool")}),
                         state("Cool", {pair("warm", "Attack")})})});
    watched.vars.push_back({"health", expr::ValueType::kFloat});
    const MachineId watched_id = fix.spawn_machine(watched);

    std::vector<std::string> log;
    RecordingHooks hooks(log);
    bus::Bus& bus = fix.bus();
    const bus::EventKey key = bus::EventKey::entity(fix.host);
    hooks.enter_action = [&bus, key](Statechart&, const StateHookContext& context) {
        Json payload = Json::object();
        payload.set("state", context.state.view());
        // Hook effects chain from the hook's own record — the cause chain
        // the dual-run compare pins byte for byte.
        REQUIRE_FALSE(bus.trigger(key, Name("echo"), payload, context.record_id).error.has_value());
    };
    for (const char* name : {"A", "B"})
        REQUIRE_FALSE(
            fix.chart().set_state_hooks(pingpong, Name("r"), Name(name), hooks).has_value());

    for (std::uint64_t tick = 1; tick <= 150; ++tick) {
        if (tick % 10 == 1) {
            Json payload = Json::object();
            payload.set("n", static_cast<std::int64_t>(tick));
            REQUIRE_FALSE(fix.loop().inject_input(key, Name("go"), payload));
        }
        if (tick % 7 == 0) {
            const auto health = static_cast<float>((tick * 13) % 60);
            REQUIRE_FALSE(fix.chart().set_var(watched_id, "health", expr::Value::of_float(health)));
        }
        if (tick % 12 == 4)
            REQUIRE_FALSE(fix.trigger("calm").error.has_value());
        if (tick % 30 == 5) {
            if (fix.chart().in_state(watched_id, Name("seq"), Name("Attack")))
                REQUIRE_FALSE(fix.chart().finish_state(watched_id, Name("seq"), Name("Attack"), 0));
            else
                REQUIRE_FALSE(fix.trigger("warm").error.has_value());
        }
        REQUIRE_FALSE(fix.loop().tick().has_value());
    }
    CHECK(fix.chart().stats().transitions > 10); // the scenario really moved
    return fix.finish();
}

} // namespace

TEST_CASE("statechart.determinism: two independent scripted runs byte-compare equal") {
    ChartFixture run_a;
    ChartFixture run_b;
    std::vector<Record> a = drive_scripted_run(run_a);
    std::vector<Record> b = drive_scripted_run(run_b);

    // Record-level identity first (readable failure), then the compressed
    // journal streams, byte for byte (the CI-lane claim).
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(midday::journal::to_jsonl(a[i]) == midday::journal::to_jsonl(b[i]));

    const std::string stream_a = slurp(run_a.bundle_path() + "/journal.jsonl.zst");
    const std::string stream_b = slurp(run_b.bundle_path() + "/journal.jsonl.zst");
    REQUIRE_FALSE(stream_a.empty());
    CHECK(stream_a == stream_b);
}

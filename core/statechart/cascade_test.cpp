// statechart.cascade — transition -> event -> transition chains run nested
// through the bus and hit the SHARED depth cap cleanly (A.2 rule 5 /
// D-BUILD-047: the offending trigger gets the structured refusal, every
// enclosing level completes, no unwinding), and the own-region stamp is the
// cycle breaker: an onEnter emission matching the NEW state's own pairs is
// voided this tick, never an infinite loop.

#include "core/statechart/test_support.h"

#include <optional>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Name;
using midday::journal::Record;

namespace {

// Emits one event from on_enter, chaining from the hook's own record id —
// the cause-chain discipline every scripted hook will follow.
struct EmitOnEnter final : StateHooks {
    bus::Bus* bus = nullptr;
    bus::EventKey key;
    Name event;
    std::vector<std::optional<base::Error>>* results = nullptr;

    void on_enter(Statechart&, const StateHookContext& context) override {
        if (event.empty())
            return;
        results->push_back(bus->trigger(key, event, base::Json::object(), context.record_id).error);
    }
};

} // namespace

TEST_CASE("statechart.cascade: transition chains hit the shared depth cap cleanly") {
    ChartFixture fix;
    // Forty regions r0..r39: I<i> --step<i>--> T<i>; T<i>'s enter hook emits
    // step<i+1>. One outside trigger walks the whole chain nested until the
    // bus cap refuses the deeper trigger.
    constexpr std::uint32_t kRegions = 40;
    std::vector<RegionDesc> regions;
    for (std::uint32_t i = 0; i < kRegions; ++i) {
        const std::string n = std::to_string(i);
        regions.push_back(region(
            "r" + n, "I" + n, {state("I" + n, {pair("step" + n, "T" + n)}), state("T" + n)}));
    }
    const MachineId id = fix.spawn_machine(machine("chain", std::move(regions)));

    std::vector<std::optional<base::Error>> results;
    std::vector<EmitOnEnter> hooks(kRegions);
    for (std::uint32_t i = 0; i < kRegions; ++i) {
        hooks[i].bus = &fix.bus();
        hooks[i].key = bus::EventKey::entity(fix.host);
        hooks[i].event = Name("step" + std::to_string(i + 1));
        hooks[i].results = &results;
        const std::string n = std::to_string(i);
        REQUIRE_FALSE(
            fix.chart().set_state_hooks(id, Name("r" + n), Name("T" + n), hooks[i]).has_value());
    }

    // The outermost trigger itself succeeds — refusal hits only the trigger
    // that would nest past the cap (level 33), and every enclosing dispatch
    // completes normally.
    REQUIRE_FALSE(fix.trigger("step0").error.has_value());
    CHECK(fix.chart().stats().transitions == bus::Bus::kMaxCascadeDepth);
    CHECK(fix.chart().in_state(id, Name("r31"), Name("T31")));
    CHECK(fix.chart().in_state(id, Name("r32"), Name("I32"))); // never reached

    std::size_t refused = 0;
    for (const std::optional<base::Error>& error : results)
        if (error.has_value()) {
            ++refused;
            CHECK(error->code == "bus.cascade_depth");
        }
    CHECK(refused == 1);

    // The refusal journaled itself (the journal explains its own gaps).
    std::vector<Record> records = fix.finish();
    CHECK(of_kind(records, "bus.cascade_depth").size() == 1);
    CHECK(of_kind(records, "statechart.transition").size() == bus::Bus::kMaxCascadeDepth);
}

TEST_CASE("statechart.cascade: onEnter emissions matching the new state's pairs are voided") {
    ChartFixture fix;
    const MachineId id = fix.spawn_machine(machine(
        "cycle",
        {region("r", "A", {state("A", {pair("ping", "B")}), state("B", {pair("ping", "A")})})}));
    std::vector<std::optional<base::Error>> results;
    EmitOnEnter hook;
    hook.bus = &fix.bus();
    hook.key = bus::EventKey::entity(fix.host);
    hook.event = Name("ping");
    hook.results = &results;
    REQUIRE_FALSE(fix.chart().set_state_hooks(id, Name("r"), Name("B"), hook).has_value());

    // ping -> B; B.onEnter re-emits ping — the region is stamped, so the
    // emission voids region-wide instead of transitioning (A.2 rule 5).
    REQUIRE_FALSE(fix.trigger("ping").error.has_value());
    CHECK(fix.chart().in_state(id, Name("r"), Name("B")));
    CHECK(fix.chart().stats().transitions == 1);
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].has_value()); // the emission itself is legal

    std::vector<Record> records = fix.finish();
    const std::vector<Record> voided = of_kind(records, "statechart.voided");
    REQUIRE(voided.size() == 2); // both ping pairs, region-wide
    CHECK(field(voided[0].payload, "reason").as_string() == "region_already_transitioned");
    CHECK(field(voided[1].payload, "reason").as_string() == "region_already_transitioned");
    // The voided records cite the hook's OWN emission — the cause chain
    // shows exactly which trigger got swallowed by the cycle breaker.
    const std::vector<Record> triggers = of_kind(records, "event.trigger");
    REQUIRE(triggers.size() == 2);
    CHECK(voided[0].cause_id == triggers[1].id);
}

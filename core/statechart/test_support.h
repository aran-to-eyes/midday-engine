// core/statechart/test_support.h — shared fixtures for the statechart.*
// selftests ONLY (compiled into midday_statechart_tests, never the library).
// One canonical composition: Registry + World + Hierarchy + pinned journal
// Writer + Bus + TickLoop + Statechart under a testkit TempDir (the tick
// fixture pattern, D-BUILD-013: no cwd-dependent IO, no wall clock).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/reader.h"
#include "core/journal/writer.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "core/statechart/machine_desc.h"
#include "core/statechart/statechart.h"
#include "core/tick/tick_loop.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::statechart::test {

using testkit::unwrap;

// Registry (built-in vocabulary) + World + Hierarchy + Writer + Bus +
// TickLoop + Statechart wired the canonical way, plus one adopted host
// entity. Call finish() exactly once to close the journal and read it back.
struct ChartFixture {
    testkit::TempDir dir{"statechart"};
    reflect::Registry registry;
    ecs::World world{registry};
    hierarchy::Hierarchy hierarchy{world};
    std::optional<journal::Writer> writer_slot;
    std::optional<bus::Bus> bus_slot;
    std::optional<tick::TickLoop> loop_slot;
    std::optional<Statechart> chart_slot; // destroyed FIRST (detaches hooks)
    ecs::EntityRef host;

    explicit ChartFixture(journal::TierConfig tiers = {}) {
        reflect::register_builtin_events(registry);
        journal::WriterConfig pinned;
        pinned.engine_version = "0.0.0-fixture";
        pinned.api_compat_hash = "0000000000000000";
        pinned.seed = 7;
        pinned.platform = "fixture-neutral"; // info-only, pinned for byte-compares
        pinned.tiers = tiers;
        auto opened = journal::Writer::create(dir.file("run.mrj"), pinned);
        REQUIRE_FALSE(opened.error.has_value());
        writer_slot.emplace(std::move(unwrap(opened.writer)));
        bus_slot.emplace(world, registry, unwrap(writer_slot));
        loop_slot.emplace(world, hierarchy, unwrap(bus_slot), unwrap(writer_slot));
        chart_slot.emplace(
            world, hierarchy, unwrap(bus_slot), unwrap(writer_slot), unwrap(loop_slot));
        host = world.spawn();
        REQUIRE_FALSE(host.is_null());
        REQUIRE_FALSE(hierarchy.adopt(host).has_value());
    }

    [[nodiscard]] Statechart& chart() { return unwrap(chart_slot); }

    [[nodiscard]] bus::Bus& bus() { return unwrap(bus_slot); }

    [[nodiscard]] tick::TickLoop& loop() { return unwrap(loop_slot); }

    [[nodiscard]] journal::Writer& writer() { return unwrap(writer_slot); }

    [[nodiscard]] std::string bundle_path() const { return dir.file("run.mrj"); }

    // Instantiate on the fixture host, asserting success.
    MachineId spawn_machine(const MachineDesc& desc) {
        InstantiateResult result = chart().instantiate(desc, host);
        REQUIRE_FALSE(result.error.has_value());
        REQUIRE(result.machine != kInvalidMachine);
        return result.machine;
    }

    // Trigger an event on the host's private channel (empty payload).
    bus::TriggerResult trigger(std::string_view event, std::uint64_t cause_id = 0) {
        return bus().trigger(
            bus::EventKey::entity(host), base::Name(event), base::Json::object(), cause_id);
    }

    // Close the writer and stream the whole journal back.
    std::vector<journal::Record> finish() {
        REQUIRE_FALSE(writer().close().has_value());
        auto opened = journal::Reader::open(bundle_path());
        REQUIRE_FALSE(opened.error.has_value());
        journal::Reader& reader = unwrap(opened.reader);
        std::vector<journal::Record> records;
        while (true) {
            auto next = reader.next();
            REQUIRE_FALSE(next.error.has_value());
            if (!next.record.has_value())
                break;
            records.push_back(std::move(*next.record));
        }
        return records;
    }
};

// Recording hooks: appends "<kind>:<state>" to a shared log and optionally
// runs per-kind actions (the cascade / no-zombie-hitbox probes).
struct RecordingHooks final : StateHooks {
    std::vector<std::string>* log = nullptr;
    std::function<void(Statechart&, const StateHookContext&)> enter_action;
    std::function<void(Statechart&, const StateHookContext&)> exit_action;

    explicit RecordingHooks(std::vector<std::string>& log_in) : log(&log_in) {}

    void on_enter(Statechart& chart, const StateHookContext& context) override {
        log->push_back("enter:" + std::string(context.state.view()));
        if (enter_action)
            enter_action(chart, context);
    }

    void on_exit(Statechart& chart, const StateHookContext& context) override {
        log->push_back("exit:" + std::string(context.state.view()));
        if (exit_action)
            exit_action(chart, context);
    }

    void on_update(Statechart&, const StateHookContext& context) override {
        log->push_back("update:" + std::string(context.state.view()));
    }

    void on_fixed_update(Statechart&, const StateHookContext& context) override {
        log->push_back("fixed:" + std::string(context.state.view()));
    }
};

// Recording component hooks (the enter-2/exit-3 seats): appends
// "c-enter:<state>/<component>" / "c-exit:<state>/<component>" to a shared
// log — the component-side sibling of RecordingHooks.
struct RecordingComponentHooks final : ComponentHooks {
    std::vector<std::string>* log = nullptr;

    explicit RecordingComponentHooks(std::vector<std::string>& log_in) : log(&log_in) {}

    void on_enter(Statechart&, const ComponentHookContext& context) override {
        log->push_back("c-enter:" + std::string(context.state.view()) + "/" +
                       std::string(context.component.view()));
    }

    void on_exit(Statechart&, const ComponentHookContext& context) override {
        log->push_back("c-exit:" + std::string(context.state.view()) + "/" +
                       std::string(context.component.view()));
    }
};

// Desc builders — tests read as machine literals.
inline TransitionDesc pair(std::string_view event,
                           std::string_view target,
                           std::int32_t priority = 0,
                           std::string condition = {}) {
    TransitionDesc t;
    t.event = base::Name(event);
    t.target = base::Name(target);
    t.priority = priority;
    t.condition = std::move(condition);
    return t;
}

inline StateDesc state(std::string_view name, std::vector<TransitionDesc> transitions = {}) {
    StateDesc s;
    s.name = base::Name(name);
    s.transitions = std::move(transitions);
    return s;
}

inline RegionDesc region(std::string_view name,
                         std::string_view initial,
                         std::vector<StateDesc> states,
                         std::vector<TransitionDesc> any_state = {}) {
    RegionDesc r;
    r.name = base::Name(name);
    r.initial = base::Name(initial);
    r.states = std::move(states);
    r.any_state = std::move(any_state);
    return r;
}

inline MachineDesc machine(std::string_view name, std::vector<RegionDesc> regions) {
    MachineDesc m;
    m.name = base::Name(name);
    m.regions = std::move(regions);
    return m;
}

// The error code carried by an optional<Error>, "<none>" when clear.
inline std::string code_of(const std::optional<base::Error>& error) {
    return error.has_value() ? error->code : std::string("<none>");
}

// Assert-and-access for a JSON object field.
inline const base::Json& field(const base::Json& object, std::string_view key) {
    const base::Json* value = object.find(key);
    REQUIRE(value != nullptr);
    if (value == nullptr)
        std::abort(); // unreachable: REQUIRE threw
    return *value;
}

// All records of one kind, in journal order.
inline std::vector<journal::Record> of_kind(const std::vector<journal::Record>& records,
                                            std::string_view kind) {
    std::vector<journal::Record> out;
    for (const journal::Record& record : records)
        if (record.kind == kind)
            out.push_back(record);
    return out;
}

} // namespace midday::statechart::test

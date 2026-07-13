// core/loader/prefab_test_support.h — shared fixture content for
// m1-prefab-spawn's tests ONLY (core/loader/prefab_spawn_test.cpp,
// core/loader/despawn_linger_test.cpp, core/loader/despawn_journal_test.cpp
// AND ts/runtime/world_host_test.cpp — the statechart::test::ChartFixture
// precedent for a cross-module test-support header). Never compiled into a
// library.

#pragma once

#include "core/base/file_io.h"
#include "core/bus/test_support.h"
#include "core/loader/prefab_spawn.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/temp_dir.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace midday::loader::test {

// Fixture: ChartFixture (World+Hierarchy+Bus+Writer+TickLoop+Statechart)
// wired to a PrefabSpawner installed as BOTH halves of the tick's two-phase
// structural extension (M2 0B D4: prepare pre-flush, realize post-flush) —
// the EXACT composition cli/verbs/run.cpp's RunSim would build for a real
// script host, minus the TS runtime itself (the engine mechanics are what
// the exit tests are about; ts/runtime/world_host_test.cpp covers the
// script-boundary plumbing separately).
struct PrefabFixture {
    statechart::test::ChartFixture fix;
    EventsDecl events;
    PrefabSpawner spawner;

    PrefabFixture()
        : spawner(fix.world,
                  fix.hierarchy,
                  fix.chart(),
                  fix.bus(),
                  fix.writer(),
                  fix.registry,
                  events,
                  ComponentVocab{},
                  &fix.loop()) {
        fix.loop().set_structural_preparer(
            [this](std::uint64_t tick, std::uint64_t phase_record_id) {
                return spawner.prepare(tick, phase_record_id);
            });
        fix.loop().set_structural_realizer(
            [this](std::uint64_t phase_record_id) { return spawner.realize(phase_record_id); });
    }
};

// One trivial prefab: a single-region, single-state machine, no components —
// enough to prove the enter chain (Statechart::instantiate always enters the
// initial chain) without dragging in physics/rendering, which are out of
// this node's scope (see core/loader/prefab_spawn.h header note). Returns
// the entity file's path.
inline std::string write_goblin_prefab(const testkit::TempDir& dir) {
    REQUIRE_FALSE(base::write_file(dir.file("goblin.machine.yaml"),
                                   "format: 1\n"
                                   "machine: goblin\n"
                                   "regions:\n"
                                   "  main:\n"
                                   "    initial: Idle\n"
                                   "    states:\n"
                                   "      Idle: {}\n",
                                   "t")
                      .has_value());
    // Forward-slash form: this path is embedded verbatim into a TS string
    // literal (world.spawn('<path>')) where a native Windows backslash is a JS
    // escape and mangles the path; '/' is valid for file I/O on every platform
    // (D-BUILD-113: native-vs-generic broke the Windows lane before).
    const std::string entity_path =
        std::filesystem::path(dir.file("goblin.entity.yaml")).generic_string();
    REQUIRE_FALSE(base::write_file(entity_path,
                                   "format: 1\n"
                                   "entity: Goblin\n"
                                   "machines:\n"
                                   "  - instance: {path: goblin.machine.yaml}\n",
                                   "t")
                      .has_value());
    return entity_path;
}

// The DespawnHooks seam double: records the call ORDER (shared log with the
// chart's recording hooks) and every note_despawn reap tick (the G2 pin).
struct RecordingDespawnHooks final : DespawnHooks {
    std::vector<std::string>* log = nullptr;
    std::vector<std::uint64_t> note_ticks;

    explicit RecordingDespawnHooks(std::vector<std::string>& log_in) : log(&log_in) {}

    void despawn_exit(ecs::EntityRef ref, std::uint64_t) override {
        log->push_back("despawn_exit:" + std::to_string(ref.index));
    }

    void note_despawn(ecs::EntityRef ref, std::uint64_t reap_tick) override {
        note_ticks.push_back(reap_tick);
        log->push_back("note:" + std::to_string(ref.index) + "@" + std::to_string(reap_tick));
    }

    void reap_entity(ecs::EntityRef ref) override {
        log->push_back("reap:" + std::to_string(ref.index));
    }
};

// Exit-only script hooks: the phase-8 ordering log must not be polluted by
// the per-tick fixed-update hook lines RecordingHooks would add.
struct ExitRecordingHooks final : statechart::StateHooks {
    std::vector<std::string>* log = nullptr;

    explicit ExitRecordingHooks(std::vector<std::string>& log_in) : log(&log_in) {}

    void on_exit(statechart::Statechart&, const statechart::StateHookContext& context) override {
        log->push_back("exit:" + std::string(context.state.view()));
    }
};

// Every phase-8 effect surface wired to observers sharing ONE log — the
// chart exit chain (state hooks on the goblin's Idle), the base-onExit/reap
// seam (DespawnHooks), and the entity channel (entity.despawned) — seated on
// a realized goblin root.
struct EffectObservers {
    std::vector<std::string> log;
    ExitRecordingHooks scripts{log};
    RecordingDespawnHooks hooks{log};
    bus::test::RecordingListener bus_probe{"bus", log};

    EffectObservers(PrefabFixture& pf, ecs::EntityRef root) {
        const ecs::EntityRef machine_root = pf.fix.hierarchy.first_child_of(root);
        const auto* root_component = pf.fix.world.try_get<statechart::MachineRoot>(machine_root);
        REQUIRE(root_component != nullptr);
        REQUIRE_FALSE(
            pf.fix.chart()
                .set_state_hooks(
                    root_component->machine, base::Name("main"), base::Name("Idle"), scripts)
                .has_value());
        pf.spawner.set_despawn_hooks(hooks);
        REQUIRE_FALSE(pf.fix.bus().subscribe(bus_probe, bus::EventKey::entity(root)).has_value());
    }
};

// All entity.despawned event.trigger records, journal order.
inline std::vector<journal::Record>
despawned_triggers(const std::vector<journal::Record>& records) {
    std::vector<journal::Record> out;
    for (const journal::Record& record : statechart::test::of_kind(records, "event.trigger"))
        if (record.payload.find("event") != nullptr &&
            record.payload.find("event")->as_string() == "entity.despawned")
            out.push_back(record);
    return out;
}

} // namespace midday::loader::test

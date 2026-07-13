// loader.component_materialize — the ONE component dispatcher (M2 0B,
// #12b): script components route to the injected materializer in
// MATERIALIZATION ORDER (base authored order before machines; state
// components in document order), a component nothing claims REFUSES
// (component.no_materializer — never a silent omit), state components
// demand the deferred-entry split (component.requires_deferred_entry), the
// generic native-Transform entry places the prefab root, and the other
// native names refuse on the generic path (component.native_unsupported).

#include "core/base/file_io.h"
#include "core/loader/component_materialize.h"
#include "core/loader/loader.h"
#include "core/statechart/statechart.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/sim_fixture.h"

#include <optional>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::loader;
using midday::base::Name;
using midday::testkit::unwrap;

namespace {

// Records every dispatcher call as one log line; refusals injectable.
struct RecordingMaterializer final : ScriptComponentMaterializer {
    std::vector<std::string> log;

    std::optional<base::Error> materialize_base(ecs::EntityRef entity,
                                                const GenericComponentEntry& entry,
                                                std::uint64_t /*cause_id*/) override {
        log.push_back("base:" + std::string(entry.type.view()) + "@e" +
                      std::to_string(entity.index));
        return std::nullopt;
    }

    std::optional<base::Error> materialize_state(statechart::Statechart& /*chart*/,
                                                 statechart::MachineId /*machine*/,
                                                 base::Name region,
                                                 base::Name state,
                                                 ecs::EntityRef /*host*/,
                                                 const statechart::StateComponentDesc& desc,
                                                 std::uint64_t /*cause_id*/) override {
        log.push_back("state:" + std::string(region.view()) + "/" + std::string(state.view()) +
                      "/" + std::string(desc.type.view()));
        return std::nullopt;
    }

    std::optional<base::Error> mirror_native_transform(ecs::EntityRef entity,
                                                       const math::Transform& value,
                                                       std::uint64_t /*cause_id*/) override {
        log.push_back("mirror:Transform@e" + std::to_string(entity.index) +
                      ":x=" + std::to_string(static_cast<int>(value.translation.x)));
        return std::nullopt;
    }
};

// A component-bearing corpus: a prefab with base (Transform + Relay) + a
// machine whose states own components + a component-carrying state child,
// and an inline entity with a non-native component.
void write_corpus(const testkit::TempDir& dir) {
    REQUIRE_FALSE(base::write_file(dir.file("probe.machine.yaml"),
                                   "format: 1\n"
                                   "machine: probe\n"
                                   "regions:\n"
                                   "  life:\n"
                                   "    initial: Alive\n"
                                   "    states:\n"
                                   "      Alive:\n"
                                   "        components:\n"
                                   "          - PulseA: {}\n"
                                   "          - PulseB: {rate: 2}\n"
                                   "        initial: Armed\n"
                                   "        states:\n"
                                   "          Armed:\n"
                                   "            components:\n"
                                   "              - ChildPulse: {}\n"
                                   "            children:\n"
                                   "              - entity: Probe\n"
                                   "                at: [0, 1.0, 0]\n"
                                   "                components:\n"
                                   "                  - Relay: {gain: 3}\n"
                                   "      Dead: {}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("probe.entity.yaml"),
                                   "format: 1\n"
                                   "entity: LifecycleProbe\n"
                                   "base:\n"
                                   "  - Transform: {at: [7, 0, 3]}\n"
                                   "  - Relay: {expectedX: 7}\n"
                                   "machines:\n"
                                   "  - instance: {path: probe.machine.yaml}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("arena.scene.yaml"),
                                   "format: 1\n"
                                   "scene: arena\n"
                                   "entities:\n"
                                   "  - entity: Probe\n"
                                   "    prefab: {path: probe.entity.yaml}\n"
                                   "    at: [1, 0, 0]\n"
                                   "  - entity: Watcher\n"
                                   "    components:\n"
                                   "      - Relay: {gain: 1}\n",
                                   "t")
                      .has_value());
}

ComponentVocab test_vocab() {
    ComponentVocab vocab;
    vocab.extracted = {"Relay", "PulseA", "PulseB", "ChildPulse"};
    return vocab;
}

// SimFixture + Statechart + the loaded corpus, spawn deferred to each case.
struct MaterializeSim {
    testkit::TempDir dir{"component-materialize"};
    testkit::SimFixture sim;
    std::optional<statechart::Statechart> chart_slot;
    std::optional<SceneLoadResult> loaded;

    MaterializeSim() {
        write_corpus(dir);
        chart_slot.emplace(sim.world, sim.hierarchy, sim.bus(), sim.writer(), sim.loop());
        loaded.emplace(load_scene(dir.file("arena.scene.yaml"), sim.registry, false, test_vocab()));
        REQUIRE_FALSE(unwrap(loaded).error.has_value());
    }

    [[nodiscard]] statechart::Statechart& chart() { return unwrap(chart_slot); }

    [[nodiscard]] const SceneFile& scene() { return unwrap(unwrap(loaded).scene); }

    SpawnResult spawn(const SpawnOptions& options) {
        return spawn_scene(
            scene(), sim.world, sim.hierarchy, chart(), nullptr, sim.writer(), 0, options);
    }
};

} // namespace

TEST_CASE("loader.component_materialize: no materializer -> structured refusal, nothing spawns "
          "silently") {
    MaterializeSim sim;
    SpawnResult spawned = sim.spawn(SpawnOptions{}); // every pre-0B caller's default
    const base::Error& error = unwrap(spawned.error);
    const base::Json* cause = error.details.find("cause");
    REQUIRE(cause != nullptr);
    REQUIRE(cause->find("code") != nullptr);
    CHECK(cause->find("code")->as_string() == "component.no_materializer");
}

TEST_CASE("loader.component_materialize: state components demand the deferred-entry split") {
    MaterializeSim sim;
    RecordingMaterializer scripts;
    SpawnOptions options;
    options.scripts = &scripts;
    options.defer_initial_entry = false; // seated too late: refuse, never a late synthetic onEnter
    SpawnResult spawned = sim.spawn(options);
    const base::Error& error = unwrap(spawned.error);
    const base::Json* cause = error.details.find("cause");
    REQUIRE(cause != nullptr);
    REQUIRE(cause->find("code") != nullptr);
    CHECK(cause->find("code")->as_string() == "component.requires_deferred_entry");
}

TEST_CASE("loader.component_materialize: materialization order = base authored order -> state "
          "components document order -> children; Transform places the root; entries start "
          "deferred") {
    MaterializeSim sim;
    RecordingMaterializer scripts;
    SpawnOptions options;
    options.scripts = &scripts;
    options.defer_initial_entry = true;
    SpawnResult spawned = sim.spawn(options);
    REQUIRE_FALSE(spawned.error.has_value());

    // The stable prefix: prefab base (Transform mirror, then Relay in
    // authored order), then the machine's state components in document
    // order. The Probe child + inline Watcher lines carry spawn-dependent
    // indices — asserted structurally.
    const ecs::EntityRef root = spawned.machines.at(0).host;
    REQUIRE(scripts.log.size() == 7);
    CHECK(scripts.log[0] == "mirror:Transform@e" + std::to_string(root.index) + ":x=7");
    CHECK(scripts.log[1] == "base:Relay@e" + std::to_string(root.index));
    CHECK(scripts.log[2] == "state:life/Alive/PulseA");
    CHECK(scripts.log[3] == "state:life/Alive/PulseB");
    CHECK(scripts.log[4] == "state:life/Armed/ChildPulse");
    CHECK(scripts.log[5].rfind("base:Relay@e", 0) == 0); // the Probe child's component
    CHECK(scripts.log[6].rfind("base:Relay@e", 0) == 0); // the inline Watcher's component

    // The generic base Transform actually PLACED the root.
    const math::Transform* local = sim.sim.hierarchy.local_of(root);
    REQUIRE(local != nullptr);
    CHECK(local->translation.x == doctest::Approx(7.0));

    // Deferred entries: start_initial_entries runs them all, exactly once.
    REQUIRE_FALSE(start_initial_entries(sim.chart(), spawned).has_value());
    CHECK(sim.chart().in_state(spawned.machines.at(0).id, Name("life"), Name("Armed")));
    CHECK(unwrap(sim.chart().start_initial_entry(spawned.machines.at(0).id)).code ==
          "statechart.already_entered");
}

TEST_CASE("loader.component_materialize: generic native entries — Collider refuses, child "
          "Transform refuses (at: owns placement)") {
    testkit::SimFixture sim;
    hierarchy::Hierarchy& hierarchy = sim.hierarchy;
    RecordingMaterializer scripts;
    SpawnOptions options;
    options.scripts = &scripts;

    const ecs::EntityRef entity = sim.world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(entity).has_value());

    GenericComponentEntry collider;
    collider.type = Name("Collider");
    std::optional<base::Error> refused = materialize_base_component(
        hierarchy, options, entity, collider, 0, /*allow_native_transform=*/true);
    CHECK(unwrap(refused).code == "component.native_unsupported");

    GenericComponentEntry transform;
    transform.type = Name("Transform");
    refused = materialize_base_component(
        hierarchy, options, entity, transform, 0, /*allow_native_transform=*/false);
    CHECK(unwrap(refused).code == "component.native_unsupported");

    // Bad Transform fields refuse loudly, never a partial apply.
    GenericComponentEntry bad;
    bad.type = Name("Transform");
    bad.fields = base::Json::object();
    bad.fields.set("position", base::Json::array()); // wrong key: the grammar is {at: [x,y,z]}
    refused = materialize_base_component(
        hierarchy, options, entity, bad, 0, /*allow_native_transform=*/true);
    CHECK(unwrap(refused).code == "component.bad_fields");
}

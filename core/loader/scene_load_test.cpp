// loader.scene — `*.scene.yaml`: entities with the M0 component vocabulary,
// events-file wiring (project vocabulary loads first), machine instances by
// path (deduplicated), the project-root path convention, and the M0
// physics-mapping refusals.

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <filesystem>
#include <string>

using namespace midday;
using namespace midday::loader;
using midday::base::Name;
using midday::testkit::unwrap;

namespace {

struct SceneFixture {
    testkit::TempDir dir{"loader-scene"};
    reflect::Registry registry;

    SceneFixture() {
        reflect::register_builtin_events(registry);
        REQUIRE_FALSE(base::write_file(dir.file("combat.events.yaml"),
                                       "format: 1\n"
                                       "events:\n"
                                       "  death.dealt: {payload: {by: entity_ref}}\n"
                                       "  boss.died: {payload: {boss: string}}\n",
                                       "t")
                          .has_value());
        REQUIRE_FALSE(base::write_file(dir.file("boss.machine.yaml"),
                                       "format: 1\n"
                                       "machine: boss\n"
                                       "regions:\n"
                                       "  combat:\n"
                                       "    initial: Passive\n"
                                       "    anystate:\n"
                                       "      - {event: death.dealt, goto: Dead, priority: 100}\n"
                                       "    states:\n"
                                       "      Passive: {}\n"
                                       "      Dead: {}\n",
                                       "t")
                          .has_value());
    }

    SceneLoadResult load(const std::string& text) {
        const std::string path = dir.file("arena.scene.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
        return load_scene(path, registry);
    }
};

} // namespace

TEST_CASE("loader.scene: entities, components, events, and machines load; refs dedupe") {
    SceneFixture fix;
    SceneLoadResult loaded = fix.load("format: 1\n"
                                      "scene: arena\n"
                                      "events: [combat.events.yaml]\n"
                                      "entities:\n"
                                      "  - entity: Ground\n"
                                      "    components:\n"
                                      "      - Transform: {}\n"
                                      "      - Collider: {shape: plane}\n"
                                      "  - entity: Boss\n"
                                      "    components:\n"
                                      "      - Transform: {at: [0, 0.9, 0]}\n"
                                      "      - Collider: {shape: box, size: [1.2, 1.8, 1.2]}\n"
                                      "      - RigidBody: {}\n"
                                      "    machines:\n"
                                      "      - {instance: {path: boss.machine.yaml}}\n"
                                      "  - entity: Twin\n"
                                      "    machines:\n"
                                      "      - {instance: {path: boss.machine.yaml}}\n");
    REQUIRE_FALSE(loaded.error.has_value());
    const SceneFile& scene = unwrap(loaded.scene);
    CHECK(scene.name == Name("arena"));
    CHECK(scene.events.has_event("death.dealt"));
    CHECK(scene_uses_physics(scene));

    REQUIRE(scene.entities.size() == 3);
    const SceneEntityDesc& ground = scene.entities[0];
    CHECK(ground.components.transform.has_value());
    REQUIRE(ground.components.collider.has_value());
    CHECK_FALSE(unwrap(ground.components.collider).box);
    CHECK_FALSE(ground.components.rigid_body);

    const SceneEntityDesc& boss = scene.entities[1];
    CHECK(unwrap(boss.components.transform).translation.y == 0.9F);
    CHECK(unwrap(boss.components.collider).box);
    CHECK(unwrap(boss.components.collider).size.y == 1.8F);
    CHECK(boss.components.rigid_body);

    // One machine FILE, two instances (dedup by resolved path).
    REQUIRE(scene.machines.size() == 1);
    CHECK(scene.machines[0].desc.name == Name("boss"));
    REQUIRE(boss.machines.size() == 1);
    CHECK(boss.machines[0] == 0);
    CHECK(scene.entities[2].machines[0] == 0);
}

TEST_CASE("loader.scene: a physics-free scene reports no physics use") {
    SceneFixture fix;
    SceneLoadResult loaded = fix.load("format: 1\nscene: bare\nentities:\n"
                                      "  - entity: Marker\n"
                                      "    components: [{Transform: {at: [1, 2, 3]}}]\n");
    REQUIRE_FALSE(loaded.error.has_value());
    CHECK_FALSE(scene_uses_physics(unwrap(loaded.scene)));
}

TEST_CASE("loader.scene: strict component refusals") {
    SceneFixture fix;
    auto unknown = fix.load("format: 1\nscene: s\nentities:\n"
                            "  - entity: E\n"
                            "    components: [{Health: {max: 120}}]\n");
    REQUIRE(unknown.error.has_value());
    CHECK(unwrap(unknown.error).code == "loader.unknown_key");
    CHECK(unwrap(unknown.error).message.find("unknown component 'Health'") != std::string::npos);
    CHECK(unwrap(unknown.error).details.find("line")->as_int() == 5);

    auto static_box = fix.load("format: 1\nscene: s\nentities:\n"
                               "  - entity: E\n"
                               "    components: [{Collider: {shape: box, size: [1, 1, 1]}}]\n");
    REQUIRE(static_box.error.has_value());
    CHECK(unwrap(static_box.error).code == "loader.unsupported");
    CHECK(unwrap(static_box.error).message.find("m4-physics-full") != std::string::npos);

    auto bare_body = fix.load("format: 1\nscene: s\nentities:\n"
                              "  - entity: E\n"
                              "    components: [{RigidBody: {}}]\n");
    REQUIRE(bare_body.error.has_value());
    CHECK(unwrap(bare_body.error).code == "loader.bad_value");

    auto kinematic = fix.load("format: 1\nscene: s\nentities:\n"
                              "  - entity: E\n"
                              "    components:\n"
                              "      - Collider: {shape: box, size: [1, 1, 1]}\n"
                              "      - RigidBody: {kinematic: true}\n");
    REQUIRE(kinematic.error.has_value());
    CHECK(unwrap(kinematic.error).code == "loader.unknown_key");

    auto sized_plane = fix.load("format: 1\nscene: s\nentities:\n"
                                "  - entity: E\n"
                                "    components: [{Collider: {shape: plane, size: [1, 1, 1]}}]\n");
    REQUIRE(sized_plane.error.has_value());
    CHECK(unwrap(sized_plane.error).message.find("plane collider takes no size") !=
          std::string::npos);
}

TEST_CASE("loader.scene: broken references refuse at the referencing line") {
    SceneFixture fix;
    auto no_events = fix.load("format: 1\nscene: s\nevents: [ghost.events.yaml]\n");
    REQUIRE(no_events.error.has_value());
    CHECK(unwrap(no_events.error).code == "loader.bad_ref");
    CHECK(unwrap(no_events.error).details.find("line")->as_int() == 3);

    auto no_machine = fix.load("format: 1\nscene: s\nentities:\n"
                               "  - entity: E\n"
                               "    machines: [{instance: {path: ghost.machine.yaml}}]\n");
    REQUIRE(no_machine.error.has_value());
    CHECK(unwrap(no_machine.error).code == "loader.bad_ref");
    CHECK(unwrap(no_machine.error).message.find("ghost.machine.yaml") != std::string::npos);

    auto duplicate = fix.load("format: 1\nscene: s\nentities:\n"
                              "  - entity: E\n"
                              "  - entity: E\n");
    REQUIRE(duplicate.error.has_value());
    CHECK(unwrap(duplicate.error).code == "loader.duplicate");

    // A machine file's own diagnostic surfaces verbatim (its file:line).
    REQUIRE_FALSE(base::write_file(fix.dir.file("bad.machine.yaml"),
                                   "format: 1\nmachine: m\nregions:\n  r:\n    initial: A\n"
                                   "    states:\n      A:\n        on:\n"
                                   "          - {event: death.dealt, goto: Ghost}\n",
                                   "t")
                      .has_value());
    auto nested = fix.load("format: 1\nscene: s\nevents: [combat.events.yaml]\nentities:\n"
                           "  - entity: E\n"
                           "    machines: [{instance: {path: bad.machine.yaml}}]\n");
    REQUIRE(nested.error.has_value());
    CHECK(unwrap(nested.error).code == "loader.bad_ref");
    CHECK(unwrap(nested.error).message.find("bad.machine.yaml:9:") != std::string::npos);
    CHECK(unwrap(nested.error).message.find("Ghost") != std::string::npos);
}

// loader.entity — `*.entity.yaml` prefab files (m1-scene-format): `base:`
// (every component generic), `machines:` (instance + override, the machine
// file itself always hard-required), `attachments:` (sockets — lenient
// asset resolution, brand new grammar).

#include "core/base/file_io.h"
#include "core/loader/entity_format.h"
#include "core/loader/loader.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

struct EntityFixture {
    testkit::TempDir dir{"loader-entity"};
    reflect::Registry registry;
    EventsDecl vocab;
    ComponentVocab components{.extracted = {"Health"}};

    EntityFixture() {
        reflect::register_builtin_events(registry);
        REQUIRE_FALSE(base::write_file(dir.file("brain.machine.yaml"),
                                       "format: 1\n"
                                       "machine: warden\n"
                                       "regions:\n"
                                       "  combat:\n"
                                       "    initial: Passive\n"
                                       "    states:\n"
                                       "      Passive: {}\n",
                                       "t")
                          .has_value());
    }

    EntityLoadResult load(const std::string& text, bool lenient) {
        const std::string path = dir.file("warden.entity.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "t").has_value());
        return load_entity_file(path, registry, vocab, components, lenient);
    }
};

} // namespace

TEST_CASE("loader.entity: base: + machines: + attachments: parse into EntityFile") {
    EntityFixture fix;
    EntityLoadResult loaded =
        fix.load("format: 1\n"
                 "entity: Warden\n"
                 "base:\n"
                 "  - Transform: {}\n"
                 "  - Health: {max: 120}\n"
                 "machines:\n"
                 "  - instance: {uid: \"m:brn7q2\", path: brain.machine.yaml}\n"
                 "    override:\n"
                 "      combat/Passive/sequence: {duration: 1.0}\n"
                 "attachments:\n"
                 "  - socket: grip\n"
                 "    of: {path: mace.model.yaml}\n",
                 /*lenient=*/true);
    EntityFile& entity = unwrap(loaded.entity);
    CHECK(entity.name.view() == "Warden");
    REQUIRE(entity.base_components.size() == 2);
    CHECK(entity.base_components[0].type.view() == "Transform");
    CHECK(entity.base_components[1].type.view() == "Health");

    REQUIRE(entity.machines.size() == 1);
    CHECK(entity.machines[0].instance_ref.has_uid);
    CHECK(entity.machines[0].instance_ref.uid == "m:brn7q2");
    REQUIRE(entity.machines[0].overrides.size() == 1);
    CHECK(entity.machines[0].overrides[0].path == "combat/Passive/sequence");
    REQUIRE(entity.machine_files.size() == 1);
    CHECK(entity.machine_files[0].desc.name.view() == "warden");

    REQUIRE(entity.attachments.size() == 1);
    CHECK(entity.attachments[0].socket.view() == "grip");
    // mace.model.yaml does not exist -> a Gap (lenient), never a refusal.
    REQUIRE(entity.gaps.size() == 1);
    CHECK(entity.gaps[0].kind == "model");
    CHECK(entity.gaps[0].what == "mace.model.yaml");
}

TEST_CASE("loader.entity: a missing model asset refuses by default (strict)") {
    EntityFixture fix;
    EntityLoadResult loaded = fix.load("format: 1\n"
                                       "entity: Warden\n"
                                       "attachments:\n"
                                       "  - socket: grip\n"
                                       "    of: {path: mace.model.yaml}\n",
                                       /*lenient=*/false);
    CHECK(unwrap(loaded.error).code == "loader.bad_ref");
}

TEST_CASE("loader.entity: a missing MACHINE file is ALWAYS a hard refusal, never a Gap") {
    EntityFixture fix;
    EntityLoadResult loaded = fix.load("format: 1\n"
                                       "entity: Warden\n"
                                       "machines:\n"
                                       "  - instance: {path: does_not_exist.machine.yaml}\n",
                                       /*lenient=*/true); // even lenient
    CHECK(unwrap(loaded.error).code == "loader.bad_ref");
}

TEST_CASE("loader.entity: an unknown base: component name refuses by default, Gaps when lenient") {
    EntityFixture fix;
    EntityLoadResult strict = fix.load(
        "format: 1\nentity: Warden\nbase:\n  - Perception: {fov: 110}\n", /*lenient=*/false);
    CHECK(unwrap(strict.error).code == "loader.unknown_key");

    EntityLoadResult lenient = fix.load(
        "format: 1\nentity: Warden\nbase:\n  - Perception: {fov: 110}\n", /*lenient=*/true);
    EntityFile& entity = unwrap(lenient.entity);
    REQUIRE(entity.gaps.size() == 1);
    CHECK(entity.gaps[0].kind == "component");
    CHECK(entity.gaps[0].what == "Perception");
}

TEST_CASE("loader.entity: attachments' nested entity: {prefab: <path>} is a bare path, lenient") {
    EntityFixture fix;
    EntityLoadResult loaded = fix.load("format: 1\n"
                                       "entity: Warden\n"
                                       "attachments:\n"
                                       "  - socket: grip\n"
                                       "    of: {path: mace.model.yaml}\n"
                                       "    entity: {prefab: mace.entity.yaml}\n",
                                       /*lenient=*/true);
    EntityFile& entity = unwrap(loaded.entity);
    REQUIRE(entity.attachments.size() == 1);
    const AssetRefDesc& nested = unwrap(entity.attachments[0].entity_prefab);
    CHECK(nested.path_authored == "mace.entity.yaml");
    CHECK_FALSE(nested.exists);
    REQUIRE(entity.gaps.size() == 2); // model AND entity, both unresolved
}

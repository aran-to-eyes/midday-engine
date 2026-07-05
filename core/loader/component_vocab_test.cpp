// loader.component_vocab — the component-NAME vocabulary (m1-scene-format):
// native M0 names always resolve; extracted TS names resolve when a
// project manifest is supplied; everything else stays unknown.

#include "core/base/file_io.h"
#include "core/loader/component_vocab.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

TEST_CASE("loader.component_vocab: native names always resolve") {
    CHECK(is_native_component("Transform"));
    CHECK(is_native_component("Collider"));
    CHECK(is_native_component("RigidBody"));
    CHECK_FALSE(is_native_component("Health"));

    const ComponentVocab empty;
    CHECK(empty.known("Transform"));
    CHECK_FALSE(empty.known("Health"));
    CHECK_FALSE(empty.known("Perception"));
}

TEST_CASE("loader.component_vocab: an empty manifest path is an empty (native-only) vocab") {
    ComponentVocabLoadResult result = load_component_vocab("");
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.vocab.extracted.empty());
    CHECK(result.vocab.known("Collider"));
}

TEST_CASE("loader.component_vocab: extracted names from a project manifest resolve") {
    testkit::TempDir dir{"loader-component-vocab"};
    const std::string path = dir.file("warden.components.json");
    REQUIRE_FALSE(base::write_file(path,
                                   R"({"format_version":1,"components":)"
                                   R"([{"name":"Health"},{"name":"DamageOnTouch"}]})",
                                   "test.io")
                      .has_value());
    ComponentVocabLoadResult result = load_component_vocab(path);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.vocab.known("Health"));
    CHECK(result.vocab.known("DamageOnTouch"));
    CHECK(result.vocab.known("Transform")); // native still resolves
    CHECK_FALSE(result.vocab.known("Perception"));
}

TEST_CASE("loader.component_vocab: a missing manifest file is a loader.io error") {
    ComponentVocabLoadResult result = load_component_vocab("/nonexistent/does_not_exist.json");
    CHECK(unwrap(result.error).code == "loader.io");
}

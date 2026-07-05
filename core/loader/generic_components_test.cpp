// loader.generic_components — the shared `{Name: {field: value, ...}}`
// list engine (m1-scene-format): unknown component names hard-refuse by
// default and become a Gap only in lenient mode; duplicates always refuse.

#include "core/loader/entity_format.h"
#include "core/loader/generic_components.h"
#include "core/loader/yaml.h"
#include "core/statechart/machine_desc.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

YamlNode parse(const std::string& text) {
    YamlParseResult result = parse_yaml(text, "test");
    REQUIRE_FALSE(result.error.has_value());
    return result.root;
}

} // namespace

TEST_CASE("loader.generic_components: a known name parses fields verbatim") {
    const YamlNode node = parse("- Health: {max: 120, value: 120}\n");
    const ComponentVocab vocab{.extracted = {"Health"}};
    GenericComponentsResult<GenericComponentEntry> result =
        parse_generic_components<GenericComponentEntry>(node, "test", vocab, false);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.gaps.empty());
    REQUIRE(result.components.size() == 1);
    CHECK(result.components[0].type.view() == "Health");
    const base::Json* max = result.components[0].fields.find("max");
    REQUIRE(max != nullptr);
    CHECK(max->as_int() == 120);
}

TEST_CASE("loader.generic_components: an unknown name refuses by default") {
    const YamlNode node = parse("- Perception: {fov: 110}\n");
    const ComponentVocab vocab;
    GenericComponentsResult<GenericComponentEntry> result =
        parse_generic_components<GenericComponentEntry>(node, "test", vocab, false);
    CHECK(unwrap(result.error).code == "loader.unknown_key");
}

TEST_CASE("loader.generic_components: lenient mode reports a Gap and keeps the field data") {
    const YamlNode node = parse("- Perception: {fov: 110}\n");
    const ComponentVocab vocab;
    GenericComponentsResult<statechart::StateComponentDesc> result =
        parse_generic_components<statechart::StateComponentDesc>(
            node, "warden.machine.yaml", vocab, true);
    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.gaps.size() == 1);
    CHECK(result.gaps[0].kind == "component");
    CHECK(result.gaps[0].what == "Perception");
    CHECK(result.gaps[0].file == "warden.machine.yaml");
    REQUIRE(result.components.size() == 1);
    CHECK(result.components[0].type.view() == "Perception");
    const base::Json* fov = result.components[0].fields.find("fov");
    REQUIRE(fov != nullptr);
    CHECK(fov->as_int() == 110);
}

TEST_CASE("loader.generic_components: a duplicate name always refuses") {
    const YamlNode node = parse("- Health: {}\n- Health: {}\n");
    const ComponentVocab vocab{.extracted = {"Health"}};
    GenericComponentsResult<GenericComponentEntry> result =
        parse_generic_components<GenericComponentEntry>(node, "test", vocab, true);
    CHECK(unwrap(result.error).code == "loader.duplicate");
}

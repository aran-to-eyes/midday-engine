// loader.yaml_emit — the canonical emitter: block-only rendering, preserved
// quoting semantics, and (the exit-test claim) idempotence — emit(parse(x))
// reparsed and re-emitted reproduces the SAME bytes, proven here at the unit
// level for flow-collapsing, quoting, null, and defensive key-quoting cases
// (the CLI-level proof over real format fixtures rides scripts/verify.sh).

#include "core/loader/yaml.h"
#include "core/loader/yaml_emit.h"
#include "testkit/doctest.h"

#include <string>

using namespace midday;
using namespace midday::loader;

namespace {

std::string roundtrip(const std::string& text) {
    YamlParseResult parsed = parse_yaml(text, "t.yaml");
    REQUIRE_FALSE(parsed.error.has_value());
    return emit_yaml(parsed.root);
}

} // namespace

TEST_CASE("loader.yaml_emit: flow collections collapse to block, empty stays inline") {
    const std::string canonical = roundtrip("a: {x: 1, y: [2, 3]}\nb: []\nc: {}\n");
    CHECK(canonical == "a:\n"
                       "  x: 1\n"
                       "  y:\n"
                       "    - 2\n"
                       "    - 3\n"
                       "b: []\n"
                       "c: {}\n");
}

TEST_CASE("loader.yaml_emit: sequence of maps indents under the dash") {
    const std::string canonical = roundtrip("entities:\n"
                                            "  - entity: Ground\n"
                                            "    components: [{Transform: {}}]\n");
    CHECK(canonical == "entities:\n"
                       "  - entity: Ground\n"
                       "    components:\n"
                       "      - Transform: {}\n");
}

TEST_CASE("loader.yaml_emit: quoting is preserved, never guessed") {
    const std::string canonical = roundtrip("a: 42\nb: \"42\"\nc: true\nd: \"true\"\ne: \"\"\n");
    CHECK(canonical == "a: 42\n"
                       "b: \"42\"\n"
                       "c: true\n"
                       "d: \"true\"\n"
                       "e: \"\"\n");
}

TEST_CASE("loader.yaml_emit: null values render as a bare key or dash") {
    const std::string canonical = roundtrip("a:\nb:\n  - 1\n  -\n  - 3\n");
    CHECK(canonical == "a:\n"
                       "b:\n"
                       "  - 1\n"
                       "  -\n"
                       "  - 3\n");
}

TEST_CASE("loader.yaml_emit: an embedded newline forces double-quoted output") {
    YamlNode root;
    root.kind = YamlNode::Kind::kMap;
    YamlEntry entry;
    entry.key = "doc";
    YamlNode value;
    value.kind = YamlNode::Kind::kScalar;
    value.scalar = "line one\nline two";
    value.quoted = false; // as if authored as a block-literal scalar
    entry.value.push_back(value);
    root.map.push_back(entry);

    const std::string canonical = emit_yaml(root);
    CHECK(canonical == "doc: \"line one\\nline two\"\n");
    // Reparsing recovers the exact original text.
    YamlParseResult parsed = parse_yaml(canonical, "t.yaml");
    REQUIRE_FALSE(parsed.error.has_value());
    CHECK(parsed.root.find("doc")->scalar == "line one\nline two");
}

TEST_CASE("loader.yaml_emit: an unsafe key is defensively quoted") {
    YamlNode root;
    root.kind = YamlNode::Kind::kMap;
    YamlEntry entry;
    entry.key = "a: b"; // would break plain-scalar key grammar unquoted
    YamlNode value;
    value.kind = YamlNode::Kind::kScalar;
    value.scalar = "1";
    entry.value.push_back(value);
    root.map.push_back(entry);

    const std::string canonical = emit_yaml(root);
    CHECK(canonical == "\"a: b\": 1\n");
    YamlParseResult parsed = parse_yaml(canonical, "t.yaml");
    REQUIRE_FALSE(parsed.error.has_value());
    CHECK(parsed.root.map.front().key == "a: b");
}

TEST_CASE("loader.yaml_emit: idempotence — emit(parse(emit(x))) == emit(x)") {
    const std::string sources[] = {
        "format: 1\nscene: arena\nentities:\n  - entity: Ground\n"
        "    components: [{Transform: {}}, {Collider: {shape: plane}}]\n",
        "format: 1\nevents:\n  death.dealt: {payload: {by: entity_ref}, doc: lethal damage}\n"
        "keys: [squad]\n",
        "a: {}\nb: []\nc: [1, 2, 3]\nd:\n  - {x: 1}\n  - {y: [1, 2]}\n",
    };
    for (const std::string& source : sources) {
        const std::string once = roundtrip(source);
        const std::string twice = roundtrip(once);
        CHECK(once == twice);
    }
}

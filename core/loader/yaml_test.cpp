// loader.yaml — the strict-YAML wrapper: owned node model, 1-based
// locations on nodes and keys, raw-text scalars (no implicit typing), and
// the strict-subset refusals (duplicate keys, anchors/aliases/tags,
// multi-doc), each with file:line:col. Locations here are PINNED numbers:
// the loader's whole diagnostic story stands on them.

#include "core/loader/yaml.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <string>

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

const YamlNode& get(const YamlNode& map, const char* key) {
    const YamlNode* found = map.find(key);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("loader.yaml: block + flow structures parse with pinned locations") {
    const std::string text = "scene: arena\n"                                  // line 1
                             "entities:\n"                                     // line 2
                             "  - entity: Ground\n"                            // line 3
                             "    components:\n"                               // line 4
                             "      - Transform: {}\n"                         // line 5
                             "  - entity: Boss\n"                              // line 6
                             "    at: [6, 0, -2.5]\n"                          // line 7
                             "    tags: {boss: true, name: \"The Warden\"}\n"; // line 8
    YamlParseResult parsed = parse_yaml(text, "test.yaml");
    REQUIRE_FALSE(parsed.error.has_value());
    const YamlNode& root = parsed.root;
    REQUIRE(root.is_map());
    REQUIRE(root.map.size() == 2);

    CHECK(root.map[0].key == "scene");
    CHECK(root.map[0].key_line == 1);
    CHECK(root.map[0].key_col == 1);
    CHECK(get(root, "scene").is_scalar());
    CHECK(get(root, "scene").scalar == "arena");
    CHECK_FALSE(get(root, "scene").quoted);
    CHECK(get(root, "scene").line == 1);
    CHECK(get(root, "scene").col == 8);

    const YamlNode& entities = get(root, "entities");
    REQUIRE(entities.is_seq());
    REQUIRE(entities.seq.size() == 2);

    const YamlNode& ground = entities.seq[0];
    REQUIRE(ground.is_map());
    CHECK(ground.map[0].key == "entity");
    CHECK(ground.map[0].key_line == 3);
    CHECK(ground.map[0].key_col == 5);
    const YamlNode& components = get(ground, "components");
    REQUIRE(components.is_seq());
    REQUIRE(components.seq.size() == 1);
    REQUIRE(components.seq[0].is_map());
    CHECK(components.seq[0].map[0].key == "Transform");
    // "Transform: {}" — the empty flow map is a MAP node, not null.
    CHECK(components.seq[0].map[0].node().is_map());
    CHECK(components.seq[0].map[0].node().map.empty());

    const YamlNode& boss = entities.seq[1];
    const YamlNode& at = get(boss, "at");
    REQUIRE(at.is_seq());
    REQUIRE(at.seq.size() == 3);
    CHECK(at.seq[0].scalar == "6");
    CHECK(at.seq[2].scalar == "-2.5");
    // Container nodes locate at the entry's first token (the key); scalar
    // nodes locate at the value text itself (see "scene" above).
    CHECK(at.line == 7);
    CHECK(at.col == 5);

    const YamlNode& tags = get(boss, "tags");
    CHECK(get(tags, "boss").scalar == "true");
    CHECK_FALSE(get(tags, "boss").quoted);
    CHECK(get(tags, "name").scalar == "The Warden");
    CHECK(get(tags, "name").quoted); // quoted = always a string
}

TEST_CASE("loader.yaml: empty values are null nodes; spelled scalars stay raw text") {
    YamlParseResult parsed = parse_yaml("a:\nb: null\nc: ~\nd: \"\"\n", "test.yaml");
    REQUIRE_FALSE(parsed.error.has_value());
    CHECK(get(parsed.root, "a").is_null());
    // No implicit typing: the literal spellings survive as scalar text and
    // the consumer decides (a field wanting a number refuses them loudly).
    CHECK(get(parsed.root, "b").is_scalar());
    CHECK(get(parsed.root, "b").scalar == "null");
    CHECK(get(parsed.root, "c").is_scalar());
    CHECK(get(parsed.root, "c").scalar == "~");
    CHECK(get(parsed.root, "d").is_scalar());
    CHECK(get(parsed.root, "d").scalar.empty());
    CHECK(get(parsed.root, "d").quoted);
}

TEST_CASE("loader.yaml: empty and comments-only documents parse to a null root") {
    CHECK(parse_yaml("", "e.yaml").root.is_null());
    CHECK_FALSE(parse_yaml("", "e.yaml").error.has_value());
    CHECK(parse_yaml("# just a comment\n", "e.yaml").root.is_null());
}

TEST_CASE("loader.yaml: malformed YAML refuses with file:line:col") {
    YamlParseResult parsed = parse_yaml("a: [1, 2\nb: 3\n", "bad.yaml");
    REQUIRE(parsed.error.has_value());
    CHECK(unwrap(parsed.error).code == "yaml.parse");
    CHECK(unwrap(parsed.error).message.find("bad.yaml:") == 0);
    REQUIRE(unwrap(parsed.error).details.find("line") != nullptr);
    CHECK(unwrap(parsed.error).details.find("line")->as_int() > 0);
    CHECK(unwrap(parsed.error).details.find("file")->as_string() == "bad.yaml");
}

TEST_CASE("loader.yaml: duplicate keys refuse at the second key's location") {
    YamlParseResult parsed = parse_yaml("a: 1\nb: 2\na: 3\n", "dup.yaml");
    REQUIRE(parsed.error.has_value());
    CHECK(unwrap(parsed.error).code == "yaml.strict");
    CHECK(unwrap(parsed.error).message == "dup.yaml:3:1: duplicate key 'a'");
}

TEST_CASE("loader.yaml: anchors, aliases, and tags refuse") {
    YamlParseResult anchored = parse_yaml("a: &x 1\nb: 2\n", "s.yaml");
    REQUIRE(anchored.error.has_value());
    CHECK(unwrap(anchored.error).code == "yaml.strict");
    CHECK(unwrap(anchored.error).message.find("anchors") != std::string::npos);

    YamlParseResult aliased = parse_yaml("a: &x 1\nb: *x\n", "s.yaml");
    REQUIRE(aliased.error.has_value());
    CHECK(unwrap(aliased.error).code == "yaml.strict");

    YamlParseResult tagged = parse_yaml("a: !!str 1\n", "s.yaml");
    REQUIRE(tagged.error.has_value());
    CHECK(unwrap(tagged.error).code == "yaml.strict");
    CHECK(unwrap(tagged.error).message.find("tags") != std::string::npos);
}

TEST_CASE("loader.yaml: multi-document streams refuse; one leading --- unwraps") {
    YamlParseResult multi = parse_yaml("---\na: 1\n---\nb: 2\n", "m.yaml");
    REQUIRE(multi.error.has_value());
    CHECK(unwrap(multi.error).code == "yaml.strict");
    CHECK(unwrap(multi.error).message.find("multiple YAML documents") != std::string::npos);

    YamlParseResult single = parse_yaml("---\na: 1\n", "m.yaml");
    REQUIRE_FALSE(single.error.has_value());
    CHECK(get(single.root, "a").scalar == "1");
}

TEST_CASE("loader.yaml: missing file is a structured loader.io error") {
    YamlParseResult parsed = parse_yaml_file("no/such/file.yaml");
    REQUIRE(parsed.error.has_value());
    CHECK(unwrap(parsed.error).code == "loader.io");
}

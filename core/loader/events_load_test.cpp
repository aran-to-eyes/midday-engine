// loader.events — `*.events.yaml`: typed event declarations in canonical
// reflect TypeDesc spellings, declared group keys, and the strict refusals
// (format gate, unknown keys, unknown types, duplicates, built-in
// collisions, reserved key spellings) — each with file:line:col.

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <filesystem>
#include <optional>
#include <string>

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

// TempDir::file() returns a NATIVE path; load_project_events emits generic
// (forward-slash) paths so the merge order and diagnostics are byte-identical
// on every platform (Windows accepts '/' for I/O). Compare in that form.
std::string generic(const std::string& native) {
    return std::filesystem::path(native).generic_string();
}

struct EventsFixture {
    testkit::TempDir dir{"loader-events"};
    reflect::Registry registry;
    EventsDecl decl;

    EventsFixture() { reflect::register_builtin_events(registry); }

    std::optional<base::Error> load(const std::string& text) {
        const std::string path = dir.file("combat.events.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
        return load_events_file(path, registry, decl);
    }
};

} // namespace

TEST_CASE("loader.events: declarations, payload types, and group keys load") {
    EventsFixture fix;
    auto error = fix.load("format: 1\n"
                          "events:\n"
                          "  player.spotted: {payload: {player: entity_ref, distance: float}}\n"
                          "  death.dealt:\n"
                          "    doc: lethal damage landed\n"
                          "    payload: {by: entity_ref}\n"
                          "  attack.swoosh: {}\n"
                          "  boss.died: {payload: {at: vec3, scores: array<int>}}\n"
                          "keys: [squad]\n");
    REQUIRE_FALSE(error.has_value());
    CHECK(fix.decl.events.size() == 4);
    CHECK(fix.decl.has_event("player.spotted"));
    CHECK(fix.decl.has_event("attack.swoosh"));
    CHECK_FALSE(fix.decl.has_event("nope"));
    CHECK(fix.decl.has_group("squad"));
    CHECK_FALSE(fix.decl.has_group("global"));

    const EventDecl& spotted = fix.decl.events[0];
    REQUIRE(spotted.payload.size() == 2);
    CHECK(spotted.payload[0].name == "player");
    CHECK(spotted.payload[0].type.canonical() == "entity_ref");
    CHECK(spotted.payload[1].type.canonical() == "float");
    CHECK(fix.decl.events[1].doc == "lethal damage landed");
    CHECK(fix.decl.events[3].payload[1].type.canonical() == "array<int>");
}

TEST_CASE("loader.events: the format gate is mandatory and versioned") {
    EventsFixture fix;
    auto missing = fix.load("events: {a.b: {}}\n");
    REQUIRE(missing.has_value());
    CHECK(unwrap(missing).code == "loader.bad_format");

    auto future = fix.load("format: 2\nevents: {a.b: {}}\n");
    REQUIRE(future.has_value());
    CHECK(unwrap(future).code == "loader.bad_format");
    CHECK(unwrap(future).message.find("format 1") != std::string::npos);
}

TEST_CASE("loader.events: strict refusals carry file:line:col") {
    EventsFixture fix;
    auto unknown = fix.load("format: 1\nevents: {a.b: {}}\nbogus: 1\n");
    REQUIRE(unknown.has_value());
    CHECK(unwrap(unknown).code == "loader.unknown_key");
    CHECK(unwrap(unknown).message.find(":3:1: unknown key 'bogus'") != std::string::npos);
    CHECK(unwrap(unknown).details.find("line")->as_int() == 3);

    auto bad_type = fix.load("format: 1\n"
                             "events:\n"
                             "  a.c: {payload: {x: floaty}}\n");
    REQUIRE(bad_type.has_value());
    CHECK(unwrap(bad_type).code == "loader.bad_value");
    CHECK(unwrap(bad_type).details.find("line")->as_int() == 3);
    CHECK(unwrap(bad_type).message.find("unknown type 'floaty'") != std::string::npos);

    auto builtin = fix.load("format: 1\nevents: {contact.began: {}}\n");
    REQUIRE(builtin.has_value());
    CHECK(unwrap(builtin).code == "loader.duplicate");
    CHECK(unwrap(builtin).message.find("built-in vocabulary") != std::string::npos);

    auto reserved = fix.load("format: 1\nkeys: [self]\n");
    REQUIRE(reserved.has_value());
    CHECK(unwrap(reserved).code == "loader.bad_value");
    CHECK(unwrap(reserved).message.find("reserved key spelling") != std::string::npos);
}

TEST_CASE("loader.events: cross-file duplicates refuse (merged project vocabulary)") {
    EventsFixture fix;
    REQUIRE_FALSE(fix.load("format: 1\nevents: {a.b: {}}\nkeys: [squad]\n").has_value());
    auto duplicate_event = fix.load("format: 1\nevents: {a.b: {}}\n");
    REQUIRE(duplicate_event.has_value());
    CHECK(unwrap(duplicate_event).code == "loader.duplicate");

    auto duplicate_group = fix.load("format: 1\nkeys: [squad]\n");
    REQUIRE(duplicate_group.has_value());
    CHECK(unwrap(duplicate_group).code == "loader.duplicate");
}

TEST_CASE("loader.events: load_project_events merges every *.events.yaml under a root") {
    testkit::TempDir dir{"loader-project-events"};
    reflect::Registry registry;
    reflect::register_builtin_events(registry);
    REQUIRE_FALSE(base::write_file(dir.file("a.events.yaml"),
                                   "format: 1\nevents: {a.one: {}}\nkeys: [squad]\n",
                                   "test.io")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("b.events.yaml"),
                                   "format: 1\nevents: {b.two: {payload: {x: float}}}\n",
                                   "test.io")
                      .has_value());

    ProjectEventsResult merged = load_project_events(dir.path.string(), registry);
    REQUIRE_FALSE(merged.error.has_value());
    REQUIRE(merged.files.size() == 2);
    CHECK(merged.files[0] == generic(dir.file("a.events.yaml")));
    CHECK(merged.files[1] == generic(dir.file("b.events.yaml")));
    CHECK(merged.decl.events.size() == 2);
    CHECK(merged.decl.has_event("a.one"));
    CHECK(merged.decl.has_event("b.two"));
    CHECK(merged.decl.has_group("squad"));
}

TEST_CASE("loader.events: load_project_events refuses a same-name event across two files") {
    testkit::TempDir dir{"loader-project-events-dup"};
    reflect::Registry registry;
    reflect::register_builtin_events(registry);
    REQUIRE_FALSE(base::write_file(
                      dir.file("a.events.yaml"), "format: 1\nevents: {dup.name: {}}\n", "test.io")
                      .has_value());
    REQUIRE_FALSE(base::write_file(
                      dir.file("b.events.yaml"), "format: 1\nevents: {dup.name: {}}\n", "test.io")
                      .has_value());

    ProjectEventsResult merged = load_project_events(dir.path.string(), registry);
    REQUIRE(merged.error.has_value());
    CHECK(unwrap(merged.error).code == "loader.duplicate");
    CHECK(unwrap(merged.error).message.find(generic(dir.file("b.events.yaml"))) !=
          std::string::npos);
    CHECK(unwrap(merged.error).message.find("dup.name") != std::string::npos);
}

TEST_CASE("loader.events: load_project_events refuses a non-directory root") {
    testkit::TempDir dir{"loader-project-events-notdir"};
    const std::string not_a_dir = dir.file("plain.events.yaml");
    REQUIRE_FALSE(
        base::write_file(not_a_dir, "format: 1\nevents: {a.one: {}}\n", "test.io").has_value());

    reflect::Registry registry;
    reflect::register_builtin_events(registry);
    ProjectEventsResult result = load_project_events(not_a_dir, registry); // a FILE, not a dir
    REQUIRE(result.error.has_value());
    CHECK(unwrap(result.error).code == "loader.io");
}

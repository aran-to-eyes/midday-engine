// loader.machine_emit — the canonical machine-file serializer (m1-scene-
// format, exit-test #1/#3): `on:` desugars to `Transition:` in the
// canonical form, and reloading that canonical text reproduces the exact
// same MachineDesc — so a SECOND canonicalization is byte-identical to the
// first (load -> print -> load -> print, never a self-diff).

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/loader/machine_emit.h"
#include "core/loader/yaml_emit.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

struct MachineEmitFixture {
    testkit::TempDir dir{"loader-machine-emit"};
    reflect::Registry registry;
    EventsDecl vocab;
    ComponentVocab components{.extracted = {"NavFollow"}};

    MachineEmitFixture() {
        reflect::register_builtin_events(registry);
        for (const char* event : {"player.spotted", "player.lost", "player.inRange", "stagger.hit"})
            vocab.events.push_back({.name = event});
    }

    MachineLoadResult load(const std::string& text) {
        const std::string path = dir.file("warden.machine.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "t").has_value());
        return load_machine_file(path, dir.file(""), registry, vocab, components);
    }

    // Reload TEXT as a machine file at a FRESH path (simulating "the
    // printed form fed back in") and re-canonicalize.
    std::string reload_and_emit(const std::string& text) {
        const std::string path = dir.file("reload.machine.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "t").has_value());
        MachineLoadResult loaded =
            load_machine_file(path, dir.file(""), registry, vocab, components);
        MachineFile& machine = unwrap(loaded.machine);
        return emit_yaml(machine_to_yaml(machine));
    }
};

const char* kWardenLike = // clang-format off
    "format: 1\n"
    "machine: warden\n"
    "regions:\n"
    "  locomotion:\n"
    "    initial: Idle\n"
    "    states:\n"
    "      Idle:\n"
    "        on:\n"
    "          - {event: player.spotted, goto: Chase}\n"
    "      Chase:\n"
    "        components:\n"
    "          - NavFollow: {speed: 4.5, repathEvery: 0.25}\n"
    "        on:\n"
    "          - {event: player.lost, goto: Idle}\n"
    "  combat:\n"
    "    initial: Passive\n"
    "    states:\n"
    "      Passive:\n"
    "        on:\n"
    "          - {event: player.inRange, goto: SlashAttack}\n"
    "      SlashAttack:\n"
    "        initial: Windup\n"
    "        sequence: {duration: 1.2, end: hold}\n"
    "        on:\n"
    "          - {event: self.finished, goto: Passive}\n"
    "          - {event: stagger.hit, goto: Passive, priority: 10}\n"
    "        states:\n"
    "          Windup: {}\n"
    "          HitboxLive:\n"
    "            children:\n"
    "              - entity: Hurtbox\n"
    "                at: [0, 1.0, 1.2]\n"; // clang-format on

} // namespace

TEST_CASE("loader.machine_emit: on: desugars to Transition: in the canonical form") {
    MachineEmitFixture fix;
    MachineLoadResult loaded = fix.load(kWardenLike);
    MachineFile& machine = unwrap(loaded.machine);
    const std::string canonical = emit_yaml(machine_to_yaml(machine));
    CHECK(canonical.find("Transition:") != std::string::npos);
    CHECK(canonical.find("\non:") == std::string::npos);
    CHECK(canonical.find("  on:") == std::string::npos);
}

TEST_CASE("loader.machine_emit: round-trip (load -> print -> load -> print) is byte-stable") {
    MachineEmitFixture fix;
    MachineLoadResult loaded = fix.load(kWardenLike);
    MachineFile& machine = unwrap(loaded.machine);
    const std::string once = emit_yaml(machine_to_yaml(machine));
    const std::string twice = fix.reload_and_emit(once);
    CHECK(once == twice);
}

TEST_CASE("loader.machine_emit: defaults render explicitly (history, priority)") {
    MachineEmitFixture fix;
    MachineLoadResult loaded = fix.load(kWardenLike);
    MachineFile& machine = unwrap(loaded.machine);
    const std::string canonical = emit_yaml(machine_to_yaml(machine));
    // Every state/region carries an explicit history: (default false).
    CHECK(canonical.find("history: false") != std::string::npos);
    // Every pair carries an explicit priority: (default 0).
    CHECK(canonical.find("priority: 0") != std::string::npos);
}

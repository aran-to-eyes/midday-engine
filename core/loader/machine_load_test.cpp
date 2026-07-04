// loader.machine — `*.machine.yaml` -> the EXISTING MachineDesc aggregate:
// on: sugar -> TransitionDesc order, self.finished / then: canonicalization,
// any-state + priority + if:, sequences (trigger/span tracks), substates +
// initial, script refs, children under states, vars, channels from pair
// keys — plus the strict refusals: unknown key and bad state ref pinned
// with exact file:line:col (the node's exit-3 exit test rides on these).

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <optional>
#include <string>

using namespace midday;
using namespace midday::loader;
using midday::base::Name;
using midday::testkit::unwrap;

namespace {

struct MachineFixture {
    testkit::TempDir dir{"loader-machine"};
    reflect::Registry registry;
    EventsDecl vocab;

    MachineFixture() {
        reflect::register_builtin_events(registry);
        for (const char* event :
             {"player.spotted", "player.inRange", "death.dealt", "stagger.hit", "attack.swoosh"})
            vocab.events.push_back({.name = event}); // brace-init: paren-aggregate
                                                     // construct_at needs clang>=16
        vocab.group_keys.emplace_back("squad");
    }

    MachineLoadResult load(const std::string& text) {
        const std::string path = dir.file("boss.machine.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
        return load_machine_file(path, dir.file(""), registry, vocab);
    }
};

// The A.3-shaped machine, exercising every grammar feature at once.
const char* kBossMachine = // clang-format off
    "format: 1\n"
    "machine: boss\n"
    "vars: {health.current: float}\n"
    "regions:\n"
    "  locomotion:\n"
    "    initial: Idle\n"
    "    states:\n"
    "      Idle:\n"
    "        on:\n"
    "          - {event: player.spotted, key: self, goto: Chasing}\n"
    "      Chasing: {}\n"
    "  combat:\n"
    "    initial: Passive\n"
    "    anystate:\n"
    "      - {event: death.dealt, key: self, goto: Dead, priority: 100}\n"
    "    states:\n"
    "      Passive:\n"
    "        on:\n"
    "          - {event: player.inRange, goto: SlashAttack, if: 'health.current > 0'}\n"
    "      SlashAttack:\n"
    "        script: scripts/slash_attack.ts\n"
    "        initial: Windup\n"
    "        sequence:\n"
    "          duration: 1.2\n"
    "          tracks:\n"
    "            - trigger: [{t: 0.30, event: attack.swoosh}]\n"
    "            - span: {name: HitboxLive, from: 0.40, to: 0.80}\n"
    "        on:\n"
    "          - {event: self.finished, goto: Passive}\n"
    "          - {event: stagger.hit, key: squad, goto: Staggered, priority: 10}\n"
    "          - {event: HitboxLive.opened, goto: HitboxLive}\n"
    "          - {event: HitboxLive.closed, goto: Windup}\n"
    "        states:\n"
    "          Windup: {}\n"
    "          HitboxLive:\n"
    "            children:\n"
    "              - {entity: Hurtbox, at: [0, 1.0, 1.2]}\n"
    "      Staggered:\n"
    "        sequence:\n"
    "          duration: 0.9\n"
    "          end: hold\n"
    "      Dead:\n"
    "        script: scripts/dead.ts\n"; // clang-format on

} // namespace

TEST_CASE("loader.machine: the A.3-shaped machine loads into MachineDesc verbatim") {
    MachineFixture fix;
    std::filesystem::create_directories(fix.dir.path / "scripts");
    REQUIRE_FALSE(base::write_file(fix.dir.file("scripts/slash_attack.ts"), "//", "t").has_value());
    REQUIRE_FALSE(base::write_file(fix.dir.file("scripts/dead.ts"), "//", "t").has_value());

    MachineLoadResult loaded = fix.load(kBossMachine);
    REQUIRE_FALSE(loaded.error.has_value());
    const statechart::MachineDesc& desc = unwrap(loaded.machine).desc;
    CHECK(desc.name == Name("boss"));

    REQUIRE(desc.vars.size() == 1);
    CHECK(desc.vars[0].name == "health.current");
    CHECK(desc.vars[0].type == expr::ValueType::kFloat);

    // Pair `key:` channels: squad joined the machine's named channels; self
    // never does (the host subscription is always on).
    REQUIRE(desc.channels.size() == 1);
    CHECK(desc.channels[0] == Name("squad"));

    REQUIRE(desc.regions.size() == 2);
    const statechart::RegionDesc& locomotion = desc.regions[0];
    CHECK(locomotion.name == Name("locomotion"));
    CHECK(locomotion.initial == Name("Idle"));
    REQUIRE(locomotion.states.size() == 2);
    REQUIRE(locomotion.states[0].transitions.size() == 1);
    CHECK(locomotion.states[0].transitions[0].event == Name("player.spotted"));
    CHECK(locomotion.states[0].transitions[0].target == Name("Chasing"));

    const statechart::RegionDesc& combat = desc.regions[1];
    REQUIRE(combat.any_state.size() == 1);
    CHECK(combat.any_state[0].event == Name("death.dealt"));
    CHECK(combat.any_state[0].priority == 100);

    REQUIRE(combat.states.size() == 4);
    const statechart::StateDesc& passive = combat.states[0];
    REQUIRE(passive.transitions.size() == 1);
    CHECK(passive.transitions[0].condition == "health.current > 0");

    const statechart::StateDesc& slash = combat.states[1];
    CHECK(slash.name == Name("SlashAttack"));
    CHECK(slash.initial == Name("Windup"));
    REQUIRE(slash.substates.size() == 2);
    REQUIRE(slash.transitions.size() == 4);
    // self.finished canonicalized to the owning state's per-state event.
    CHECK(slash.transitions[0].event == Name("SlashAttack.finished"));
    CHECK(slash.transitions[0].target == Name("Passive"));
    CHECK(slash.transitions[1].priority == 10);
    CHECK(slash.transitions[2].event == Name("HitboxLive.opened"));

    REQUIRE(slash.sequence.has_value());
    CHECK(unwrap(slash.sequence).duration == 1.2);
    CHECK(unwrap(slash.sequence).end == statechart::SequenceEnd::kFinish);
    REQUIRE(unwrap(slash.sequence).triggers.size() == 1);
    CHECK(unwrap(slash.sequence).triggers[0].time == 0.30);
    CHECK(unwrap(slash.sequence).triggers[0].event == Name("attack.swoosh"));
    REQUIRE(unwrap(slash.sequence).spans.size() == 1);
    CHECK(unwrap(slash.sequence).spans[0].name == Name("HitboxLive"));
    CHECK(unwrap(slash.sequence).spans[0].begin == 0.40);
    CHECK(unwrap(slash.sequence).spans[0].end == 0.80);

    CHECK(unwrap(combat.states[2].sequence).end == statechart::SequenceEnd::kHold);

    // Loader extras: script seats and children under states.
    REQUIRE(unwrap(loaded.machine).scripts.size() == 2);
    CHECK(unwrap(loaded.machine).scripts[0].state == Name("SlashAttack"));
    CHECK(unwrap(loaded.machine).scripts[0].ref == "scripts/slash_attack.ts");
    CHECK(unwrap(loaded.machine).scripts[1].state == Name("Dead"));
    REQUIRE(unwrap(loaded.machine).children.size() == 1);
    CHECK(unwrap(loaded.machine).children[0].region == Name("combat"));
    CHECK(unwrap(loaded.machine).children[0].state == Name("HitboxLive"));
    REQUIRE(unwrap(loaded.machine).children[0].children.size() == 1);
    CHECK(unwrap(loaded.machine).children[0].children[0].entity == Name("Hurtbox"));
    CHECK(unwrap(loaded.machine).children[0].children[0].at.translation.z == 1.2F);
}

TEST_CASE("loader.machine: then: sugar appends the finished-pair") {
    MachineFixture fix;
    MachineLoadResult loaded = fix.load("format: 1\n"
                                        "machine: m\n"
                                        "regions:\n"
                                        "  r:\n"
                                        "    initial: A\n"
                                        "    states:\n"
                                        "      A:\n"
                                        "        sequence: {duration: 0.5, then: B}\n"
                                        "      B: {}\n");
    REQUIRE_FALSE(loaded.error.has_value());
    const statechart::StateDesc& state = unwrap(loaded.machine).desc.regions[0].states[0];
    REQUIRE(state.transitions.size() == 1);
    CHECK(state.transitions[0].event == Name("A.finished"));
    CHECK(state.transitions[0].target == Name("B"));
}

TEST_CASE("loader.machine: unknown key refuses with exact file:line:col") {
    MachineFixture fix;
    MachineLoadResult loaded = fix.load("format: 1\n"
                                        "machine: m\n"
                                        "regions:\n"
                                        "  r:\n"
                                        "    initial: A\n"
                                        "    states:\n"
                                        "      A:\n"
                                        "        onn:\n" // line 8, col 9
                                        "          - {event: death.dealt, goto: A}\n");
    REQUIRE(loaded.error.has_value());
    CHECK(unwrap(loaded.error).code == "loader.unknown_key");
    CHECK(unwrap(loaded.error).message.find(":8:9: unknown key 'onn'") != std::string::npos);
    CHECK(unwrap(loaded.error).details.find("line")->as_int() == 8);
    CHECK(unwrap(loaded.error).details.find("col")->as_int() == 9);
}

TEST_CASE("loader.machine: bad goto target refuses with exact file:line:col") {
    MachineFixture fix;
    MachineLoadResult loaded = fix.load("format: 1\n"
                                        "machine: m\n"
                                        "regions:\n"
                                        "  r:\n"
                                        "    initial: A\n"
                                        "    states:\n"
                                        "      A:\n"
                                        "        on:\n"
                                        "          - {event: death.dealt, goto: Ghost}\n");
    REQUIRE(loaded.error.has_value());
    CHECK(unwrap(loaded.error).code == "loader.bad_ref");
    CHECK(unwrap(loaded.error).message.find("goto target 'Ghost' is not a state of region 'r'") !=
          std::string::npos);
    CHECK(unwrap(loaded.error).details.find("line")->as_int() == 9);
}

TEST_CASE("loader.machine: reference validation refuses bad initials, events, keys, filters") {
    MachineFixture fix;
    auto initial_missing = fix.load("format: 1\nmachine: m\nregions:\n  r:\n    states: {A: {}}\n");
    REQUIRE(initial_missing.error.has_value());
    CHECK(unwrap(initial_missing.error).code == "loader.bad_value");

    auto initial_bad =
        fix.load("format: 1\nmachine: m\nregions:\n  r:\n    initial: Z\n    states: {A: {}}\n");
    REQUIRE(initial_bad.error.has_value());
    CHECK(unwrap(initial_bad.error).code == "loader.bad_ref");
    CHECK(unwrap(initial_bad.error).message.find("top-level state") != std::string::npos);

    auto unknown_event = fix.load("format: 1\nmachine: m\nregions:\n  r:\n    initial: A\n"
                                  "    states:\n      A:\n        on:\n"
                                  "          - {event: no.such, goto: A}\n");
    REQUIRE(unknown_event.error.has_value());
    CHECK(unwrap(unknown_event.error).code == "loader.bad_ref");
    CHECK(unwrap(unknown_event.error).message.find("unknown event 'no.such'") != std::string::npos);

    auto derived_ok = fix.load("format: 1\nmachine: m\nregions:\n  r:\n    initial: A\n"
                               "    states:\n      A:\n        on:\n"
                               "          - {event: B.finished, goto: B}\n      B: {}\n");
    CHECK_FALSE(derived_ok.error.has_value());

    auto bad_key = fix.load("format: 1\nmachine: m\nregions:\n  r:\n    initial: A\n"
                            "    states:\n      A:\n        on:\n"
                            "          - {event: death.dealt, key: platoon, goto: A}\n");
    REQUIRE(bad_key.error.has_value());
    CHECK(unwrap(bad_key.error).code == "loader.bad_ref");
    CHECK(unwrap(bad_key.error).message.find("unknown key 'platoon'") != std::string::npos);

    auto bad_filter = fix.load("format: 1\nmachine: m\nvars: {hp: float}\nregions:\n  r:\n"
                               "    initial: A\n    states:\n      A:\n        on:\n"
                               "          - {event: death.dealt, goto: A, if: 'hp <=> 3'}\n");
    REQUIRE(bad_filter.error.has_value());
    CHECK(unwrap(bad_filter.error).code == "loader.bad_value");
    CHECK(unwrap(bad_filter.error).message.find("does not compile") != std::string::npos);

    auto anystate_self = fix.load("format: 1\nmachine: m\nregions:\n  r:\n    initial: A\n"
                                  "    anystate:\n      - {event: self.finished, goto: A}\n"
                                  "    states: {A: {}}\n");
    REQUIRE(anystate_self.error.has_value());
    CHECK(unwrap(anystate_self.error).message.find("self.finished") != std::string::npos);

    auto missing_script = fix.load("format: 1\nmachine: m\nregions:\n  r:\n    initial: A\n"
                                   "    states:\n      A:\n        script: scripts/ghost.ts\n");
    REQUIRE(missing_script.error.has_value());
    CHECK(unwrap(missing_script.error).code == "loader.bad_ref");
    CHECK(unwrap(missing_script.error).message.find("not found") != std::string::npos);

    auto duplicate_state = fix.load("format: 1\nmachine: m\nregions:\n  r:\n    initial: A\n"
                                    "    states:\n      A:\n        initial: B\n"
                                    "        states: {B: {}}\n      B: {}\n");
    REQUIRE(duplicate_state.error.has_value());
    CHECK(unwrap(duplicate_state.error).code == "loader.duplicate");
    CHECK(unwrap(duplicate_state.error).message.find("region-unique") != std::string::npos);
}

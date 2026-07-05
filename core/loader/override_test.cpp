// loader.override — the property-diff override engine (m1-scene-format,
// exit-test #2: "the override path grammar resolves BY NAMES, never
// indices"). The reordering test below is the literal proof: two machine
// files describing the SAME states/children/components in a DIFFERENT
// declaration order resolve the identical override path to the identical
// target — an index-based walk would diverge the moment declaration order
// changes; this one cannot, because it never looks at a position, only a
// name, at every level of the walk.

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/loader/override.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

struct OverrideFixture {
    testkit::TempDir dir{"loader-override"};
    reflect::Registry registry;
    EventsDecl vocab;
    ComponentVocab components{.extracted = {"DamageOnTouch"}};

    OverrideFixture() {
        reflect::register_builtin_events(registry);
        vocab.events.push_back({.name = "player.inRange"});
    }

    // `state_order` picks which of SlashAttack's substates (Windup,
    // HitboxLive) is authored first, and which of HitboxLive's children
    // (Decoy, Hurtbox) is authored first — the SAME machine, reshuffled.
    MachineLoadResult load(bool swap_substates, bool swap_children) {
        const std::string hitbox_children =
            swap_children ? "                  - entity: Hurtbox\n"
                            "                    at: [0, 1.0, 1.2]\n"
                            "                    components:\n"
                            "                      - DamageOnTouch: {amount: 40, stagger: 8}\n"
                            "                  - entity: Decoy\n"
                            "                    at: [1, 0, 0]\n"
                          : "                  - entity: Decoy\n"
                            "                    at: [1, 0, 0]\n"
                            "                  - entity: Hurtbox\n"
                            "                    at: [0, 1.0, 1.2]\n"
                            "                    components:\n"
                            "                      - DamageOnTouch: {amount: 40, stagger: 8}\n";
        const std::string substates = swap_substates
                                          ? "              HitboxLive:\n"
                                            "                children:\n" +
                                                hitbox_children + "              Windup: {}\n"
                                          : "              Windup: {}\n"
                                            "              HitboxLive:\n"
                                            "                children:\n" +
                                                hitbox_children;
        const std::string text = // clang-format off
            "format: 1\n"
            "machine: warden\n"
            "regions:\n"
            "  combat:\n"
            "    initial: Passive\n"
            "    states:\n"
            "      Passive:\n"
            "        on: [{event: player.inRange, goto: SlashAttack}]\n"
            "      SlashAttack:\n"
            "        initial: Windup\n"
            "        sequence: {duration: 1.2, end: hold}\n"
            "        on: [{event: self.finished, goto: Passive}]\n"
            "        states:\n" + substates; // clang-format on
        const std::string path = dir.file("warden.machine.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
        return load_machine_file(path, dir.file(""), registry, vocab, components);
    }
};

const std::vector<OverrideEntry> kSampleOverrides = {
    OverrideEntry{.path = "combat/SlashAttack/sequence",
                  .diff =
                      [] {
                          base::Json diff = base::Json::object();
                          diff.set("duration", 1.0);
                          return diff;
                      }()},
    OverrideEntry{.path = "combat/SlashAttack/HitboxLive/Hurtbox/DamageOnTouch",
                  .diff =
                      [] {
                          base::Json diff = base::Json::object();
                          diff.set("amount", static_cast<std::int64_t>(55));
                          return diff;
                      }()},
};

const statechart::StateDesc* find_state(const std::vector<statechart::StateDesc>& states,
                                        std::string_view name) {
    for (const statechart::StateDesc& state : states) {
        if (state.name.view() == name)
            return &state;
        if (const statechart::StateDesc* nested = find_state(state.substates, name))
            return nested;
    }
    return nullptr;
}

} // namespace

TEST_CASE("loader.override: resolves BY NAME, invariant under declaration reordering") {
    for (const bool swap_substates : {false, true}) {
        for (const bool swap_children : {false, true}) {
            OverrideFixture fix;
            MachineLoadResult loaded = fix.load(swap_substates, swap_children);
            MachineFile& machine = unwrap(loaded.machine);
            ApplyOverridesResult applied =
                apply_overrides(machine, kSampleOverrides, "arena.scene.yaml");
            REQUIRE_FALSE(applied.error.has_value());

            const statechart::RegionDesc& combat = applied.machine.desc.regions.front();
            const statechart::StateDesc* slash = find_state(combat.states, "SlashAttack");
            REQUIRE(slash != nullptr);
            REQUIRE(slash->sequence.has_value());
            CHECK(slash->sequence->duration == doctest::Approx(1.0));
            // Untouched sequence fields survive the diff (a shallow merge,
            // never a replace).
            CHECK(slash->sequence->end == statechart::SequenceEnd::kHold);

            bool found_child_override = false;
            for (const StateChildren& children : applied.machine.children) {
                if (children.state.view() != "HitboxLive")
                    continue;
                for (const StateChildDesc& child : children.children) {
                    if (child.entity.view() != "Hurtbox")
                        continue;
                    for (const GenericComponentEntry& component : child.components) {
                        if (component.type.view() != "DamageOnTouch")
                            continue;
                        const base::Json* amount = component.fields.find("amount");
                        REQUIRE(amount != nullptr);
                        CHECK(amount->as_int() == 55);
                        const base::Json* stagger = component.fields.find("stagger");
                        REQUIRE(stagger != nullptr);
                        CHECK(stagger->as_int() == 8); // survives the diff untouched
                        found_child_override = true;
                    }
                }
            }
            CHECK(found_child_override);
        }
    }
}

TEST_CASE("loader.override: a bad path is ALWAYS a hard refusal, never a Gap") {
    OverrideFixture fix;
    MachineLoadResult loaded = fix.load(false, false);
    MachineFile& machine = unwrap(loaded.machine);

    SUBCASE("unknown region") {
        std::vector<OverrideEntry> overrides = {OverrideEntry{.path = "nope/SlashAttack/sequence"}};
        ApplyOverridesResult applied = apply_overrides(machine, overrides, "f");
        CHECK(unwrap(applied.error).code == "loader.bad_ref");
    }
    SUBCASE("unknown top-level state") {
        std::vector<OverrideEntry> overrides = {OverrideEntry{.path = "combat/Nope/sequence"}};
        ApplyOverridesResult applied = apply_overrides(machine, overrides, "f");
        CHECK(unwrap(applied.error).code == "loader.bad_ref");
    }
    SUBCASE("no sequence on that state") {
        std::vector<OverrideEntry> overrides = {OverrideEntry{.path = "combat/Passive/sequence"}};
        ApplyOverridesResult applied = apply_overrides(machine, overrides, "f");
        CHECK(unwrap(applied.error).code == "loader.bad_ref");
    }
    SUBCASE("unknown child entity") {
        std::vector<OverrideEntry> overrides = {
            OverrideEntry{.path = "combat/SlashAttack/HitboxLive/Ghost/DamageOnTouch"}};
        ApplyOverridesResult applied = apply_overrides(machine, overrides, "f");
        CHECK(unwrap(applied.error).code == "loader.bad_ref");
    }
    SUBCASE("unknown component on a real child") {
        std::vector<OverrideEntry> overrides = {
            OverrideEntry{.path = "combat/SlashAttack/HitboxLive/Hurtbox/Nope"}};
        ApplyOverridesResult applied = apply_overrides(machine, overrides, "f");
        CHECK(unwrap(applied.error).code == "loader.bad_ref");
    }
}

TEST_CASE("loader.override: parse_override_block reads a {<path>: {diff}} mapping") {
    YamlParseResult parsed =
        parse_yaml("combat/SlashAttack/sequence: {duration: 1.0}\n"
                   "combat/SlashAttack/HitboxLive/Hurtbox/DamageOnTouch: {amount: 55}\n",
                   "test");
    REQUIRE_FALSE(parsed.error.has_value());
    OverrideParseResult result = parse_override_block(parsed.root, "test");
    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.entries.size() == 2);
    CHECK(result.entries[0].path == "combat/SlashAttack/sequence");
    CHECK(result.entries[0].line > 0);
    CHECK(result.entries[1].path == "combat/SlashAttack/HitboxLive/Hurtbox/DamageOnTouch");
}

TEST_CASE("loader.override: split_overrides_for_machine strips the machine-name segment") {
    std::vector<OverrideEntry> overrides = {
        OverrideEntry{.path = "warden/combat/SlashAttack/sequence"},
        OverrideEntry{.path = "player/locomotion/Walk/sequence"},
    };
    SplitOverrides split = split_overrides_for_machine(overrides, "warden");
    REQUIRE(split.matched.size() == 1);
    CHECK(split.matched[0].path == "combat/SlashAttack/sequence");
    REQUIRE(split.unmatched.size() == 1);
    CHECK(split.unmatched[0].path == "player/locomotion/Walk/sequence");
}

// ecs.determinism — THE m0-ecs-core exit fixture: a Philox-driven
// spawn/despawn/toggle script runs against two INDEPENDENT worlds (two
// runs diffed, never a self-diff — working-agreement rule 5) and the XXH3
// digest of every visit order must match. A third run appends net-zero
// toggle churn and must digest identically: iteration order is independent
// of toggle history by construction (bits, not moves).

#include "core/base/name.h"
#include "core/ecs/world.h"
#include "core/math/rng.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdint>
#include <vector>

using midday::base::Name;
using midday::ecs::EntityRef;
using midday::ecs::World;
using midday::math::RngStream;
using midday::reflect::ClassDesc;
using midday::reflect::Registry;

namespace {

struct Mass {
    std::uint32_t grams = 0;
};

struct Charge {
    std::uint32_t coulombs = 0;
};

ClassDesc component_class(const char* name) {
    ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

constexpr int kOps = 3000;
constexpr std::uint64_t kSeed = 0x00C0FFEE;

// One full independent run: fresh registry, world, and RNG stream; returns
// the XXH3-64 digest of every visit the standard view set makes.
std::uint64_t run_script(bool churn_toggles) {
    Registry registry;
    World world(registry);
    world.register_component<Mass>(component_class("Mass"));
    world.register_component<Charge>(component_class("Charge"));

    RngStream rng(kSeed);
    std::vector<EntityRef> live;

    for (int op = 0; op < kOps; ++op) {
        const std::uint32_t roll = rng.uniform_below(100);
        if (roll < 50 || live.empty()) {
            const EntityRef e = world.spawn();
            live.push_back(e);
            // Draws are hoisted into locals: C++ argument evaluation order
            // is unspecified, and the draw sequence must not depend on it.
            const std::uint32_t mass_value = rng.next_u32();
            const bool mass_active = rng.uniform_below(2) == 0;
            REQUIRE_FALSE(world.emplace(e, Mass{mass_value}, mass_active));
            if (rng.uniform_below(2) == 0) {
                const std::uint32_t charge_value = rng.next_u32();
                const bool charge_active = rng.uniform_below(2) == 0;
                REQUIRE_FALSE(world.emplace(e, Charge{charge_value}, charge_active));
            }
        } else if (roll < 75) {
            const std::uint32_t pick = rng.uniform_below(static_cast<std::uint32_t>(live.size()));
            REQUIRE_FALSE(world.despawn(live[pick]));
            live[pick] = live.back();
            live.pop_back();
        } else {
            const std::uint32_t pick = rng.uniform_below(static_cast<std::uint32_t>(live.size()));
            REQUIRE_FALSE(world.set_active<Mass>(live[pick], rng.uniform_below(2) == 0));
        }
    }

    if (churn_toggles) {
        // Net-zero churn: flip every live Mass row off and back to its
        // original bit. Must not perturb any visit order or membership.
        for (const EntityRef e : live) {
            // value_or is the analyzer-checked access; every live entity has
            // a Mass row, so the fallback arm is unreachable.
            const bool was = world.is_active<Mass>(e).value_or(false);
            REQUIRE_FALSE(world.set_active<Mass>(e, false));
            REQUIRE_FALSE(world.set_active<Mass>(e, true));
            REQUIRE_FALSE(world.set_active<Mass>(e, was));
        }
    }

    std::vector<std::uint64_t> visits;
    const auto record = [&visits](EntityRef ref, std::uint32_t value) {
        visits.push_back(ref.to_bits());
        visits.push_back(value);
    };
    world.view<Mass>().each([&](EntityRef ref, Mass& m) { record(ref, m.grams); });
    world.view<Mass>().include_inactive().each(
        [&](EntityRef ref, Mass& m) { record(ref, m.grams); });
    world.view<Charge>().each([&](EntityRef ref, Charge& c) { record(ref, c.coulombs); });
    world.view<Mass, Charge>().each(
        [&](EntityRef ref, Mass& m, Charge& c) { record(ref, m.grams + c.coulombs); });
    world.view<Mass, Charge>().include_inactive().each(
        [&](EntityRef ref, Mass& m, Charge& c) { record(ref, m.grams + c.coulombs); });

    return XXH3_64bits(visits.data(), visits.size() * sizeof(std::uint64_t));
}

} // namespace

TEST_CASE("ecs.determinism: two independent runs digest identically") {
    const std::uint64_t first = run_script(false);
    const std::uint64_t second = run_script(false);
    CHECK(first == second);
    MESSAGE("ecs visit-order digest = ", first);
}

TEST_CASE("ecs.determinism: iteration order is independent of toggle history") {
    const std::uint64_t clean = run_script(false);
    const std::uint64_t churned = run_script(true);
    CHECK(clean == churned);
}

// hierarchy.order — the deterministic depth-first total order: parent
// before children, siblings in attach order, roots in adoption order;
// reparenting one subtree preserves the relative order of every other
// entity; identical op scripts digest identically across two independent
// runs (core/hierarchy/tree_order.cpp).

#include "core/base/name.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

using midday::base::Name;
using midday::ecs::EntityRef;
using midday::ecs::World;
using midday::hierarchy::Hierarchy;
using midday::reflect::ClassDesc;
using midday::reflect::Registry;

namespace {

struct Marker {
    std::uint32_t value = 0;
};

ClassDesc component_class(const char* name) {
    ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

// All adopted entities in tree order, discovered via order_index sorting-free
// traversal: walk roots and children directly, asserting the cached indices
// agree with the structural DFS.
std::vector<EntityRef> tree_order_of(Hierarchy& hierarchy) {
    std::vector<EntityRef> out;
    std::vector<EntityRef> stack;
    for (EntityRef root = hierarchy.first_root(); !root.is_null();
         root = hierarchy.next_sibling_of(root)) {
        stack.push_back(root);
        while (!stack.empty()) {
            const EntityRef e = stack.back();
            stack.pop_back();
            out.push_back(e);
            std::vector<EntityRef> children;
            for (EntityRef c = hierarchy.first_child_of(e); !c.is_null();
                 c = hierarchy.next_sibling_of(c))
                children.push_back(c);
            for (const EntityRef child : std::ranges::reverse_view(children))
                stack.push_back(child);
        }
    }
    return out;
}

} // namespace

TEST_CASE("hierarchy.order: depth-first indices — parent first, siblings in attach order") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    // r0 -> (a -> (a1, a2), b); r1 -> (c)
    const EntityRef r0 = world.spawn();
    const EntityRef r1 = world.spawn();
    const EntityRef a = world.spawn();
    const EntityRef b = world.spawn();
    const EntityRef a1 = world.spawn();
    const EntityRef a2 = world.spawn();
    const EntityRef c = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(r0));
    REQUIRE_FALSE(hierarchy.adopt(r1));
    REQUIRE_FALSE(hierarchy.queue_attach(a, r0));
    REQUIRE_FALSE(hierarchy.queue_attach(b, r0));
    REQUIRE_FALSE(hierarchy.queue_attach(a1, a));
    REQUIRE_FALSE(hierarchy.queue_attach(a2, a));
    REQUIRE_FALSE(hierarchy.queue_attach(c, r1));
    REQUIRE_FALSE(world.flush_structural());

    const std::vector<EntityRef> expected = {r0, a, a1, a2, b, r1, c};
    CHECK(tree_order_of(hierarchy) == expected);
    for (std::uint32_t i = 0; i < expected.size(); ++i)
        CHECK(hierarchy.order_index(expected[i]) == i);
    CHECK(hierarchy.tree_less(a, a1));
    CHECK(hierarchy.tree_less(a2, b));
    CHECK(hierarchy.compare(c, c) == 0);

    // Total order extends over live UNADOPTED entities: after all adopted.
    const EntityRef stray = world.spawn();
    CHECK(hierarchy.order_index(stray) == std::nullopt);
    CHECK(hierarchy.tree_less(c, stray));
    const EntityRef stray2 = world.spawn();
    CHECK(hierarchy.compare(stray, stray2) == -1);
    CHECK(hierarchy.compare(stray2, stray) == 1);
}

TEST_CASE("hierarchy.order: mid-iteration queued reparent preserves order of other subtrees") {
    Registry registry;
    World world(registry);
    world.register_component<Marker>(component_class("Marker"));
    Hierarchy hierarchy(world);

    // Two independent subtrees plus a mover: r0 -> (a, b), r1 -> (c), m.
    const EntityRef r0 = world.spawn();
    const EntityRef r1 = world.spawn();
    const EntityRef a = world.spawn();
    const EntityRef b = world.spawn();
    const EntityRef c = world.spawn();
    const EntityRef m = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(r0));
    REQUIRE_FALSE(hierarchy.adopt(r1));
    REQUIRE_FALSE(hierarchy.adopt(m));
    REQUIRE_FALSE(hierarchy.queue_attach(a, r0));
    REQUIRE_FALSE(hierarchy.queue_attach(b, r0));
    REQUIRE_FALSE(hierarchy.queue_attach(c, r1));
    REQUIRE_FALSE(world.flush_structural());
    REQUIRE_FALSE(world.emplace(r0, Marker{0}));

    const std::vector<EntityRef> before = tree_order_of(hierarchy);
    CHECK(before == std::vector<EntityRef>{r0, a, b, r1, c, m});

    // THE exit fixture: queue the reparent mid-iteration; nothing moves
    // until the flush, and afterwards every entity OUTSIDE the moved
    // subtree keeps its relative order.
    world.view<Marker>().each([&](EntityRef, Marker&) {
        REQUIRE_FALSE(hierarchy.queue_attach(m, r1));
        CHECK(tree_order_of(hierarchy) == before); // applied at flush, not now
    });
    CHECK(tree_order_of(hierarchy) == before);
    REQUIRE_FALSE(world.flush_structural());

    const std::vector<EntityRef> after = tree_order_of(hierarchy);
    CHECK(after == std::vector<EntityRef>{r0, a, b, r1, c, m});

    // Move the whole r0 subtree under c: r1's line is untouched relative
    // order; r0/a/b stay contiguous and internally ordered.
    REQUIRE_FALSE(hierarchy.queue_attach(r0, c));
    REQUIRE_FALSE(world.flush_structural());
    CHECK(tree_order_of(hierarchy) == std::vector<EntityRef>{r1, c, r0, a, b, m});
}

TEST_CASE("hierarchy.order: identical op scripts produce identical orders (dual run)") {
    // Two INDEPENDENT worlds run the same topology script (working-agreement
    // rule 5: two runs diffed, never a self-diff); the XXH3 digest over
    // (slot index, order index) pairs must match bit-for-bit.
    auto run_script = []() -> std::uint64_t {
        Registry registry;
        World world(registry);
        Hierarchy hierarchy(world);
        std::vector<EntityRef> entities;
        entities.reserve(64);
        for (int i = 0; i < 64; ++i)
            entities.push_back(world.spawn());
        for (int i = 0; i < 8; ++i)
            REQUIRE_FALSE(hierarchy.adopt(entities[static_cast<std::size_t>(i)]));
        // Deterministic pseudo-script: attach i under (i % 8), then churn.
        for (int i = 8; i < 64; ++i)
            REQUIRE_FALSE(hierarchy.queue_attach(entities[static_cast<std::size_t>(i)],
                                                 entities[static_cast<std::size_t>(i % 8)]));
        REQUIRE_FALSE(world.flush_structural());
        for (int i = 8; i < 64; i += 5)
            REQUIRE_FALSE(hierarchy.queue_attach(entities[static_cast<std::size_t>(i)],
                                                 entities[static_cast<std::size_t>((i + 3) % 8)]));
        for (int i = 10; i < 64; i += 7)
            REQUIRE_FALSE(hierarchy.queue_detach(entities[static_cast<std::size_t>(i)]));
        REQUIRE_FALSE(world.flush_structural());

        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        for (const EntityRef e : entities) {
            const std::uint32_t pair[2] = {e.index, *hierarchy.order_index(e)};
            XXH3_64bits_update(&state, pair, sizeof(pair));
        }
        return XXH3_64bits_digest(&state);
    };

    const std::uint64_t first = run_script();
    const std::uint64_t second = run_script();
    CHECK(first == second);
}

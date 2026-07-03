// ecs.entity — generational handles and the slot table (core/ecs/entity.h):
// stale detection, deterministic LIFO slot reuse, the pending window, and
// the to_bits round trip journals will rely on.

#include "core/ecs/entity.h"
#include "doctest/doctest.h"

#include <cstdint>

using midday::ecs::EntityRef;
using midday::ecs::EntityTable;
using midday::ecs::SlotState;

TEST_CASE("ecs.entity: allocation hands out unique live slots") {
    EntityTable table;
    const EntityRef a = table.allocate(SlotState::kAlive);
    const EntityRef b = table.allocate(SlotState::kAlive);

    CHECK(a.index == 0);
    CHECK(b.index == 1);
    CHECK(a.generation == 0);
    CHECK(table.is_alive(a));
    CHECK(table.is_alive(b));
    CHECK(table.alive_count() == 2);
    CHECK_FALSE(a == b);
}

TEST_CASE("ecs.entity: release stales every outstanding ref, forever") {
    EntityTable table;
    const EntityRef a = table.allocate(SlotState::kAlive);
    table.release(a);

    CHECK_FALSE(table.is_alive(a));
    CHECK_FALSE(table.is_current(a));
    CHECK(table.alive_count() == 0);

    // The slot is reused with a bumped generation: the old ref stays stale.
    const EntityRef reborn = table.allocate(SlotState::kAlive);
    CHECK(reborn.index == a.index);
    CHECK(reborn.generation == a.generation + 1);
    CHECK(table.is_alive(reborn));
    CHECK_FALSE(table.is_alive(a));
}

TEST_CASE("ecs.entity: slot reuse is LIFO — a pure function of the op sequence") {
    EntityTable table;
    const EntityRef a = table.allocate(SlotState::kAlive);
    const EntityRef b = table.allocate(SlotState::kAlive);
    const EntityRef c = table.allocate(SlotState::kAlive);
    table.release(a);
    table.release(c);

    // Contract pinned: last freed, first reused (entity.h header).
    CHECK(table.allocate(SlotState::kAlive).index == c.index);
    CHECK(table.allocate(SlotState::kAlive).index == a.index);
    CHECK(table.is_alive(b));
}

TEST_CASE("ecs.entity: pending slots are current but not alive until made so") {
    EntityTable table;
    const EntityRef pending = table.allocate(SlotState::kPending);

    CHECK(table.is_current(pending));
    CHECK(table.is_pending(pending));
    CHECK_FALSE(table.is_alive(pending));
    CHECK(table.alive_count() == 0);

    table.make_alive(pending);
    CHECK(table.is_alive(pending));
    CHECK_FALSE(table.is_pending(pending));
    CHECK(table.alive_count() == 1);
}

TEST_CASE("ecs.entity: refs round-trip through their 64-bit journal form") {
    const EntityRef ref{7, 42};
    const std::uint64_t bits = ref.to_bits();

    CHECK(bits == ((std::uint64_t{42} << 32u) | 7u));
    CHECK(EntityRef::from_bits(bits) == ref);

    const EntityRef null_ref;
    CHECK(null_ref.is_null());
    CHECK(EntityRef::from_bits(null_ref.to_bits()).is_null());
}

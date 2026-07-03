// ecs.sparse — the paged sparse set (core/ecs/sparse_set.h): paging across
// the 4096-entry boundary, swap-and-pop dense order (pinned — it is the
// deterministic iteration order), and active bits that survive row moves.

#include "core/ecs/sparse_set.h"
#include "testkit/doctest.h"

#include <cstdint>
#include <vector>

using midday::ecs::kNpos;
using midday::ecs::kSparsePageSize;
using midday::ecs::SparseSet;

TEST_CASE("ecs.sparse: insert/find/remove across page boundaries") {
    SparseSet set;
    // Indices on pages 0, 0(last slot), 1, and 2 — far apart on purpose.
    const std::vector<std::uint32_t> indices = {
        0, kSparsePageSize - 1, kSparsePageSize, 2 * kSparsePageSize + 5};
    for (std::uint32_t i = 0; i < indices.size(); ++i)
        CHECK(set.insert(indices[i], true) == i);

    CHECK(set.size() == 4);
    for (std::uint32_t i = 0; i < indices.size(); ++i)
        CHECK(set.find(indices[i]) == i);

    // Absent indices: same page as a member, and a never-touched page.
    CHECK(set.find(1) == kNpos);
    CHECK(set.find(kSparsePageSize + 1) == kNpos);
    CHECK(set.find(7 * kSparsePageSize) == kNpos);
    CHECK_FALSE(set.contains(1));

    const SparseSet::RemoveResult removed = set.remove(kSparsePageSize - 1);
    CHECK(removed.dense_pos == 1);
    CHECK(removed.moved_last); // last row (page-2 index) swapped in
    CHECK(set.find(kSparsePageSize - 1) == kNpos);
    CHECK(set.find(2 * kSparsePageSize + 5) == 1);
    CHECK(set.size() == 3);
}

TEST_CASE("ecs.sparse: dense order is swap-and-pop, pinned exactly") {
    SparseSet set;
    for (std::uint32_t e : {10u, 20u, 30u, 40u, 50u})
        set.insert(e, true);

    set.remove(20); // 50 swaps into position 1
    CHECK(set.dense() == std::vector<std::uint32_t>{10, 50, 30, 40});

    set.remove(40); // last row: plain pop
    CHECK(set.dense() == std::vector<std::uint32_t>{10, 50, 30});

    set.insert(60, true); // append
    CHECK(set.dense() == std::vector<std::uint32_t>{10, 50, 30, 60});
}

TEST_CASE("ecs.sparse: active bits toggle in place and move with swapped rows") {
    SparseSet set;
    for (std::uint32_t e : {1u, 2u, 3u, 4u})
        set.insert(e, true);

    set.set_active(set.find(2), false);
    CHECK_FALSE(set.is_active(set.find(2)));
    CHECK(set.is_active(set.find(1)));

    // Remove row 0: the last row (entity 4, active) swaps into position 0
    // and keeps its bit; entity 2 stays inactive at its position.
    set.remove(1);
    CHECK(set.is_active(set.find(4)));
    CHECK_FALSE(set.is_active(set.find(2)));
    CHECK(set.is_active(set.find(3)));

    // Inactive rows carry cleared bits through swaps too: clear 4's bit,
    // remove entity 2 — entity 3's SET bit swaps into position 1 while
    // entity 4 keeps its cleared bit at position 0.
    set.set_active(set.find(4), false);
    set.remove(2);
    CHECK_FALSE(set.is_active(set.find(4)));
    CHECK(set.is_active(set.find(3)));
}

TEST_CASE("ecs.sparse: reused dense rows get fresh bits after removals") {
    SparseSet set;
    set.insert(100, false);
    set.remove(100);
    // Reinsert at the same dense position with the opposite activity: the
    // stale cleared bit must not leak through.
    set.insert(200, true);
    CHECK(set.is_active(set.find(200)));

    set.remove(200);
    set.insert(300, false);
    CHECK_FALSE(set.is_active(set.find(300)));
}

// core.arena.* — tests for the frame/arena allocator (core/base/arena.h).
// Determinism claim proven the contractual way: two independently driven
// arenas fed the same allocation sequence agree on every observable.

#include "core/base/arena.h"
#include "testkit/doctest.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using midday::base::Arena;

TEST_CASE("core.arena: allocations are aligned, disjoint, and hold their values") {
    Arena arena(256);
    auto* a = arena.create<std::uint32_t>(0xAAAAAAAAU);
    auto* b = arena.create<std::uint64_t>(0xBBBBBBBBBBBBBBBBULL);
    auto* c = arena.create<char>('c');
    auto* d = arena.create<double>(2.5);
    CHECK(reinterpret_cast<std::uintptr_t>(a) % alignof(std::uint32_t) == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % alignof(std::uint64_t) == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(d) % alignof(double) == 0);
    CHECK(*a == 0xAAAAAAAAU);
    CHECK(*b == 0xBBBBBBBBBBBBBBBBULL);
    CHECK(*c == 'c');
    CHECK(*d == 2.5);
}

TEST_CASE("core.arena: spans are value-initialized and disjoint") {
    Arena arena;
    auto ints = arena.allocate_span<std::int32_t>(16);
    REQUIRE(ints.size() == 16);
    for (std::int32_t v : ints)
        CHECK(v == 0); // value-initialized, never garbage
    auto more = arena.allocate_span<std::int32_t>(16);
    // Disjoint regions: writing one span never touches the other.
    ints[15] = 7;
    more[0] = 9;
    CHECK(ints[15] == 7);
    CHECK(more[0] == 9);
}

TEST_CASE("core.arena: reset retains blocks — steady-state frames allocate nothing") {
    Arena arena(128);
    for (int i = 0; i < 10; ++i)
        arena.create<std::uint64_t>(std::uint64_t(i));
    const std::size_t blocks_after_frame = arena.block_count();
    CHECK(blocks_after_frame >= 1);

    for (int frame = 0; frame < 5; ++frame) {
        arena.reset();
        CHECK(arena.bytes_used() == 0);
        for (int i = 0; i < 10; ++i)
            arena.create<std::uint64_t>(std::uint64_t(i));
        // Same sequence, same footprint: no new blocks appear after reset.
        CHECK(arena.block_count() == blocks_after_frame);
    }
}

TEST_CASE("core.arena: oversized requests get a dedicated block") {
    Arena arena(64);
    auto big = arena.allocate_span<std::byte>(1024);
    CHECK(big.size() == 1024);
    std::memset(big.data(), 0x5A, big.size());
    CHECK(std::to_integer<int>(big[1023]) == 0x5A);
    // The small lane still works alongside the oversized block.
    CHECK(*arena.create<int>(41) == 41);
}

TEST_CASE("core.arena: layout is a pure function of the allocation sequence") {
    // Two independent arenas (two "runs") driven identically must agree on
    // every observable: bytes used, block count, and intra-block offsets.
    auto drive = [](Arena& arena) {
        std::vector<std::ptrdiff_t> deltas;
        const char* prev = nullptr;
        for (int i = 0; i < 64; ++i) {
            auto* p = static_cast<char*>(arena.allocate((i % 13) + 1, (i % 2) != 0 ? 8 : 4));
            if (prev != nullptr && arena.block_count() == 1)
                deltas.push_back(p - prev);
            prev = p;
        }
        return deltas;
    };
    Arena run_a(4096);
    Arena run_b(4096);
    const auto deltas_a = drive(run_a);
    const auto deltas_b = drive(run_b);
    CHECK(deltas_a == deltas_b);
    CHECK(run_a.bytes_used() == run_b.bytes_used());
    CHECK(run_a.block_count() == run_b.block_count());
}

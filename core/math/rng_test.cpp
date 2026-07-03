// math.rng.* — Philox4x32-10 against the OFFICIAL Random123 known-answer
// vectors (the cross-platform lock), stream splitting, distributions, and
// the deterministic log underpinning the bit-portable normal().

#include "core/base/name.h"
#include "core/math/rng.h"
#include "testkit/doctest.h"

#include <cmath>
#include <cstdint>

using midday::base::Name;
using midday::math::det_log;
using midday::math::RngStream;

TEST_CASE("math.rng: Philox4x32-10 matches the official Random123 vectors") {
    using midday::math::detail::philox4x32_10;
    // Source: DEShawResearch/random123 tests/kat_vectors (philox4x32 10).
    const auto zero = philox4x32_10(0, 0, 0, 0, 0, 0);
    CHECK(zero.w[0] == 0x6627e8d5u);
    CHECK(zero.w[1] == 0xe169c58du);
    CHECK(zero.w[2] == 0xbc57ac4cu);
    CHECK(zero.w[3] == 0x9b00dbd8u);

    const auto ones =
        philox4x32_10(0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    CHECK(ones.w[0] == 0x408f276du);
    CHECK(ones.w[1] == 0x41c83b0eu);
    CHECK(ones.w[2] == 0xa20bc7c6u);
    CHECK(ones.w[3] == 0x6d5451fdu);

    const auto pi =
        philox4x32_10(0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u, 0xa4093822u, 0x299f31d0u);
    CHECK(pi.w[0] == 0xd16cfe09u);
    CHECK(pi.w[1] == 0x94fdccebu);
    CHECK(pi.w[2] == 0x5001e420u);
    CHECK(pi.w[3] == 0x24126ea1u);
}

TEST_CASE("math.rng: stream draws expose the Philox blocks in counter order") {
    // seed 0, stream 0, block 0 => the all-zero KAT block, words in order.
    RngStream rng(0);
    CHECK(rng.next_u32() == 0x6627e8d5u);
    CHECK(rng.next_u32() == 0xe169c58du);
    CHECK(rng.next_u32() == 0xbc57ac4cu);
    CHECK(rng.next_u32() == 0x9b00dbd8u);
    // next_u64 packs low word first.
    RngStream rng2(0);
    CHECK(rng2.next_u64() == ((std::uint64_t{0xe169c58du} << 32) | 0x6627e8d5u));
}

TEST_CASE("math.rng: identical (seed, stream) => identical sequence; others diverge") {
    RngStream a(42, 7);
    RngStream b(42, 7);
    for (int i = 0; i < 64; ++i)
        CHECK(a.next_u32() == b.next_u32());
    RngStream other_stream(42, 8);
    RngStream other_seed(43, 7);
    bool stream_diff = false;
    bool seed_diff = false;
    RngStream base(42, 7);
    for (int i = 0; i < 8; ++i) {
        const std::uint32_t v = base.next_u32();
        stream_diff = stream_diff || (other_stream.next_u32() != v);
        seed_diff = seed_diff || (other_seed.next_u32() != v);
    }
    CHECK(stream_diff);
    CHECK(seed_diff);
}

TEST_CASE("math.rng: child streams are pure, order-independent derivations") {
    const RngStream parent(1234, 5);
    RngStream drained(1234, 5);
    for (int i = 0; i < 100; ++i)
        (void)drained.next_u32();

    // Drawing from the parent does not change what children it derives.
    RngStream child_a = parent.child(Name("enemy.spawner"));
    RngStream child_b = drained.child(Name("enemy.spawner"));
    CHECK(child_a.stream() == child_b.stream());
    for (int i = 0; i < 16; ++i)
        CHECK(child_a.next_u32() == child_b.next_u32());

    // Distinct names / indices give distinct streams; name- and index-derived
    // children live in separate domains.
    CHECK(parent.child(Name("a")).stream() != parent.child(Name("b")).stream());
    CHECK(parent.child(std::uint64_t{0}).stream() != parent.child(std::uint64_t{1}).stream());
    CHECK(parent.child(Name("a")).stream() != parent.child(std::uint64_t{0}).stream());
    // Children share the parent's seed (key); identity lives in the stream.
    CHECK(parent.child(Name("a")).seed() == parent.seed());
}

TEST_CASE("math.rng: uniform ranges hold and are unbiased at the edges") {
    RngStream rng(99);
    for (int i = 0; i < 1000; ++i) {
        const float f = rng.uniform_float();
        CHECK(f >= 0.0f);
        CHECK(f < 1.0f);
        const double d = rng.uniform_double();
        CHECK(d >= 0.0);
        CHECK(d < 1.0);
        CHECK(rng.uniform_below(7) < 7u);
        const std::int32_t v = rng.uniform_int(-3, 3);
        CHECK(v >= -3);
        CHECK(v <= 3);
        CHECK(rng.uniform_u64_below(1000000007ull) < 1000000007ull);
    }
    // Degenerate and full-width spans.
    CHECK(rng.uniform_int(5, 5) == 5);
    CHECK(rng.uniform_below(1) == 0u);
    CHECK(rng.uniform_u64_below(1) == 0ull);
    const std::int32_t full = rng.uniform_int(-2147483647 - 1, 2147483647);
    (void)full; // any value is in range by type
    // Small-bound coverage: all 5 buckets hit within 200 draws.
    bool seen[5] = {};
    for (int i = 0; i < 200; ++i)
        seen[rng.uniform_below(5)] = true;
    for (const bool s : seen)
        CHECK(s);
}

TEST_CASE("math.rng: det_log is accurate and bit-stable") {
    CHECK(det_log(1.0) == 0.0); // exact by construction
    // Accuracy vs libm across magnitudes (libm is the accuracy reference
    // here, not the determinism reference).
    for (const double x : {1e-300, 2.2e-308, 1e-12, 0.02425, 0.5, 0.9999999, 1.5, 2.0, 1e17}) {
        const double got = det_log(x);
        const double want = std::log(x);
        CHECK(std::fabs(got - want) <= 4e-15 * std::fabs(want) + 1e-16);
    }
    // Subnormal domain (normal() never reaches it; det_log still must not lie).
    CHECK(std::fabs(det_log(1e-320) - std::log(1e-320)) < 1e-12);
    // Bit-stability: repeat calls give identical bits.
    CHECK(det_log(0.02424999) == det_log(0.02424999));
}

TEST_CASE("math.rng: normal() has standard moments and pinned determinism") {
    RngStream rng(2718);
    const int n = 20000;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = rng.normal();
        sum += v;
        sum_sq += v * v;
    }
    const double mean = sum / n;
    const double variance = sum_sq / n - mean * mean;
    CHECK(std::fabs(mean) < 0.02);
    CHECK(std::fabs(variance - 1.0) < 0.05);
    // Bit determinism of the full path (uniform -> ICDF incl. det_log tails).
    RngStream x(555);
    RngStream y(555);
    for (int i = 0; i < 4096; ++i)
        CHECK(x.normal() == y.normal());
}

TEST_CASE("math.rng: sphere/disk samples satisfy their geometric contracts") {
    RngStream rng(31337);
    for (int i = 0; i < 500; ++i) {
        const auto s = rng.on_sphere();
        CHECK(midday::math::almost_equal(s.length_squared(), 1.0f, 1e-5f));
        const auto d = rng.in_disk();
        CHECK(d.length_squared() <= 1.0f);
        const auto b = rng.in_sphere();
        CHECK(b.length_squared() <= 1.0f);
    }
    // Both hemispheres reachable.
    RngStream rng2(1);
    bool up = false;
    bool down = false;
    for (int i = 0; i < 100; ++i) {
        const float z = rng2.on_sphere().z;
        up = up || z > 0.0f;
        down = down || z < 0.0f;
    }
    CHECK(up);
    CHECK(down);
}

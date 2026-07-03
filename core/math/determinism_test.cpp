// math.determinism — THE m0-math-stdlib exit fixture: 1,000,000 RNG/
// transform/geometry operations, digested with XXH3-64. Run twice
// IN-PROCESS and compared (two independent evaluations, not a self-diff),
// and pinned against the committed cross-platform constant. `midday
// selftest --json` additionally publishes the digest as `math_fixture_hash`
// so CI's determinism lane can byte-compare across hosts.

#include "core/math/fixture.h"
#include "testkit/doctest.h"

#include <cstdint>

TEST_CASE("math.determinism: 1M-op fixture digests identically across runs") {
    const std::uint64_t first = midday::math::determinism_fixture_hash();
    const std::uint64_t second = midday::math::determinism_fixture_hash();
    CHECK(first == second);
    MESSAGE("math_fixture_hash = ", first);

    // Cross-platform lock: pinned from the first verified implementation.
    // Every compiler/platform lane must reproduce this exact digest — only
    // bit-portable operations are on the fixture path (see fixture.h); a
    // mismatch means FP-flag drift, a libm call leaking in, or hash drift.
    constexpr std::uint64_t kPinnedDigest = 0x71086ED73CCCE502ull;
    CHECK(first == kPinnedDigest);
}

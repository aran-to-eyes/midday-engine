// core/math/fixture.h — the m0-math-stdlib determinism fixture: exactly
// 1,000,000 RNG/transform/geometry operations, every result folded into one
// XXH3-64 digest.
//
// Everything the fixture touches is from the BIT-PORTABLE determinism class
// (no libm calls anywhere on the fixture path), so the digest is one number
// that must match across runs, hosts, AND platforms. `midday selftest`
// re-runs it twice in-process (math.determinism test) and publishes the hex
// digest in the --json payload (`math_fixture_hash`) for CI's determinism
// lane to byte-compare across independent runs.

#pragma once

#include <cstdint>

namespace midday::math {

inline constexpr std::uint64_t kFixtureOps = 1'000'000;

// Pure function: same build, same bits, always.
std::uint64_t determinism_fixture_hash();

} // namespace midday::math

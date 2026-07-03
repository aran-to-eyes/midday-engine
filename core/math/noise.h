// core/math/noise.h — deterministic coherent noise: value noise, Perlin
// (improved, 2002) gradient noise, and an fBm stack over either.
//
// Cross-platform bit-exactness by construction (spec section 13 procgen
// expects C++/TS hash-identical outputs later):
//   * Lattice hashing is INTEGER-ONLY: a SplitMix64-style finalizer
//     (Steele/Lea/Vigna's mix constants) over (seed, cell coords). No float
//     enters the hash; no permutation table (a seed parameter replaces
//     Perlin's fixed table so every field is independently seedable).
//   * Float math is confined to documented-safe operations: +, -, * and
//     std::floor — all IEEE-exact under the deterministic-FP policy.
//   * Gradients are Ken Perlin's improved-noise set (the h & 15 selection
//     from the 2002 reference implementation).
//
// Ranges: perlin_* is in [-1, 1] (zero at lattice points); value_* is in
// [-1, 1] (corner values are uniform). fbm_* normalizes by the summed
// amplitude, staying within the base noise range.
//
// Domain: |coordinate| < 2^31 lattice cells (int32 cell indices; documented
// bound, not checked per call on the hot path).

#pragma once

#include "core/math/vec.h"

#include <cstdint>

namespace midday::math {

// SplitMix64 finalizer — the engine's canonical integer mixer for lattice/
// procgen hashing (distinct from XXH3, which owns content/identity hashing).
constexpr std::uint64_t mix64(std::uint64_t z) {
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ull;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBull;
    z ^= z >> 31;
    return z;
}

float value_noise_2d(std::uint64_t seed, Vec2 p);

float value_noise_3d(std::uint64_t seed, Vec3 p);

float perlin_2d(std::uint64_t seed, Vec2 p);

float perlin_3d(std::uint64_t seed, Vec3 p);

// Fractal Brownian motion parameters. Per-octave seeds are decorrelated by
// mix64(seed ^ octave); frequency scales by `lacunarity`, amplitude by `gain`.
struct Fbm {
    int octaves = 4;
    float lacunarity = 2.0f;
    float gain = 0.5f;
};

float fbm_value_3d(std::uint64_t seed, Vec3 p, const Fbm& params);

float fbm_perlin_3d(std::uint64_t seed, Vec3 p, const Fbm& params);

} // namespace midday::math

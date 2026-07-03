#include "core/math/noise.h"

#include <cmath>

namespace midday::math {
namespace {

// Integer lattice hash: coordinates are decorrelated with distinct odd
// multipliers (xxhash's 64-bit prime family), then finalized with mix64.
std::uint64_t lattice_hash(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t z) {
    std::uint64_t h = seed;
    h = mix64(h ^
              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0x9E3779B185EBCA87ull));
    h = mix64(h ^
              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0xC2B2AE3D27D4EB4Full));
    h = mix64(h ^
              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 0x165667B19E3779F9ull));
    return h;
}

// Corner value in [-1, 1): top 24 hash bits scaled exactly (same construction
// as RngStream::uniform_float).
float corner_value(std::uint64_t h) {
    return static_cast<float>(h >> 40) * 0x1p-23f - 1.0f;
}

// Ken Perlin's improved-noise gradient select (2002 reference, h & 15).
float grad3(std::uint64_t hash, float x, float y, float z) {
    const auto h = static_cast<std::uint32_t>(hash >> 32) & 15u; // decorrelated high bits
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

// 2D gradients: the four diagonals (the classic 2D Perlin set) — with them
// the interpolated extremum is exactly +/- sqrt(2)/2, so a sqrt(2) scale
// yields a tight [-1, 1] range (provable, unlike mixed gradient sets).
float grad2(std::uint64_t hash, float x, float y) {
    switch (static_cast<std::uint32_t>(hash >> 32) & 3u) {
    case 0:
        return x + y;
    case 1:
        return x - y;
    case 2:
        return -x + y;
    default:
        return -x - y;
    }
}

// Perlin's quintic fade: 6t^5 - 15t^4 + 10t^3 (C2 at the lattice).
constexpr float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

struct Cell {
    std::int32_t index;
    float frac;
};

Cell split(float coord) {
    const float floored = std::floor(coord);
    return {static_cast<std::int32_t>(floored), coord - floored};
}

} // namespace

float value_noise_2d(std::uint64_t seed, Vec2 p) {
    const Cell cx = split(p.x);
    const Cell cy = split(p.y);
    const float u = fade(cx.frac);
    const float v = fade(cy.frac);
    const float n00 = corner_value(lattice_hash(seed, cx.index, cy.index, 0));
    const float n10 = corner_value(lattice_hash(seed, cx.index + 1, cy.index, 0));
    const float n01 = corner_value(lattice_hash(seed, cx.index, cy.index + 1, 0));
    const float n11 = corner_value(lattice_hash(seed, cx.index + 1, cy.index + 1, 0));
    return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
}

float value_noise_3d(std::uint64_t seed, Vec3 p) {
    const Cell cx = split(p.x);
    const Cell cy = split(p.y);
    const Cell cz = split(p.z);
    const float u = fade(cx.frac);
    const float v = fade(cy.frac);
    const float w = fade(cz.frac);
    float corners[2][2][2];
    for (std::int32_t dz = 0; dz < 2; ++dz)
        for (std::int32_t dy = 0; dy < 2; ++dy)
            for (std::int32_t dx = 0; dx < 2; ++dx)
                corners[dz][dy][dx] =
                    corner_value(lattice_hash(seed, cx.index + dx, cy.index + dy, cz.index + dz));
    const float front = lerp(lerp(corners[0][0][0], corners[0][0][1], u),
                             lerp(corners[0][1][0], corners[0][1][1], u),
                             v);
    const float back = lerp(lerp(corners[1][0][0], corners[1][0][1], u),
                            lerp(corners[1][1][0], corners[1][1][1], u),
                            v);
    return lerp(front, back, w);
}

float perlin_2d(std::uint64_t seed, Vec2 p) {
    const Cell cx = split(p.x);
    const Cell cy = split(p.y);
    const float u = fade(cx.frac);
    const float v = fade(cy.frac);
    const float n00 = grad2(lattice_hash(seed, cx.index, cy.index, 0), cx.frac, cy.frac);
    const float n10 = grad2(lattice_hash(seed, cx.index + 1, cy.index, 0), cx.frac - 1.0f, cy.frac);
    const float n01 = grad2(lattice_hash(seed, cx.index, cy.index + 1, 0), cx.frac, cy.frac - 1.0f);
    const float n11 =
        grad2(lattice_hash(seed, cx.index + 1, cy.index + 1, 0), cx.frac - 1.0f, cy.frac - 1.0f);
    // Length-sqrt(2) diagonal gradients bound the value by sqrt(2) * the
    // classic sqrt(2)/2 unit-gradient extremum = 1: already in [-1, 1]
    // (empirical extrema ~ +/-0.78, asserted by math.noise range tests).
    return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
}

float perlin_3d(std::uint64_t seed, Vec3 p) {
    const Cell cx = split(p.x);
    const Cell cy = split(p.y);
    const Cell cz = split(p.z);
    const float u = fade(cx.frac);
    const float v = fade(cy.frac);
    const float w = fade(cz.frac);
    float dots[2][2][2];
    for (std::int32_t dz = 0; dz < 2; ++dz)
        for (std::int32_t dy = 0; dy < 2; ++dy)
            for (std::int32_t dx = 0; dx < 2; ++dx)
                dots[dz][dy][dx] =
                    grad3(lattice_hash(seed, cx.index + dx, cy.index + dy, cz.index + dz),
                          cx.frac - static_cast<float>(dx),
                          cy.frac - static_cast<float>(dy),
                          cz.frac - static_cast<float>(dz));
    const float front =
        lerp(lerp(dots[0][0][0], dots[0][0][1], u), lerp(dots[0][1][0], dots[0][1][1], u), v);
    const float back =
        lerp(lerp(dots[1][0][0], dots[1][0][1], u), lerp(dots[1][1][0], dots[1][1][1], u), v);
    return lerp(front, back, w);
}

namespace {

template <typename NoiseFn>
float fbm(std::uint64_t seed, Vec3 p, const Fbm& params, NoiseFn noise) {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    Vec3 q = p;
    for (int octave = 0; octave < params.octaves; ++octave) {
        sum += amplitude * noise(mix64(seed ^ static_cast<std::uint64_t>(octave)), q);
        total += amplitude;
        amplitude *= params.gain;
        q = q * params.lacunarity;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

} // namespace

float fbm_value_3d(std::uint64_t seed, Vec3 p, const Fbm& params) {
    return fbm(seed, p, params, value_noise_3d);
}

float fbm_perlin_3d(std::uint64_t seed, Vec3 p, const Fbm& params) {
    return fbm(seed, p, params, perlin_3d);
}

} // namespace midday::math

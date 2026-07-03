// math.noise.* — determinism, lattice-zero property, range bounds, seed
// sensitivity, continuity, and pinned known-answer bits (the cross-platform
// lock: these exact float bit patterns must reproduce on every lane).

#include "core/math/noise.h"
#include "core/math/vec.h"
#include "doctest/doctest.h"

#include <cstdint>
#include <cstring>

using namespace midday::math;

namespace {

std::uint32_t bits_of(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

constexpr std::uint64_t kSeed = 0xC0FFEE0123456789ull;

} // namespace

TEST_CASE("math.noise: identical inputs give identical bits; seeds decorrelate") {
    const Vec3 p{1.37f, -4.2f, 0.55f};
    CHECK(bits_of(perlin_3d(kSeed, p)) == bits_of(perlin_3d(kSeed, p)));
    CHECK(bits_of(value_noise_3d(kSeed, p)) == bits_of(value_noise_3d(kSeed, p)));
    CHECK(perlin_3d(kSeed, p) != perlin_3d(kSeed + 1, p));
    CHECK(value_noise_3d(kSeed, p) != value_noise_3d(kSeed + 1, p));
    CHECK(perlin_2d(kSeed, {1.5f, 2.5f}) != perlin_2d(kSeed + 1, {1.5f, 2.5f}));
}

TEST_CASE("math.noise: perlin is exactly zero on the integer lattice") {
    for (int x = -3; x <= 3; ++x)
        for (int y = -3; y <= 3; ++y) {
            CHECK(perlin_2d(kSeed, {static_cast<float>(x), static_cast<float>(y)}) == 0.0f);
            CHECK(perlin_3d(kSeed, {static_cast<float>(x), static_cast<float>(y), -2.0f}) == 0.0f);
        }
}

TEST_CASE("math.noise: outputs stay inside the documented ranges") {
    float lo2 = 1e9f;
    float hi2 = -1e9f;
    for (int i = 0; i < 4000; ++i) {
        const int row = i / 63;
        const float fx = static_cast<float>(i % 63) * 0.173f - 5.0f;
        const float fy = static_cast<float>(row) * 0.291f - 9.0f;
        const float p2 = perlin_2d(kSeed, {fx, fy});
        CHECK(p2 >= -1.0f);
        CHECK(p2 <= 1.0f);
        lo2 = p2 < lo2 ? p2 : lo2;
        hi2 = p2 > hi2 ? p2 : hi2;
        const float p3 = perlin_3d(kSeed, {fx, fy, fx * 0.5f + 0.25f});
        CHECK(p3 >= -1.0f);
        CHECK(p3 <= 1.0f);
        const float v3 = value_noise_3d(kSeed, {fx, fy, 0.77f});
        CHECK(v3 >= -1.0f);
        CHECK(v3 <= 1.0f);
        const float v2 = value_noise_2d(kSeed, {fx, fy});
        CHECK(v2 >= -1.0f);
        CHECK(v2 <= 1.0f);
    }
    // The field actually uses a good part of its range (not stuck near zero).
    CHECK(lo2 < -0.4f);
    CHECK(hi2 > 0.4f);
}

TEST_CASE("math.noise: fields are continuous across cell boundaries") {
    // Step across the x = 2 lattice line in tiny increments: no jumps.
    for (int i = -4; i <= 4; ++i) {
        const float x = 2.0f + static_cast<float>(i) * 1e-3f;
        const float a = perlin_3d(kSeed, {x, 0.5f, 0.5f});
        const float b = perlin_3d(kSeed, {x + 1e-3f, 0.5f, 0.5f});
        CHECK(almost_equal(a, b, 2e-2f));
        const float va = value_noise_2d(kSeed, {x, 0.5f});
        const float vb = value_noise_2d(kSeed, {x + 1e-3f, 0.5f});
        CHECK(almost_equal(va, vb, 2e-2f));
    }
}

TEST_CASE("math.noise: fbm normalizes amplitude and adds detail") {
    const Fbm params{5, 2.0f, 0.5f};
    for (int i = 0; i < 500; ++i) {
        const Vec3 p{static_cast<float>(i) * 0.137f, static_cast<float>(i % 17) * 0.311f, 1.5f};
        const float f = fbm_perlin_3d(kSeed, p, params);
        CHECK(f >= -1.0f);
        CHECK(f <= 1.0f);
        const float v = fbm_value_3d(kSeed, p, params);
        CHECK(v >= -1.0f);
        CHECK(v <= 1.0f);
    }
    // One octave of fbm IS the base field (normalization sanity).
    const Vec3 p{0.3f, 0.7f, 0.9f};
    CHECK(fbm_perlin_3d(kSeed, p, Fbm{1, 2.0f, 0.5f}) ==
          perlin_3d(mix64(kSeed), p)); // octave 0 seed = mix64(seed ^ 0)
}

TEST_CASE("math.noise: known-answer bits (cross-platform lock)") {
    // Pinned on first implementation (macOS arm64 / AppleClang); every lane
    // must reproduce these bits exactly — a mismatch is FP or hash drift.
    CHECK(mix64(0) == 0ull);
    CHECK(mix64(1) == 0x5692161D100B05E5ull);
    CHECK(mix64(0x9E3779B97F4A7C15ull) == 0xE220A8397B1DCDAFull);

    CHECK(bits_of(perlin_3d(kSeed, {1.37f, -4.2f, 0.55f})) == 0x3D118630u);
    CHECK(bits_of(perlin_2d(kSeed, {0.31f, 7.9f})) == 0xBE8873ECu);
    CHECK(bits_of(value_noise_3d(kSeed, {-2.6f, 3.1f, 9.42f})) == 0x3D655030u);
    CHECK(bits_of(fbm_perlin_3d(kSeed, {0.5f, 0.25f, -1.75f}, Fbm{})) == 0xBEE959E6u);
}

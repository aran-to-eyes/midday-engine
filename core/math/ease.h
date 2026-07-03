// core/math/ease.h — the standard easing family (Penner easings, exact
// formulas as standardized at easings.net; constants c1/c2/c3/c4/c5/n1/d1
// carry their conventional names so the formulas can be checked against the
// reference by eye).
//
// Every function maps t in [0, 1] to [0, 1]-ish (back/elastic overshoot by
// design); inputs outside [0, 1] extrapolate the formula (callers clamp).
//
// Determinism classes (per function, see core/math/README.md):
//   BIT-PORTABLE: linear, quad, cubic, quart, quint, circ (sqrt), back,
//                 bounce — polynomial/sqrt arithmetic only.
//   LIBM-BOUND:   sine (cos/sin), expo (exp2), elastic (exp2/sin) —
//                 deterministic within one build only. Sequences that must be
//                 cross-platform bit-exact use the bit-portable set.

#pragma once

#include "core/math/vec.h"

#include <cmath>
#include <cstdint>

namespace midday::math {

enum class Ease : std::uint8_t {
    Linear,
    InQuad,
    OutQuad,
    InOutQuad,
    InCubic,
    OutCubic,
    InOutCubic,
    InQuart,
    OutQuart,
    InOutQuart,
    InQuint,
    OutQuint,
    InOutQuint,
    InSine,
    OutSine,
    InOutSine,
    InExpo,
    OutExpo,
    InOutExpo,
    InCirc,
    OutCirc,
    InOutCirc,
    InBack,
    OutBack,
    InOutBack,
    InElastic,
    OutElastic,
    InOutElastic,
    InBounce,
    OutBounce,
    InOutBounce,
};

constexpr float ease_linear(float t) {
    return t;
}

constexpr float ease_in_quad(float t) {
    return t * t;
}

constexpr float ease_out_quad(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t);
}

constexpr float ease_in_out_quad(float t) {
    return t < 0.5f ? 2.0f * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * 0.5f;
}

constexpr float ease_in_cubic(float t) {
    return t * t * t;
}

constexpr float ease_out_cubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

constexpr float ease_in_out_cubic(float t) {
    if (t < 0.5f)
        return 4.0f * t * t * t;
    const float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * 0.5f;
}

constexpr float ease_in_quart(float t) {
    return t * t * t * t;
}

constexpr float ease_out_quart(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u * u;
}

constexpr float ease_in_out_quart(float t) {
    if (t < 0.5f)
        return 8.0f * t * t * t * t;
    const float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * u * 0.5f;
}

constexpr float ease_in_quint(float t) {
    return t * t * t * t * t;
}

constexpr float ease_out_quint(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

constexpr float ease_in_out_quint(float t) {
    if (t < 0.5f)
        return 16.0f * t * t * t * t * t;
    const float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u * u * u * 0.5f;
}

// LIBM-BOUND (cos/sin).
inline float ease_in_sine(float t) {
    return 1.0f - std::cos(t * kPi * 0.5f);
}

inline float ease_out_sine(float t) {
    return std::sin(t * kPi * 0.5f);
}

inline float ease_in_out_sine(float t) {
    return -(std::cos(kPi * t) - 1.0f) * 0.5f;
}

// LIBM-BOUND (exp2). Exact 0/1 endpoints by definition.
inline float ease_in_expo(float t) {
    return t == 0.0f ? 0.0f : std::exp2(10.0f * t - 10.0f);
}

inline float ease_out_expo(float t) {
    return t == 1.0f ? 1.0f : 1.0f - std::exp2(-10.0f * t);
}

inline float ease_in_out_expo(float t) {
    if (t == 0.0f || t == 1.0f)
        return t;
    if (t < 0.5f)
        return std::exp2(20.0f * t - 10.0f) * 0.5f;
    return (2.0f - std::exp2(-20.0f * t + 10.0f)) * 0.5f;
}

inline float ease_in_circ(float t) {
    return 1.0f - std::sqrt(1.0f - t * t);
}

inline float ease_out_circ(float t) {
    return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f));
}

inline float ease_in_out_circ(float t) {
    if (t < 0.5f)
        return (1.0f - std::sqrt(1.0f - 4.0f * t * t)) * 0.5f;
    const float u = -2.0f * t + 2.0f;
    return (std::sqrt(1.0f - u * u) + 1.0f) * 0.5f;
}

inline constexpr float kBackC1 = 1.70158f;
inline constexpr float kBackC2 = kBackC1 * 1.525f;
inline constexpr float kBackC3 = kBackC1 + 1.0f;

constexpr float ease_in_back(float t) {
    return kBackC3 * t * t * t - kBackC1 * t * t;
}

constexpr float ease_out_back(float t) {
    const float u = t - 1.0f;
    return 1.0f + kBackC3 * u * u * u + kBackC1 * u * u;
}

constexpr float ease_in_out_back(float t) {
    if (t < 0.5f) {
        const float u = 2.0f * t;
        return u * u * ((kBackC2 + 1.0f) * u - kBackC2) * 0.5f;
    }
    const float u = 2.0f * t - 2.0f;
    return (u * u * ((kBackC2 + 1.0f) * u + kBackC2) + 2.0f) * 0.5f;
}

// LIBM-BOUND (exp2/sin). c4 = 2*pi/3, c5 = 2*pi/4.5 (reference constants).
inline float ease_in_elastic(float t) {
    if (t == 0.0f || t == 1.0f)
        return t;
    constexpr float c4 = kTau / 3.0f;
    return -std::exp2(10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
}

inline float ease_out_elastic(float t) {
    if (t == 0.0f || t == 1.0f)
        return t;
    constexpr float c4 = kTau / 3.0f;
    return std::exp2(-10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

inline float ease_in_out_elastic(float t) {
    if (t == 0.0f || t == 1.0f)
        return t;
    constexpr float c5 = kTau / 4.5f;
    if (t < 0.5f)
        return -std::exp2(20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * c5) * 0.5f;
    return std::exp2(-20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * c5) * 0.5f + 1.0f;
}

// Bounce: n1 = 7.5625, d1 = 2.75 (reference constants). Pure arithmetic.
constexpr float ease_out_bounce(float t) {
    constexpr float n1 = 7.5625f;
    constexpr float d1 = 2.75f;
    if (t < 1.0f / d1)
        return n1 * t * t;
    if (t < 2.0f / d1) {
        const float u = t - 1.5f / d1;
        return n1 * u * u + 0.75f;
    }
    if (t < 2.5f / d1) {
        const float u = t - 2.25f / d1;
        return n1 * u * u + 0.9375f;
    }
    const float u = t - 2.625f / d1;
    return n1 * u * u + 0.984375f;
}

constexpr float ease_in_bounce(float t) {
    return 1.0f - ease_out_bounce(1.0f - t);
}

constexpr float ease_in_out_bounce(float t) {
    if (t < 0.5f)
        return (1.0f - ease_out_bounce(1.0f - 2.0f * t)) * 0.5f;
    return (1.0f + ease_out_bounce(2.0f * t - 1.0f)) * 0.5f;
}

// Enum dispatch — the form sequence tracks and reflection bind against.
float ease(Ease kind, float t);

} // namespace midday::math

// core/math/spline.h — spline MATH only (spec section 13: splines are a scene
// primitive; the component/consumer integration is node m4-splines — this
// layer is the evaluation kernels it and the procgen stdlib share).
//
// Segment evaluators (one cubic segment, t in [0, 1]):
//   * bezier_cubic       — de Casteljau form (numerically the stable choice;
//                          Farin, "Curves and Surfaces for CAGD").
//   * catmull_rom        — uniform Catmull-Rom via its Hermite form with
//                          tangents (p2 - p0)/2, (p3 - p1)/2 (tau = 1/2, the
//                          standard parameterization).
//   * bspline_cubic      — uniform cubic B-spline basis (approximating, C2).
// Each evaluator has a *_derivative sibling (velocity along the segment).
//
// Arc length: sampled cumulative-length table + inverse lookup — the same
// bake-then-interpolate approach as Godot's Curve3D, chosen deliberately: it
// is exactly reproducible (fixed sample count, fixed summation order) where
// adaptive quadrature would make the bits depend on recursion tolerances.
// The accumulator is double so table quality does not decay with segment
// count; stored entries are float32 (the sim scalar).
//
// Determinism class: BIT-PORTABLE (arithmetic + sqrt only).

#pragma once

#include "core/math/vec.h"

#include <cstddef>
#include <vector>

namespace midday::math {

constexpr Vec3 bezier_cubic(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    const Vec3 a = lerp(p0, p1, t);
    const Vec3 b = lerp(p1, p2, t);
    const Vec3 c = lerp(p2, p3, t);
    return lerp(lerp(a, b, t), lerp(b, c, t), t);
}

constexpr Vec3 bezier_cubic_derivative(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    const Vec3 a = lerp(p1 - p0, p2 - p1, t);
    const Vec3 b = lerp(p2 - p1, p3 - p2, t);
    return lerp(a, b, t) * 3.0f;
}

// Uniform Catmull-Rom (interpolates p1..p2; p0/p3 shape the tangents).
constexpr Vec3 catmull_rom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    const Vec3 m1 = (p2 - p0) * 0.5f;
    const Vec3 m2 = (p3 - p1) * 0.5f;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return p1 * (2.0f * t3 - 3.0f * t2 + 1.0f) + m1 * (t3 - 2.0f * t2 + t) +
           p2 * (-2.0f * t3 + 3.0f * t2) + m2 * (t3 - t2);
}

constexpr Vec3 catmull_rom_derivative(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    const Vec3 m1 = (p2 - p0) * 0.5f;
    const Vec3 m2 = (p3 - p1) * 0.5f;
    const float t2 = t * t;
    return p1 * (6.0f * t2 - 6.0f * t) + m1 * (3.0f * t2 - 4.0f * t + 1.0f) +
           p2 * (-6.0f * t2 + 6.0f * t) + m2 * (3.0f * t2 - 2.0f * t);
}

// Uniform cubic B-spline segment (approximating: the curve does not pass
// through the control points; C2-continuous across segments).
constexpr Vec3 bspline_cubic(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float b0 = (1.0f - 3.0f * t + 3.0f * t2 - t3) * (1.0f / 6.0f);
    const float b1 = (4.0f - 6.0f * t2 + 3.0f * t3) * (1.0f / 6.0f);
    const float b2 = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) * (1.0f / 6.0f);
    const float b3 = t3 * (1.0f / 6.0f);
    return p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3;
}

constexpr Vec3 bspline_cubic_derivative(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    const float t2 = t * t;
    const float b0 = (-1.0f + 2.0f * t - t2) * 0.5f;
    const float b1 = (-4.0f * t + 3.0f * t2) * 0.5f;
    const float b2 = (1.0f + 2.0f * t - 3.0f * t2) * 0.5f;
    const float b3 = t2 * 0.5f;
    return p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3;
}

// Cumulative arc-length table over a curve parameterized on t in [0, 1].
// build() samples the curve at `samples` uniform parameters (chord lengths,
// double accumulator, fixed order). t_at_length() inverts by binary search +
// linear interpolation between samples.
class ArcLengthTable {
public:
    // `eval`: Vec3(float t). samples >= 2 (the sample count is part of the
    // deterministic contract: same curve + same count = same table bits).
    template <typename F> static ArcLengthTable build(F&& eval, int samples) {
        ArcLengthTable table;
        table.cumulative_.resize(static_cast<std::size_t>(samples));
        double sum = 0.0;
        Vec3 prev = eval(0.0f);
        table.cumulative_[0] = 0.0f;
        const float step = 1.0f / static_cast<float>(samples - 1);
        for (int i = 1; i < samples; ++i) {
            const Vec3 p = eval(static_cast<float>(i) * step);
            sum += static_cast<double>((p - prev).length());
            table.cumulative_[static_cast<std::size_t>(i)] = static_cast<float>(sum);
            prev = p;
        }
        return table;
    }

    [[nodiscard]] float length() const { return cumulative_.back(); }

    // Curve parameter t whose arc length from t=0 is `s` (clamped to the
    // curve). Piecewise-linear inverse of the sampled length function.
    [[nodiscard]] float t_at_length(float s) const;

    // Convenience: t for a normalized position u in [0, 1] along the curve.
    [[nodiscard]] float t_at_fraction(float u) const { return t_at_length(u * length()); }

private:
    std::vector<float> cumulative_; // cumulative_[i] = length at t = i/(n-1)
};

} // namespace midday::math

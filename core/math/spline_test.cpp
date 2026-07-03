// math.spline.* — segment evaluators (known answers, endpoint interpolation,
// derivative cross-checks) and the arc-length table.

#include "core/math/spline.h"
#include "core/math/vec.h"
#include "doctest/doctest.h"

using namespace midday::math;

namespace {

bool vec3_almost_equal(Vec3 a, Vec3 b, float eps = 1e-5f) {
    return almost_equal(a.x, b.x, eps) && almost_equal(a.y, b.y, eps) &&
           almost_equal(a.z, b.z, eps);
}

// Central-difference derivative check shared by all evaluators.
template <typename Eval, typename Deriv> void check_derivative(Eval eval, Deriv deriv, float t) {
    constexpr float h = 1e-3f;
    const Vec3 numeric = (eval(t + h) - eval(t - h)) / (2.0f * h);
    const Vec3 analytic = deriv(t);
    CHECK(vec3_almost_equal(numeric, analytic, 5e-3f));
}

const Vec3 kP0{0, 0, 0};
const Vec3 kP1{1, 2, 0};
const Vec3 kP2{3, 2, 1};
const Vec3 kP3{4, 0, 2};

} // namespace

TEST_CASE("math.spline: bezier interpolates endpoints, de Casteljau midpoint") {
    CHECK(bezier_cubic(kP0, kP1, kP2, kP3, 0.0f) == kP0);
    CHECK(bezier_cubic(kP0, kP1, kP2, kP3, 1.0f) == kP3);
    // Closed form at t = 1/2: (p0 + 3 p1 + 3 p2 + p3) / 8.
    const Vec3 mid = (kP0 + kP1 * 3.0f + kP2 * 3.0f + kP3) * 0.125f;
    CHECK(vec3_almost_equal(bezier_cubic(kP0, kP1, kP2, kP3, 0.5f), mid));
    // Degenerate control polygon = straight line.
    const Vec3 a{0, 0, 0};
    const Vec3 b{3, 0, 0};
    CHECK(vec3_almost_equal(bezier_cubic(a, {1, 0, 0}, {2, 0, 0}, b, 0.25f), {0.75f, 0, 0}));
}

TEST_CASE("math.spline: bezier derivative matches finite differences") {
    const auto eval = [](float t) { return bezier_cubic(kP0, kP1, kP2, kP3, t); };
    const auto deriv = [](float t) { return bezier_cubic_derivative(kP0, kP1, kP2, kP3, t); };
    for (const float t : {0.1f, 0.35f, 0.5f, 0.82f})
        check_derivative(eval, deriv, t);
    // Endpoint tangents: 3 (p1 - p0) and 3 (p3 - p2).
    CHECK(bezier_cubic_derivative(kP0, kP1, kP2, kP3, 0.0f) == (kP1 - kP0) * 3.0f);
    CHECK(bezier_cubic_derivative(kP0, kP1, kP2, kP3, 1.0f) == (kP3 - kP2) * 3.0f);
}

TEST_CASE("math.spline: catmull-rom interpolates p1..p2 with standard tangents") {
    CHECK(catmull_rom(kP0, kP1, kP2, kP3, 0.0f) == kP1);
    CHECK(catmull_rom(kP0, kP1, kP2, kP3, 1.0f) == kP2);
    // Tangent at t=0 is (p2 - p0) / 2 (tau = 1/2 uniform parameterization).
    CHECK(catmull_rom_derivative(kP0, kP1, kP2, kP3, 0.0f) == (kP2 - kP0) * 0.5f);
    CHECK(catmull_rom_derivative(kP0, kP1, kP2, kP3, 1.0f) == (kP3 - kP1) * 0.5f);
    const auto eval = [](float t) { return catmull_rom(kP0, kP1, kP2, kP3, t); };
    const auto deriv = [](float t) { return catmull_rom_derivative(kP0, kP1, kP2, kP3, t); };
    for (const float t : {0.2f, 0.5f, 0.77f})
        check_derivative(eval, deriv, t);
}

TEST_CASE("math.spline: bspline stays in the convex hull, known midvalues") {
    // Uniform control points: the curve is the same straight line.
    const Vec3 q0{0, 0, 0};
    const Vec3 q1{1, 0, 0};
    const Vec3 q2{2, 0, 0};
    const Vec3 q3{3, 0, 0};
    // At t=0 the uniform B-spline evaluates to (p0 + 4 p1 + p2) / 6.
    CHECK(vec3_almost_equal(bspline_cubic(q0, q1, q2, q3, 0.0f), {1, 0, 0}));
    CHECK(vec3_almost_equal(bspline_cubic(q0, q1, q2, q3, 1.0f), {2, 0, 0}));
    CHECK(vec3_almost_equal(bspline_cubic(q0, q1, q2, q3, 0.5f), {1.5f, 0, 0}));
    const auto eval = [](float t) { return bspline_cubic(kP0, kP1, kP2, kP3, t); };
    const auto deriv = [](float t) { return bspline_cubic_derivative(kP0, kP1, kP2, kP3, t); };
    for (const float t : {0.15f, 0.5f, 0.9f})
        check_derivative(eval, deriv, t);
}

TEST_CASE("math.spline: arc length of a straight segment is exact-ish") {
    const auto line = [](float t) { return Vec3{3.0f * t, 4.0f * t, 0}; }; // length 5
    const ArcLengthTable table = ArcLengthTable::build(line, 64);
    CHECK(almost_equal(table.length(), 5.0f, 1e-4f));
    // Halfway along the length is halfway along t for a line.
    CHECK(almost_equal(table.t_at_length(2.5f), 0.5f, 1e-4f));
    CHECK(almost_equal(table.t_at_fraction(0.25f), 0.25f, 1e-4f));
    // Clamping.
    CHECK(table.t_at_length(-1.0f) == 0.0f);
    CHECK(table.t_at_length(99.0f) == 1.0f);
}

TEST_CASE("math.spline: arc-length reparameterization equalizes speed on a curve") {
    const auto curve = [](float t) { return bezier_cubic(kP0, kP1, kP2, kP3, t); };
    const ArcLengthTable table = ArcLengthTable::build(curve, 256);
    // Walk at equal arc-length steps: chord lengths must be near-equal even
    // though t steps are not.
    const int steps = 16;
    Vec3 prev = curve(table.t_at_fraction(0.0f));
    float min_chord = 1e9f;
    float max_chord = 0.0f;
    for (int i = 1; i <= steps; ++i) {
        const Vec3 p = curve(table.t_at_fraction(static_cast<float>(i) / steps));
        const float chord = (p - prev).length();
        min_chord = chord < min_chord ? chord : min_chord;
        max_chord = chord > max_chord ? chord : max_chord;
        prev = p;
    }
    CHECK(max_chord / min_chord < 1.05f); // near-uniform speed
    // Determinism: same build inputs, same table bits.
    const ArcLengthTable again = ArcLengthTable::build(curve, 256);
    CHECK(again.length() == table.length());
    CHECK(again.t_at_length(1.234f) == table.t_at_length(1.234f));
}

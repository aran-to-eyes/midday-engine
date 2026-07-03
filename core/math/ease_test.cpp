// math.ease.* — endpoint exactness for the whole family, spot values against
// the reference formulas, in/out reflection symmetry, enum dispatch.

#include "core/math/ease.h"
#include "doctest/doctest.h"

using namespace midday::math;

namespace {

constexpr Ease kAll[] = {
    Ease::Linear,      Ease::InQuad,     Ease::OutQuad,      Ease::InOutQuad, Ease::InCubic,
    Ease::OutCubic,    Ease::InOutCubic, Ease::InQuart,      Ease::OutQuart,  Ease::InOutQuart,
    Ease::InQuint,     Ease::OutQuint,   Ease::InOutQuint,   Ease::InSine,    Ease::OutSine,
    Ease::InOutSine,   Ease::InExpo,     Ease::OutExpo,      Ease::InOutExpo, Ease::InCirc,
    Ease::OutCirc,     Ease::InOutCirc,  Ease::InBack,       Ease::OutBack,   Ease::InOutBack,
    Ease::InElastic,   Ease::OutElastic, Ease::InOutElastic, Ease::InBounce,  Ease::OutBounce,
    Ease::InOutBounce,
};

} // namespace

TEST_CASE("math.ease: every easing maps 0 -> 0 and 1 -> 1") {
    for (const Ease kind : kAll) {
        CHECK(almost_equal(ease(kind, 0.0f), 0.0f, 1e-6f));
        CHECK(almost_equal(ease(kind, 1.0f), 1.0f, 1e-6f));
    }
    // Expo/elastic endpoints are exact by definition (branch, not limit).
    CHECK(ease_in_expo(0.0f) == 0.0f);
    CHECK(ease_out_expo(1.0f) == 1.0f);
    CHECK(ease_in_elastic(0.0f) == 0.0f);
    CHECK(ease_out_elastic(1.0f) == 1.0f);
}

TEST_CASE("math.ease: polynomial spot values match the reference formulas") {
    CHECK(ease_linear(0.3f) == 0.3f);
    CHECK(ease_in_quad(0.5f) == 0.25f);
    CHECK(ease_out_quad(0.5f) == 0.75f);
    CHECK(ease_in_out_quad(0.5f) == 0.5f);
    CHECK(ease_in_cubic(0.5f) == 0.125f);
    CHECK(ease_in_quart(0.5f) == 0.0625f);
    CHECK(ease_in_quint(0.5f) == 0.03125f);
    CHECK(almost_equal(ease_out_cubic(0.5f), 0.875f));
    // Bounce: t = 1 / d1 lands exactly on the first bounce peak (value 1).
    CHECK(almost_equal(ease_out_bounce(1.0f / 2.75f), 1.0f));
    // In-out easings pass through (0.5, 0.5) for symmetric families.
    CHECK(ease_in_out_cubic(0.5f) == 0.5f);
    CHECK(ease_in_out_quint(0.5f) == 0.5f);
    CHECK(almost_equal(ease_in_out_sine(0.5f), 0.5f));
    CHECK(almost_equal(ease_in_out_expo(0.5f), 0.5f));
    CHECK(almost_equal(ease_in_out_circ(0.5f), 0.5f));
    CHECK(almost_equal(ease_in_out_bounce(0.5f), 0.5f));
}

TEST_CASE("math.ease: out is the point reflection of in") {
    // out(t) == 1 - in(1 - t) for every matched pair.
    for (const float t : {0.1f, 0.25f, 0.5f, 0.62f, 0.9f}) {
        CHECK(almost_equal(ease_out_quad(t), 1.0f - ease_in_quad(1.0f - t)));
        CHECK(almost_equal(ease_out_cubic(t), 1.0f - ease_in_cubic(1.0f - t)));
        CHECK(almost_equal(ease_out_quart(t), 1.0f - ease_in_quart(1.0f - t)));
        CHECK(almost_equal(ease_out_quint(t), 1.0f - ease_in_quint(1.0f - t)));
        CHECK(almost_equal(ease_out_sine(t), 1.0f - ease_in_sine(1.0f - t), 1e-4f));
        CHECK(almost_equal(ease_out_expo(t), 1.0f - ease_in_expo(1.0f - t), 1e-4f));
        CHECK(almost_equal(ease_out_circ(t), 1.0f - ease_in_circ(1.0f - t), 1e-4f));
        CHECK(almost_equal(ease_out_back(t), 1.0f - ease_in_back(1.0f - t), 1e-4f));
        CHECK(almost_equal(ease_out_bounce(t), 1.0f - ease_in_bounce(1.0f - t)));
    }
}

TEST_CASE("math.ease: overshoot families overshoot as designed") {
    // Back: dips below 0 going in, above 1 coming out.
    CHECK(ease_in_back(0.2f) < 0.0f);
    CHECK(ease_out_back(0.8f) > 1.0f);
    // Elastic oscillates outside [0, 1] near the ends.
    CHECK(ease_out_elastic(0.1f) > 1.0f);
    // Bounce stays within [0, 1].
    for (const float t : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
        CHECK(ease_out_bounce(t) >= 0.0f);
        CHECK(ease_out_bounce(t) <= 1.0f);
    }
}

TEST_CASE("math.ease: enum dispatch agrees with the direct functions") {
    const float t = 0.37f;
    CHECK(ease(Ease::Linear, t) == ease_linear(t));
    CHECK(ease(Ease::InQuad, t) == ease_in_quad(t));
    CHECK(ease(Ease::InOutCubic, t) == ease_in_out_cubic(t));
    CHECK(ease(Ease::OutSine, t) == ease_out_sine(t));
    CHECK(ease(Ease::InOutExpo, t) == ease_in_out_expo(t));
    CHECK(ease(Ease::OutBack, t) == ease_out_back(t));
    CHECK(ease(Ease::InElastic, t) == ease_in_elastic(t));
    CHECK(ease(Ease::InOutBounce, t) == ease_in_out_bounce(t));
}

#include "core/math/ease.h"

namespace midday::math {

float ease(Ease kind, float t) {
    switch (kind) {
    case Ease::Linear:
        return ease_linear(t);
    case Ease::InQuad:
        return ease_in_quad(t);
    case Ease::OutQuad:
        return ease_out_quad(t);
    case Ease::InOutQuad:
        return ease_in_out_quad(t);
    case Ease::InCubic:
        return ease_in_cubic(t);
    case Ease::OutCubic:
        return ease_out_cubic(t);
    case Ease::InOutCubic:
        return ease_in_out_cubic(t);
    case Ease::InQuart:
        return ease_in_quart(t);
    case Ease::OutQuart:
        return ease_out_quart(t);
    case Ease::InOutQuart:
        return ease_in_out_quart(t);
    case Ease::InQuint:
        return ease_in_quint(t);
    case Ease::OutQuint:
        return ease_out_quint(t);
    case Ease::InOutQuint:
        return ease_in_out_quint(t);
    case Ease::InSine:
        return ease_in_sine(t);
    case Ease::OutSine:
        return ease_out_sine(t);
    case Ease::InOutSine:
        return ease_in_out_sine(t);
    case Ease::InExpo:
        return ease_in_expo(t);
    case Ease::OutExpo:
        return ease_out_expo(t);
    case Ease::InOutExpo:
        return ease_in_out_expo(t);
    case Ease::InCirc:
        return ease_in_circ(t);
    case Ease::OutCirc:
        return ease_out_circ(t);
    case Ease::InOutCirc:
        return ease_in_out_circ(t);
    case Ease::InBack:
        return ease_in_back(t);
    case Ease::OutBack:
        return ease_out_back(t);
    case Ease::InOutBack:
        return ease_in_out_back(t);
    case Ease::InElastic:
        return ease_in_elastic(t);
    case Ease::OutElastic:
        return ease_out_elastic(t);
    case Ease::InOutElastic:
        return ease_in_out_elastic(t);
    case Ease::InBounce:
        return ease_in_bounce(t);
    case Ease::OutBounce:
        return ease_out_bounce(t);
    case Ease::InOutBounce:
        return ease_in_out_bounce(t);
    }
    return t; // unreachable with a valid enum; deterministic fallback
}

} // namespace midday::math

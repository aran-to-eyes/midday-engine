#include "core/math/spline.h"

#include <algorithm>
#include <cstddef>

namespace midday::math {

float ArcLengthTable::t_at_length(float s) const {
    const auto n = cumulative_.size();
    if (s <= 0.0f)
        return 0.0f;
    if (s >= cumulative_.back())
        return 1.0f;
    // First sample with cumulative length >= s; its predecessor brackets s.
    const auto it = std::ranges::lower_bound(cumulative_, s);
    const auto hi = static_cast<std::size_t>(it - cumulative_.begin());
    const std::size_t lo = hi - 1;
    const float seg = cumulative_[hi] - cumulative_[lo];
    const float frac = seg > 0.0f ? (s - cumulative_[lo]) / seg : 0.0f;
    const float step = 1.0f / static_cast<float>(n - 1);
    return (static_cast<float>(lo) + frac) * step;
}

} // namespace midday::math

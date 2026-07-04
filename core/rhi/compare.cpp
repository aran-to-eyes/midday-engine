// core/rhi/compare.cpp — two-tier comparison + diff emission (compare.h).
// One integer pass over the union canvas produces every tier-2 metric; the
// only floating-point operations are two final divisions of exact integer
// sums (IEEE-correctly-rounded, bit-portable class per D-BUILD-019).

#include "core/rhi/compare.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>

namespace midday::rhi {
namespace {

// A pixel fetch that answers "absent" outside an image's extent — the union
// canvas treats one-sided pixels as maximally different (delta 255).
std::array<int, 4>
channel_deltas(const ImageRgba8& a, const ImageRgba8& b, std::uint32_t x, std::uint32_t y) {
    const bool in_a = x < a.width && y < a.height;
    const bool in_b = x < b.width && y < b.height;
    if (!in_a || !in_b)
        return {255, 255, 255, 255};
    const std::array<std::uint8_t, 4> pa = a.at(x, y);
    const std::array<std::uint8_t, 4> pb = b.at(x, y);
    return {std::abs(int{pa[0]} - int{pb[0]}),
            std::abs(int{pa[1]} - int{pb[1]}),
            std::abs(int{pa[2]} - int{pb[2]}),
            std::abs(int{pa[3]} - int{pb[3]})};
}

} // namespace

CompareVerdict
compare_images(const ImageRgba8& a, const ImageRgba8& b, const ToleranceThresholds& thresholds) {
    CompareVerdict verdict;
    verdict.hash_a = pixel_hash(a);
    verdict.hash_b = pixel_hash(b);
    verdict.hash_equal = verdict.hash_a == verdict.hash_b;

    ToleranceVerdict& tol = verdict.tolerance;
    tol.dims_match = a.width == b.width && a.height == b.height;
    tol.width = std::max(a.width, b.width);
    tol.height = std::max(a.height, b.height);

    std::uint64_t delta_sum = 0;
    for (std::uint32_t y = 0; y < tol.height; ++y) {
        for (std::uint32_t x = 0; x < tol.width; ++x) {
            const std::array<int, 4> deltas = channel_deltas(a, b, x, y);
            int pixel_max = 0;
            for (const int delta : deltas) {
                delta_sum += static_cast<std::uint64_t>(delta);
                pixel_max = std::max(pixel_max, delta);
            }
            tol.max_channel_delta = std::max(tol.max_channel_delta, pixel_max);
            if (pixel_max > thresholds.channel_tolerance)
                ++tol.pixels_over;
        }
    }

    const std::uint64_t pixel_count = std::uint64_t{tol.width} * tol.height;
    if (pixel_count > 0) {
        tol.mean_channel_delta =
            static_cast<double>(delta_sum) / static_cast<double>(pixel_count * 4);
        tol.pct_pixels_over =
            100.0 * static_cast<double>(tol.pixels_over) / static_cast<double>(pixel_count);
    }
    tol.pass = tol.dims_match && tol.pct_pixels_over <= thresholds.max_pct_over &&
               tol.mean_channel_delta <= thresholds.max_mean_delta;

    verdict.pass = verdict.hash_equal || tol.pass;
    return verdict;
}

ImageRgba8 diff_image(const ImageRgba8& a, const ImageRgba8& b) {
    ImageRgba8 diff{
        .width = std::max(a.width, b.width), .height = std::max(a.height, b.height), .pixels = {}};
    diff.pixels.resize(diff.byte_size());
    const auto amplify = [](int delta) {
        return static_cast<std::uint8_t>(std::min(255, delta * 8));
    };
    for (std::uint32_t y = 0; y < diff.height; ++y) {
        for (std::uint32_t x = 0; x < diff.width; ++x) {
            const std::array<int, 4> deltas = channel_deltas(a, b, x, y);
            const std::uint8_t alpha_fold = amplify(deltas[3]);
            const std::size_t at = (std::size_t{y} * diff.width + x) * 4;
            diff.pixels[at + 0] = std::max(amplify(deltas[0]), alpha_fold);
            diff.pixels[at + 1] = std::max(amplify(deltas[1]), alpha_fold);
            diff.pixels[at + 2] = std::max(amplify(deltas[2]), alpha_fold);
            diff.pixels[at + 3] = 255;
        }
    }
    return diff;
}

} // namespace midday::rhi

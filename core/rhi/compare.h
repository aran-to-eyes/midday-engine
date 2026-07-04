// core/rhi/compare.h — two-tier image comparison, THE verification pillar's
// comparator (m0-golden-compare; spec section 5 "comparison semantics are
// two-tier").
//
// Tier 1 (hash): XXH3-64 equality over DECODED pixels (image.h::pixel_hash,
//   Aurora D-14) — bit-exact, the gate WITHIN a pinned driver class.
// Tier 2 (tolerance): explicit-threshold metrics over per-channel deltas —
//   the gate ACROSS drivers/backends (m0-rhi-metal's Metal-vs-Vulkan compare
//   consumes exactly this verdict). Metrics, all derived from one pass of
//   pure integer arithmetic (bit-portable on every platform):
//     max_channel_delta   worst |a-b| over every RGBA channel
//     mean_channel_delta  mean |a-b| over every channel (MAE) — the cheap
//                         perceptual metric: catches global shifts
//                         (brightness/gamma drift) that per-pixel counting
//                         forgives once channel_tolerance is raised. A full
//                         SSIM is deliberately NOT computed: windowed
//                         statistics bring tuning constants that would need
//                         their own pinning, and no consumer asks for
//                         structural similarity — MAE + the over-threshold
//                         count already separate noise from damage.
//     pct_pixels_over     % of pixels with ANY channel delta beyond
//                         channel_tolerance — localizes concentrated damage
//                         (a moved edge) that a mean would dilute.
//   pass = dims_match && pct_pixels_over <= max_pct_over
//                     && mean_channel_delta <= max_mean_delta.
//
// Different-sized images compare on the UNION canvas: a pixel present in
// only one image counts as maximally different (delta 255 per channel), and
// tier 2 fails regardless of thresholds (dims_match gates) — "within
// tolerance" between different-sized frames has no meaning, but the metrics
// and diff image still show WHERE the frames disagree.

#pragma once

#include "core/rhi/image.h"

#include <cstdint>

namespace midday::rhi {

// Tier-2 thresholds. Always explicit at the boundary (spec: "thresholds
// explicit"); these defaults are the documented noise-class contract the
// CLI ships: up to 2 LSB per channel is noise, zero pixels beyond that,
// average drift under 1 LSB.
struct ToleranceThresholds {
    int channel_tolerance = 2;   // per-channel |delta| that does not count as "over"
    double max_pct_over = 0.0;   // budget of over-tolerance pixels, in percent
    double max_mean_delta = 1.0; // mean-absolute-delta (MAE) budget
};

struct ToleranceVerdict {
    bool dims_match = false;
    std::uint32_t width = 0;  // union canvas
    std::uint32_t height = 0; // union canvas
    int max_channel_delta = 0;
    double mean_channel_delta = 0.0;
    std::uint64_t pixels_over = 0;
    double pct_pixels_over = 0.0;
    bool pass = false;
};

struct CompareVerdict {
    std::uint64_t hash_a = 0; // pixel_hash of each side, hex64-spelled at JSON
    std::uint64_t hash_b = 0;
    bool hash_equal = false;    // tier 1
    ToleranceVerdict tolerance; // tier 2
    bool pass = false;          // hash_equal || tolerance.pass
};

CompareVerdict
compare_images(const ImageRgba8& a, const ImageRgba8& b, const ToleranceThresholds& thresholds);

// The human-inspectable failure artifact: per-pixel absolute delta,
// amplified x8 and saturated so the noise class is faint-but-visible
// (delta 1 -> 8) and structural damage is white (delta >= 32 -> 255).
// Mapping per pixel: rgb channel c -> min(255, 8*|a.c - b.c|), then the
// ALPHA delta folds into all three rgb channels via max (alpha-only damage
// renders gray instead of staying invisible); diff alpha is always 255.
// Union-canvas semantics as above: one-sided pixels render full white.
ImageRgba8 diff_image(const ImageRgba8& a, const ImageRgba8& b);

} // namespace midday::rhi

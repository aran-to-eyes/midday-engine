// testkit/compare_fixtures.h — THE golden-compare fixture triplet
// (m0-golden-compare), generated deterministically so the committed PNGs
// under testkit/fixtures/goldens/ are regenerable and byte-pinned by the
// verify gate (journal greppable.mrj precedent).
//
// One 24x24 base image; three counterparts, one per comparison verdict:
//   base.png      gradient background + an 8x8 red square at (4,4)
//   identical.png SAME pixels, DIFFERENT PNG bytes (re-filtered encode) —
//                 the pair that pins Aurora D-14: tier-1 hash equality is a
//                 property of the decoded pixels, never of the file bytes.
//   noise.png     base with every third byte's LSB flipped (max delta 1) —
//                 tier-1 fails, tier-2 passes: the driver-noise class.
//   shifted.png   the square moved by (+3,+2) — structural damage, both
//                 tiers fail.

#pragma once

#include "core/rhi/image.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace midday::testkit {

inline constexpr std::uint32_t kCompareFixtureSize = 24;

// Gradient background with an 8x8 (240,40,40) square whose top-left corner
// sits at `square_x, square_y`. Pure function of its arguments.
inline rhi::ImageRgba8 compare_pattern(std::uint32_t square_x, std::uint32_t square_y) {
    rhi::ImageRgba8 image{
        .width = kCompareFixtureSize, .height = kCompareFixtureSize, .pixels = {}};
    image.pixels.resize(image.byte_size());
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t at = (std::size_t{y} * image.width + x) * 4;
            const bool in_square =
                x >= square_x && x < square_x + 8 && y >= square_y && y < square_y + 8;
            image.pixels[at + 0] = in_square ? 240 : static_cast<std::uint8_t>(x * 10);
            image.pixels[at + 1] = in_square ? 40 : static_cast<std::uint8_t>(y * 10);
            image.pixels[at + 2] = in_square ? 40 : 128;
            image.pixels[at + 3] = 255;
        }
    }
    return image;
}

inline rhi::ImageRgba8 compare_base_image() {
    return compare_pattern(4, 4);
}

// 1-LSB perturbation: every third byte XOR 1 — every touched channel moves
// by exactly +/-1, the decoded-noise class a pinned-driver hash catches and
// the tolerance tier forgives.
inline rhi::ImageRgba8 compare_noise_image() {
    rhi::ImageRgba8 image = compare_base_image();
    for (std::size_t i = 0; i < image.pixels.size(); i += 3)
        image.pixels[i] ^= 1;
    return image;
}

// Structural change: the same square, shifted by (+3,+2).
inline rhi::ImageRgba8 compare_shifted_image() {
    return compare_pattern(7, 6);
}

// Writes the four fixture PNGs into `dir` (created if absent). identical.png
// is the SAME pixel data as base.png through the re-filtered encoder path,
// so its bytes differ while its decoded pixels do not.
inline std::optional<base::Error> write_compare_fixtures(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto at = [&dir](const char* name) {
        return (std::filesystem::path(dir) / name).string();
    };
    if (auto error = rhi::write_png(compare_base_image(), at("base.png")))
        return error;
    if (auto error = rhi::write_png_refiltered(compare_base_image(), at("identical.png")))
        return error;
    if (auto error = rhi::write_png(compare_noise_image(), at("noise.png")))
        return error;
    return rhi::write_png(compare_shifted_image(), at("shifted.png"));
}

} // namespace midday::testkit

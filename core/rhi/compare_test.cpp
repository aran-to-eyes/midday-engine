// core/rhi/compare_test.cpp — two-tier image comparison (compare.*).
// Written FIRST (test-first): every metric, threshold gate, the diff-image
// mapping, PNG decode, and the committed fixture triplet are pinned here
// before compare.cpp exists. Tier semantics under test (MILESTONE_0 item 24
// + spec section 5): tier 1 = decoded-pixel hash equality (bit-exact within
// a pinned driver class), tier 2 = explicit-threshold tolerance
// (cross-driver/backend), diff image = amplified per-channel delta map.

#include "core/base/file_io.h"
#include "core/rhi/compare.h"
#include "testkit/compare_fixtures.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace midday;
using namespace midday::rhi;

ImageRgba8 image_from(std::uint32_t width,
                      std::uint32_t height,
                      const std::vector<std::array<std::uint8_t, 4>>& pixels) {
    ImageRgba8 image{.width = width, .height = height, .pixels = {}};
    for (const auto& pixel : pixels)
        image.pixels.insert(image.pixels.end(), pixel.begin(), pixel.end());
    return image;
}

// The 2x2 metric probe: one identical pixel, two 1-LSB pixels (one of them
// in alpha), one pixel 5 off — every metric value is hand-derived.
const ImageRgba8 kProbeA = image_from(
    2, 2, {{100, 150, 200, 255}, {100, 150, 200, 255}, {100, 150, 200, 255}, {100, 150, 200, 255}});
const ImageRgba8 kProbeB = image_from(
    2, 2, {{100, 150, 200, 255}, {101, 149, 200, 255}, {105, 150, 200, 255}, {100, 150, 200, 254}});

TEST_CASE("compare.identical_images_pass_both_tiers") {
    const CompareVerdict verdict =
        compare_images(kProbeA, kProbeA, ToleranceThresholds{.channel_tolerance = 0});
    CHECK(verdict.hash_equal);
    CHECK(verdict.hash_a == verdict.hash_b);
    CHECK(verdict.tolerance.dims_match);
    CHECK(verdict.tolerance.max_channel_delta == 0);
    CHECK(verdict.tolerance.mean_channel_delta == 0.0);
    CHECK(verdict.tolerance.pixels_over == 0);
    CHECK(verdict.tolerance.pct_pixels_over == 0.0);
    CHECK(verdict.tolerance.pass);
    CHECK(verdict.pass);
}

TEST_CASE("compare.metrics_are_exact") {
    // deltas per pixel: {0,0,0,0} {1,1,0,0} {5,0,0,0} {0,0,0,1}
    // sum 8 over 16 channels -> mean 0.5; one pixel over tolerance 2 -> 25%.
    const CompareVerdict verdict = compare_images(
        kProbeA,
        kProbeB,
        ToleranceThresholds{.channel_tolerance = 2, .max_pct_over = 0.0, .max_mean_delta = 1.0});
    CHECK_FALSE(verdict.hash_equal);
    CHECK(verdict.hash_a != verdict.hash_b);
    CHECK(verdict.tolerance.dims_match);
    CHECK(verdict.tolerance.max_channel_delta == 5);
    CHECK(verdict.tolerance.mean_channel_delta == 0.5);
    CHECK(verdict.tolerance.pixels_over == 1);
    CHECK(verdict.tolerance.pct_pixels_over == 25.0);
    CHECK_FALSE(verdict.tolerance.pass); // 25% > the 0% budget
    CHECK_FALSE(verdict.pass);
}

TEST_CASE("compare.each_threshold_gates_independently") {
    const ToleranceThresholds base{
        .channel_tolerance = 2, .max_pct_over = 25.0, .max_mean_delta = 1.0};
    CHECK(compare_images(kProbeA, kProbeB, base).tolerance.pass);

    // Raising the per-channel tolerance to the max delta empties the over set.
    ToleranceThresholds wide = base;
    wide.channel_tolerance = 5;
    wide.max_pct_over = 0.0;
    CHECK(compare_images(kProbeA, kProbeB, wide).tolerance.pass);

    // Tightening the pixel budget below the over percentage fails.
    ToleranceThresholds tight_pct = base;
    tight_pct.max_pct_over = 24.9;
    CHECK_FALSE(compare_images(kProbeA, kProbeB, tight_pct).tolerance.pass);

    // Tightening the mean budget below 0.5 fails even with pixels in budget.
    ToleranceThresholds tight_mean = base;
    tight_mean.max_mean_delta = 0.4;
    CHECK_FALSE(compare_images(kProbeA, kProbeB, tight_mean).tolerance.pass);
}

TEST_CASE("compare.dimension_mismatch_fails_tier2_on_the_union_canvas") {
    // 1x1 vs 2x1: the union canvas is 2x1; the pixel present in only one
    // image counts as maximally different (delta 255 on all four channels).
    const ImageRgba8 small = image_from(1, 1, {{0, 0, 0, 255}});
    const ImageRgba8 wide = image_from(2, 1, {{0, 0, 0, 255}, {9, 9, 9, 255}});
    const CompareVerdict verdict = compare_images(small,
                                                  wide,
                                                  ToleranceThresholds{.channel_tolerance = 255,
                                                                      .max_pct_over = 100.0,
                                                                      .max_mean_delta = 1024.0});
    CHECK_FALSE(verdict.hash_equal);
    CHECK_FALSE(verdict.tolerance.dims_match);
    CHECK(verdict.tolerance.width == 2);
    CHECK(verdict.tolerance.height == 1);
    CHECK(verdict.tolerance.max_channel_delta == 255);
    CHECK(verdict.tolerance.pixels_over == 0); // 255 <= tolerance 255
    CHECK(verdict.tolerance.mean_channel_delta == 1020.0 / 8.0);
    // Mismatched dimensions fail tier 2 REGARDLESS of thresholds: there is
    // no meaningful "within tolerance" between different-sized frames.
    CHECK_FALSE(verdict.tolerance.pass);
    CHECK_FALSE(verdict.pass);
}

TEST_CASE("compare.diff_image_mapping") {
    // Mapping under test (documented in compare.h): rgb channel = |delta|*8
    // saturated at 255; the alpha-channel delta folds into all three rgb
    // channels via max (alpha damage shows gray/white); diff alpha = 255.
    const ImageRgba8 a = image_from(2, 1, {{10, 20, 30, 255}, {0, 200, 0, 255}});
    const ImageRgba8 b = image_from(2, 1, {{12, 20, 30, 251}, {0, 0, 0, 255}});
    const ImageRgba8 diff = diff_image(a, b);
    REQUIRE(diff.width == 2);
    REQUIRE(diff.height == 1);
    // px0: dr=2 -> 16; alpha delta 4 -> 32 folded into every rgb channel.
    CHECK(diff.at(0, 0) == std::array<std::uint8_t, 4>{32, 32, 32, 255});
    // px1: dg=200 -> saturates.
    CHECK(diff.at(1, 0) == std::array<std::uint8_t, 4>{0, 255, 0, 255});

    // Union canvas: a missing pixel renders as full white (delta 255).
    const ImageRgba8 wide = image_from(2, 1, {{10, 20, 30, 255}, {1, 2, 3, 255}});
    const ImageRgba8 narrow = image_from(1, 1, {{10, 20, 30, 255}});
    const ImageRgba8 union_diff = diff_image(wide, narrow);
    REQUIRE(union_diff.width == 2);
    CHECK(union_diff.at(0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
    CHECK(union_diff.at(1, 0) == std::array<std::uint8_t, 4>{255, 255, 255, 255});
}

TEST_CASE("compare.read_png_roundtrip_and_refusals") {
    testkit::TempDir dir("compare-read");
    const ImageRgba8 original = testkit::compare_base_image();
    const std::string path = dir.file("roundtrip.png");
    REQUIRE_FALSE(write_png(original, path).has_value());

    // PNG is lossless: the decode is the exact inverse of the encode, so
    // the decoded-pixel hash survives the file trip (Aurora D-14's premise).
    const ImageReadResult back = read_png(path);
    REQUIRE(back.ok());
    CHECK(back.image.width == original.width);
    CHECK(back.image.height == original.height);
    CHECK(back.image.pixels == original.pixels);
    CHECK(pixel_hash(back.image) == pixel_hash(original));

    const ImageReadResult missing = read_png(dir.file("nope.png"));
    REQUIRE_FALSE(missing.ok());
    CHECK(testkit::unwrap(missing.error).code == "rhi.image_read");

    const std::string garbage = dir.file("garbage.png");
    REQUIRE_FALSE(base::write_file(garbage, "not a png at all", "test.io").has_value());
    const ImageReadResult broken = read_png(garbage);
    REQUIRE_FALSE(broken.ok());
    CHECK(testkit::unwrap(broken.error).code == "rhi.image_read");
}

TEST_CASE("compare.fixtures") {
    // Regenerates the committed testkit/fixtures/goldens/ triplet. The
    // verify gate byte-compares a regeneration against the committed files
    // (set MIDDAY_COMPARE_FIXTURE_DIR, journal greppable.mrj precedent);
    // here the pairs are verified to MEAN what the exit tests claim.
    testkit::TempDir tmp("compare-fixtures");
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* fixture_dir = std::getenv("MIDDAY_COMPARE_FIXTURE_DIR");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    const std::string dir = fixture_dir != nullptr ? std::string(fixture_dir) : tmp.file("out");
    REQUIRE_FALSE(testkit::write_compare_fixtures(dir).has_value());

    const auto slurp = [&dir](const char* name) {
        base::ReadFileResult read = base::read_file(std::filesystem::path(dir) / name, "test.io");
        REQUIRE_FALSE(read.error.has_value());
        return read.bytes;
    };
    const auto decode = [&dir](const char* name) {
        ImageReadResult read = read_png((std::filesystem::path(dir) / name).string());
        REQUIRE(read.ok());
        return read.image;
    };

    // Identical pair: DIFFERENT file bytes, IDENTICAL decoded pixels — the
    // committed proof that tier 1 hashes pixels, never encodings (D-14).
    CHECK(slurp("base.png") != slurp("identical.png"));
    const ImageRgba8 base_image = decode("base.png");
    const ImageRgba8 identical = decode("identical.png");
    CHECK(base_image.pixels == identical.pixels);
    CHECK(pixel_hash(base_image) == pixel_hash(identical));
    CHECK(base_image.pixels == testkit::compare_base_image().pixels);

    const ToleranceThresholds defaults{};
    const CompareVerdict same = compare_images(base_image, identical, defaults);
    CHECK(same.hash_equal);
    CHECK(same.tolerance.pass);
    CHECK(same.pass);

    // Noise pair: tier 1 fails, tier 2 passes — max delta is exactly 1.
    const CompareVerdict noise = compare_images(base_image, decode("noise.png"), defaults);
    CHECK_FALSE(noise.hash_equal);
    CHECK(noise.tolerance.max_channel_delta == 1);
    CHECK(noise.tolerance.pixels_over == 0);
    CHECK(noise.tolerance.pass);
    CHECK(noise.pass);

    // Structural pair: both tiers fail.
    const CompareVerdict shifted = compare_images(base_image, decode("shifted.png"), defaults);
    CHECK_FALSE(shifted.hash_equal);
    CHECK(shifted.tolerance.pct_pixels_over > 0.0);
    CHECK_FALSE(shifted.tolerance.pass);
    CHECK_FALSE(shifted.pass);
}

} // namespace

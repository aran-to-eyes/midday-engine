// core/rhi/vulkan/vulkan_test.cpp — the Vulkan backend against the three M0
// scenes (rhi.vk.*). PLATFORM REALITY: many dev hosts have no Vulkan ICD.
// These tests SKIP loudly (stderr line + doctest message, counted and
// reported by rhi.vk.zz_skip_report) when no device comes up — skip is
// never a silent pass, and the CI lanes are the enforcement point:
// golden-software (lavapipe) runs them with an ICD guaranteed present.
//
// Two-tier pixel truth (scenes.h): structural assertions hold on ANY
// conformant driver and run wherever an ICD exists; exact hash comparison
// against testkit/goldens/m0 runs only when the active driver matches the
// committed DRIVER_PIN.txt (the lavapipe class the goldens were minted on).
// Note: this file contains no Vulkan includes — the backend is exercised
// entirely through its clean factory header (the boundary, demonstrated).

#include "core/base/hex.h"
#include "core/rhi/device.h"
#include "core/rhi/goldens.h"
#include "core/rhi/scenes.h"
#include "core/rhi/vulkan/vulkan_device.h"
#include "testkit/doctest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {

using namespace midday;
using namespace midday::rhi;

// tidy-clean optional access: the code of an expected error, or a marker
// that fails the string compare when the optional is empty.
std::string err_code(std::optional<midday::base::Error> error) {
    return error.has_value() ? error->code : std::string("(no error)");
}

std::string failure_reason(std::optional<midday::base::Error> error) {
    return error.has_value() ? error->message : std::string("unknown");
}

struct VkTestContext {
    DeviceResult result;
    int skipped = 0;

    VkTestContext() : result(create_vulkan_device({})) {}
};

VkTestContext& context() {
    static VkTestContext ctx; // one device for the whole rhi.vk suite
    return ctx;
}

// Returns the shared device or nullptr after logging a LOUD skip.
RhiDevice* acquire(const char* test_name) {
    VkTestContext& ctx = context();
    if (!ctx.result.ok()) {
        ++ctx.skipped;
        const std::string reason = failure_reason(ctx.result.error);
        std::fprintf(stderr, "rhi.vk SKIP %s — %s\n", test_name, reason.c_str());
        MESSAGE("SKIPPED (no usable Vulkan device): " << reason);
        return nullptr;
    }
    return ctx.result.device.get();
}

// Golden dir: MIDDAY_RHI_GOLDEN_DIR (CI lane / verify set it) or the repo
// default when running from the repo root. Absent dir = no hash gate here.
std::string golden_dir() {
    // push/disable/pop, not warning(suppress): an #endif eats a suppress
    // (journal writer_test precedent, D-BUILD ledger 2026-07-03).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* env = std::getenv("MIDDAY_RHI_GOLDEN_DIR");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (env != nullptr && *env != '\0')
        return env;
    const std::string fallback = "testkit/goldens/m0";
    return std::filesystem::exists(fallback) ? fallback : std::string();
}

bool channel_dominant(const std::array<std::uint8_t, 4>& px, int channel) {
    for (int c = 0; c < 3; ++c)
        if (c != channel &&
            px[static_cast<std::size_t>(channel)] <= px[static_cast<std::size_t>(c)])
            return false;
    return true;
}

TEST_CASE("rhi.vk.device_probe_reports_caps") {
    RhiDevice* device = acquire("rhi.vk.device_probe_reports_caps");
    if (device == nullptr)
        return;
    const DeviceCaps& caps = device->caps();
    CHECK(caps.backend == "vulkan");
    CHECK_FALSE(caps.device_name.empty());
    CHECK_FALSE(caps.api_version.empty());
    CHECK(caps.max_texture_size >= kSceneExtent);
    MESSAGE("vulkan device: " << caps.device_name << " | driver: " << caps.driver_info << " | api: "
                              << caps.api_version << " | software: " << caps.software_rasterizer);
}

TEST_CASE("rhi.vk.stale_handle_detected_by_real_backend") {
    RhiDevice* device = acquire("rhi.vk.stale_handle_detected_by_real_backend");
    if (device == nullptr)
        return;
    BufferResult buffer = device->create_buffer({.size_bytes = 16, .debug_name = "stale"}, {});
    REQUIRE(buffer.ok());
    CHECK_FALSE(device->destroy_buffer(buffer.handle).has_value());
    CHECK(err_code(device->destroy_buffer(buffer.handle)) == "rhi.stale_handle");
}

TEST_CASE("rhi.vk.shader_compile_errors_are_structured") {
    RhiDevice* device = acquire("rhi.vk.shader_compile_errors_are_structured");
    if (device == nullptr)
        return;
    ShaderResult bad = device->create_shader(
        {.stage = ShaderStage::kVertex, .glsl = "#version 450\nbroken", .debug_name = "bad"});
    REQUIRE_FALSE(bad.ok());
    CHECK(err_code(bad.error) == "rhi.shader_compile");
}

TEST_CASE("rhi.vk.clear_scene_is_pixel_exact") {
    RhiDevice* device = acquire("rhi.vk.clear_scene_is_pixel_exact");
    if (device == nullptr)
        return;
    SceneRender render = render_scene(*device, SceneId::kClear);
    REQUIRE(render.ok());
    REQUIRE(render.image.width == kSceneExtent);
    // The pinned clear color has no UNORM rounding ties: exact on every
    // conformant implementation, not just the pinned CI class.
    CHECK(render.image.at(0, 0) == kClearRgba);
    CHECK(render.image.at(kSceneExtent / 2, kSceneExtent / 2) == kClearRgba);
    CHECK(render.image.at(kSceneExtent - 1, kSceneExtent - 1) == kClearRgba);
}

TEST_CASE("rhi.vk.triangle_scene_structure") {
    RhiDevice* device = acquire("rhi.vk.triangle_scene_structure");
    if (device == nullptr)
        return;
    SceneRender render = render_scene(*device, SceneId::kTriangle);
    REQUIRE(render.ok());
    const ImageRgba8& img = render.image;
    // Corners lie outside the triangle: exact background.
    CHECK(img.at(2, 2) == kClearRgba);
    CHECK(img.at(kSceneExtent - 3, 2) == kClearRgba);
    CHECK(img.at(2, kSceneExtent - 3) == kClearRgba);
    CHECK(img.at(kSceneExtent - 3, kSceneExtent - 3) == kClearRgba);
    // Near the apex (top center, y-down NDC -0.6 -> row ~51): red dominates.
    CHECK(channel_dominant(img.at(128, 70), 0));
    // Near bottom-right vertex (0.6, 0.6) -> (~205, ~205): green dominates.
    CHECK(channel_dominant(img.at(195, 195), 1));
    // Near bottom-left vertex (-0.6, 0.6) -> (~51, ~205): blue dominates.
    CHECK(channel_dominant(img.at(61, 195), 2));
    // Interior pixel is not background.
    CHECK_FALSE(img.at(128, 128) == kClearRgba);
}

TEST_CASE("rhi.vk.textured_quad_structure") {
    RhiDevice* device = acquire("rhi.vk.textured_quad_structure");
    if (device == nullptr)
        return;
    SceneRender render = render_scene(*device, SceneId::kTexturedQuad);
    REQUIRE(render.ok());
    const ImageRgba8& img = render.image;
    // Outside the quad ([-0.75,0.75] -> pixels [32,224)): background.
    CHECK(img.at(10, 128) == kClearRgba);
    CHECK(img.at(128, 10) == kClearRgba);
    CHECK(img.at(245, 128) == kClearRgba);
    // Checker cell centers map to texel centers (192px quad / 8 cells =
    // 24px per cell; center of cell (i,j) = 44 + 24i): nearest sampling
    // returns the exact texel bytes on any conformant driver.
    CHECK(img.at(44, 44) == kCheckerColorA);                   // cell (0,0)
    CHECK(img.at(68, 44) == kCheckerColorB);                   // cell (1,0)
    CHECK(img.at(44, 68) == kCheckerColorB);                   // cell (0,1)
    CHECK(img.at(68, 68) == kCheckerColorA);                   // cell (1,1)
    CHECK(img.at(44 + 24 * 7, 44 + 24 * 7) == kCheckerColorA); // cell (7,7)
}

TEST_CASE("rhi.vk.dual_render_hashes_identical") {
    RhiDevice* device = acquire("rhi.vk.dual_render_hashes_identical");
    if (device == nullptr)
        return;
    // Two INDEPENDENT renders (fresh resources each) diffed — same device,
    // same bytes. Cross-driver identity is deliberately NOT claimed here.
    SceneRender first = render_scene(*device, SceneId::kTriangle);
    SceneRender second = render_scene(*device, SceneId::kTriangle);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(pixel_hash(first.image) == pixel_hash(second.image));
}

TEST_CASE("rhi.vk.goldens_match_within_pinned_driver_class") {
    RhiDevice* device = acquire("rhi.vk.goldens_match_within_pinned_driver_class");
    if (device == nullptr)
        return;
    const std::string dir = golden_dir();
    if (dir.empty()) {
        MESSAGE("no golden dir (env MIDDAY_RHI_GOLDEN_DIR unset, no testkit/goldens/m0 in cwd)");
        return;
    }
    const std::optional<std::string> pin = read_driver_pin(dir);
    if (!pin.has_value()) {
        MESSAGE("goldens not minted yet (no DRIVER_PIN.txt in " << dir << ") — bootstrap pending");
        return;
    }
    if (*pin != device->caps().driver_info) {
        MESSAGE("driver '" << device->caps().driver_info << "' is not the pinned class '" << *pin
                           << "' — hash gate skipped (structural tests still ran)");
        return;
    }
    for (SceneId scene : kAllScenes) {
        INFO("scene: " << std::string(to_string(scene)));
        const std::string want =
            read_golden_hash(dir, scene).value_or("(golden hash file missing)");
        REQUIRE_MESSAGE(want != "(golden hash file missing)",
                        "golden hash file missing for pinned driver");
        SceneRender render = render_scene(*device, scene);
        REQUIRE(render.ok());
        CHECK(base::hex64(pixel_hash(render.image)) == want);
    }
}

// Keep LAST in this file: reports the suite's skip count loudly. Skips are
// expected on ICD-less dev machines and FORBIDDEN in the golden-software
// lane, which gates via `midday rhi probe`/`render` before trusting tests.
TEST_CASE("rhi.vk.zz_skip_report") {
    VkTestContext& ctx = context();
    if (ctx.result.ok()) {
        MESSAGE("rhi.vk suite ran against: " << ctx.result.device->caps().device_name);
        CHECK(ctx.skipped == 0);
    } else {
        const std::string reason = failure_reason(ctx.result.error);
        std::fprintf(stderr,
                     "rhi.vk: %d case(s) SKIPPED — no usable Vulkan device: %s\n",
                     ctx.skipped,
                     reason.c_str());
        MESSAGE("rhi.vk skipped " << ctx.skipped << " case(s): " << reason);
    }
}

} // namespace

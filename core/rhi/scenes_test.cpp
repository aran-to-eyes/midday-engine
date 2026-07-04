// core/rhi/scenes_test.cpp — the three M0 scenes driven through the seam
// against NullDevice (rhi.scenes.*): the full record -> submit -> readback
// protocol runs CPU-only on every platform; pixel truth for real
// rasterizers lives in rhi.vk.* (and rhi.metal.* at m0-rhi-metal).

#include "core/rhi/null_device.h"
#include "core/rhi/scenes.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <cstdint>
#include <string>

namespace {

using namespace midday::rhi;

TEST_CASE("rhi.scenes.names_round_trip") {
    for (SceneId scene : kAllScenes) {
        auto parsed = scene_from_name(to_string(scene));
        CHECK(midday::testkit::unwrap(parsed) == scene);
    }
    CHECK_FALSE(scene_from_name("no_such_scene").has_value());
}

TEST_CASE("rhi.scenes.checkerboard_pins") {
    const ImageRgba8 checker = checkerboard_image();
    CHECK(checker.width == kCheckerTextureExtent);
    CHECK(checker.height == kCheckerTextureExtent);
    // Cell (0,0) is color A; neighbors alternate; cell extent is 8 texels.
    CHECK(checker.at(0, 0) == kCheckerColorA);
    CHECK(checker.at(7, 7) == kCheckerColorA);   // still cell (0,0)
    CHECK(checker.at(8, 0) == kCheckerColorB);   // cell (1,0)
    CHECK(checker.at(0, 8) == kCheckerColorB);   // cell (0,1)
    CHECK(checker.at(8, 8) == kCheckerColorA);   // cell (1,1)
    CHECK(checker.at(63, 63) == kCheckerColorA); // cell (7,7)
    // Deterministic: the generator is a pure function.
    CHECK(pixel_hash(checker) == pixel_hash(checkerboard_image()));
}

TEST_CASE("rhi.scenes.clear_scene_on_null_device_is_pixel_exact") {
    NullDevice device;
    SceneRender render = render_scene(device, SceneId::kClear);
    REQUIRE(render.ok());
    REQUIRE(render.image.width == kSceneExtent);
    REQUIRE(render.image.height == kSceneExtent);
    CHECK(render.image.at(0, 0) == kClearRgba);
    CHECK(render.image.at(kSceneExtent / 2, kSceneExtent / 2) == kClearRgba);
    CHECK(render.image.at(kSceneExtent - 1, kSceneExtent - 1) == kClearRgba);

    // Dual-run determinism: two renders, two devices, one hash.
    NullDevice second;
    SceneRender rerun = render_scene(second, SceneId::kClear);
    REQUIRE(rerun.ok());
    CHECK(pixel_hash(render.image) == pixel_hash(rerun.image));
}

TEST_CASE("rhi.scenes.geometry_scenes_run_the_full_protocol") {
    NullDevice device;
    for (SceneId scene : {SceneId::kTriangle, SceneId::kTexturedQuad}) {
        INFO("scene: " << std::string(to_string(scene)));
        SceneRender render = render_scene(device, scene);
        REQUIRE(render.ok());
        // NullDevice rasterizes nothing: geometry scenes read back as the
        // clear color — the protocol truth, not a pixel truth.
        CHECK(render.image.at(kSceneExtent / 2, kSceneExtent / 2) == kClearRgba);
    }
    // One draw per geometry scene reached submit.
    CHECK(device.submitted_draws() == 2);
}

TEST_CASE("rhi.scenes.no_resource_leaks") {
    NullDevice device;
    for (SceneId scene : kAllScenes) {
        SceneRender render = render_scene(device, scene);
        REQUIRE(render.ok());
    }
    // render_scene leaves the device exactly as it found it.
    CHECK(device.live_counts().total() == 0);
}

} // namespace

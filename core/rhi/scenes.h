// core/rhi/scenes.h — the three pinned M0 render scenes (m0-rhi-vulkan):
// solid clear, single triangle, textured quad. They are the seam's
// conformance corpus: rendered on lavapipe for the golden-software lane,
// on Metal for the m0-rhi-metal cross-backend compare, and against
// NullDevice for CPU-only protocol tests — the SAME code drives every
// backend because scenes speak only rhi::RhiDevice.
//
// Every constant here is PINNED: changing any of them invalidates the
// committed goldens under testkit/goldens/m0/ and requires re-minting
// through the golden-software candidate flow (see that directory's README).
//
// Two-tier assertion scheme (spec section 5 comparison semantics):
//   * STRUCTURAL truths hold on ANY conformant rasterizer and are asserted
//     by selftests everywhere: exact clear bytes (the UNORM8 conversion of
//     the pinned clear color is unambiguous), background pixels outside
//     geometry, color dominance at triangle corners, exact texel colors at
//     checkerboard cell centers (nearest sampling at cell centers).
//   * HASH-EQUAL (pixel_hash vs testkit/goldens/m0) applies only within the
//     pinned CI driver class (lavapipe; DRIVER_PIN.txt) — GPU output is not
//     bit-identical across drivers and the gate never pretends it is.

#pragma once

#include "core/rhi/device.h"
#include "core/rhi/image.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace midday::rhi {

enum class SceneId : std::uint8_t { kClear, kTriangle, kTexturedQuad };

inline constexpr std::array<SceneId, 3> kAllScenes = {
    SceneId::kClear, SceneId::kTriangle, SceneId::kTexturedQuad};

[[nodiscard]] std::string_view to_string(SceneId scene); // clear|triangle|textured_quad
[[nodiscard]] std::optional<SceneId> scene_from_name(std::string_view name);

// -- Pins -------------------------------------------------------------------

// Render target: 256x256 RGBA8.
inline constexpr std::uint32_t kSceneExtent = 256;

// Clear color, chosen so UNORM8 conversion is unambiguous on every
// conformant implementation (no .5 rounding ties): bytes (51, 102, 153, 255).
inline constexpr ClearColor kClearColor{0.2F, 0.4F, 0.6F, 1.0F};
inline constexpr std::array<std::uint8_t, 4> kClearRgba = {51, 102, 153, 255};

// Triangle: NDC per the seam contract (y DOWN). Apex top-center red, then
// bottom-right green, bottom-left blue. Cull is NONE (types.h pinned state),
// so winding can never make it vanish on a conforming backend.
// Interleaved [x, y, r, g, b] * 3.
inline constexpr std::array<float, 15> kTriangleVertices = {
    0.0F,
    -0.6F,
    1.0F,
    0.0F,
    0.0F, // apex (top center): red
    0.6F,
    0.6F,
    0.0F,
    1.0F,
    0.0F, // bottom right: green
    -0.6F,
    0.6F,
    0.0F,
    0.0F,
    1.0F, // bottom left: blue
};

// Textured quad: axis-aligned, NDC [-0.75, 0.75]^2 (pixel columns/rows
// [32, 224) at 256px), uv (0,0) at the top-left corner. Two triangles,
// interleaved [x, y, u, v] * 6.
inline constexpr float kQuadHalfExtentNdc = 0.75F;
inline constexpr std::array<float, 24> kQuadVertices = {
    -0.75F, -0.75F, 0.0F, 0.0F, /**/ 0.75F, -0.75F, 1.0F, 0.0F, /**/ 0.75F,  0.75F, 1.0F, 1.0F,
    -0.75F, -0.75F, 0.0F, 0.0F, /**/ 0.75F, 0.75F,  1.0F, 1.0F, /**/ -0.75F, 0.75F, 0.0F, 1.0F,
};

// Procedural checkerboard: 64x64 texels, 8x8 cells (8 texels per cell),
// nearest sampling, clamp-to-edge. Cell (0,0) — the top-left — is color A.
// The two colors are deliberately channel-asymmetric so an RGBA/BGRA swap
// anywhere in a backend shows up as a hard color mismatch, not a gray-equal.
inline constexpr std::uint32_t kCheckerTextureExtent = 64;
inline constexpr std::uint32_t kCheckerCellsPerSide = 8;
inline constexpr std::array<std::uint8_t, 4> kCheckerColorA = {204, 51, 51, 255}; // red-ish
inline constexpr std::array<std::uint8_t, 4> kCheckerColorB = {51, 51, 204, 255}; // blue-ish

// The checkerboard pixel generator (also what tests compare texel pins
// against — one authority for the pattern).
[[nodiscard]] ImageRgba8 checkerboard_image();

// -- Rendering --------------------------------------------------------------

struct SceneRender {
    ImageRgba8 image{};
    std::optional<base::Error> error = std::nullopt;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Renders `scene` on `device` at kSceneExtent^2, synchronously:
// create resources -> record -> submit_and_wait -> read_texture -> destroy.
// The device is left with exactly the resources it had before the call.
[[nodiscard]] SceneRender render_scene(RhiDevice& device, SceneId scene);

} // namespace midday::rhi

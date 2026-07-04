// core/rhi/types.h — the seam's value vocabulary: formats, descriptors,
// capability flags, result envelopes (m0-rhi-vulkan). Everything here is
// backend-agnostic plain data; no GPU API type ever appears above the seam
// (scripts/check_include_boundaries.py enforces).
//
// Coordinate contract (pinned for every backend): clip space is the Vulkan
// convention — x right, y DOWN, z in [0, 1]. Texture row 0 is the TOP image
// row, and readback returns tightly packed RGBA8 rows top-to-bottom. The
// Metal backend (m0-rhi-metal) adapts internally; the seam never leaks a
// per-backend axis flip.

#pragma once

#include "core/base/error.h"
#include "core/rhi/handle.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::rhi {

// Error codes returned by seam implementations (structured base::Error, the
// tree-wide envelope). Backends must use exactly these spellings so seam
// tests written against NullDevice hold for every backend:
//   rhi.unavailable         backend cannot come up (no loader, no ICD, ...)
//   rhi.validation_unavailable  validation explicitly requested, layer absent
//   rhi.null_handle         a null handle where a live resource is required
//   rhi.stale_handle        handle to a destroyed resource (generation miss)
//   rhi.invalid_argument    malformed descriptor (zero size, bad extent, ...)
//   rhi.unsupported         valid request the backend/device cannot honor
//   rhi.shader_compile      GLSL -> SPIR-V front-end refusal (log in details)
//   rhi.already_recording   begin() on a list that is recording
//   rhi.not_recording       cmd_* on a list that is not recording
//   rhi.pass_active         end()/begin_render_pass() with a pass still open
//   rhi.no_pass             draw/bind outside a render pass
//   rhi.no_pipeline         draw with no pipeline bound in this pass
//   rhi.no_vertex_buffer    draw with no vertex buffer bound in this pass
//   rhi.texture_missing     draw on a texture pipeline with no texture bound
//   rhi.not_ready           submit of a list that is not in the ready state
//   rhi.size_mismatch       readback span size != texture byte size
//   rhi.device_fault        backend runtime failure (VkResult etc. in details)

// ---------------------------------------------------------------------------
// Enums. Deliberately small: every value listed is implemented by every
// backend; capability growth adds values WITH their implementations.

enum class TextureFormat : std::uint8_t {
    kRGBA8Unorm, // the M0 golden transport format: 4 bytes/pixel, sRGB-free
};

enum class TextureUsage : std::uint8_t {
    kSampled,      // shader-read + upload target
    kRenderTarget, // color attachment + readback source
};

enum class FilterMode : std::uint8_t { kNearest, kLinear };
enum class AddressMode : std::uint8_t { kClampToEdge, kRepeat };

enum class BufferUsage : std::uint8_t {
    kVertex,
};

enum class ShaderStage : std::uint8_t { kVertex, kFragment };

enum class VertexFormat : std::uint8_t { kFloat2, kFloat3, kFloat4 };

enum class PrimitiveTopology : std::uint8_t { kTriangleList };

// ---------------------------------------------------------------------------
// Descriptors: pipeline-as-data (spec section 5 SRP direction) — a pipeline
// is a value describing state, never a builder with hidden defaults.

struct BufferDesc {
    std::uint64_t size_bytes = 0;
    BufferUsage usage = BufferUsage::kVertex;
    std::string debug_name{}; // surfaced in errors and backend debug labels
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::kRGBA8Unorm;
    TextureUsage usage = TextureUsage::kSampled;
    std::string debug_name{};
};

struct SamplerDesc {
    FilterMode filter = FilterMode::kNearest; // both minify and magnify (M0)
    AddressMode address = AddressMode::kClampToEdge;
    std::string debug_name{};
};

// Shaders enter the seam as GLSL SOURCE (spec section 5: GLSL + SPIR-V via
// glslang). The backend compiles at create_shader — runtime compile in M0;
// the shader/pipeline cache is a later SRP node.
struct ShaderDesc {
    ShaderStage stage = ShaderStage::kVertex;
    std::string glsl{}; // Vulkan-flavored GLSL, #version 450
    std::string entry_point = "main";
    std::string debug_name{};
};

struct VertexAttribute {
    std::uint32_t location = 0; // GLSL layout(location = N)
    VertexFormat format = VertexFormat::kFloat2;
    std::uint32_t offset_bytes = 0;
};

// One vertex buffer binding (binding 0), interleaved attributes.
struct VertexLayout {
    std::uint32_t stride_bytes = 0;
    std::vector<VertexAttribute> attributes{};
};

struct PipelineDesc {
    ShaderHandle vertex_shader{};   // snapshot: destroying the shader after
    ShaderHandle fragment_shader{}; // create_pipeline leaves the pipeline valid
    VertexLayout vertex_layout{};
    PrimitiveTopology topology = PrimitiveTopology::kTriangleList;
    TextureFormat color_format = TextureFormat::kRGBA8Unorm;
    // M0 binding model: at most ONE combined image sampler at (set 0,
    // binding 0), fragment stage. True = draws require cmd_bind_texture.
    bool uses_texture = false;
    // Fixed M0 state, pinned rather than configurable: no depth, no blend,
    // cull NONE (no winding-order portability trap), front face CCW.
    std::string debug_name{};
};

struct ClearColor {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

// A render pass in M0: one color target, always cleared on load, stored on
// end. The render-graph document layers on top of this later (SRP).
struct RenderPassDesc {
    TextureHandle color_target{}; // usage must be kRenderTarget
    ClearColor clear{};
};

// ---------------------------------------------------------------------------
// Capability surface. Honest and minimal: fields describe what THIS seam
// exposes today, not what the API underneath could do. Grows with the seam.

struct DeviceCaps {
    std::string backend{};     // "vulkan" | "metal" | "null"
    std::string device_name{}; // e.g. "llvmpipe (LLVM 17.0.6, 256 bits)"
    std::string driver_info{}; // driver name + version string
    std::string api_version{}; // e.g. "1.3.278"
    std::uint32_t max_texture_size = 0;
    bool software_rasterizer = false; // lavapipe/SwiftShader class
    bool validation_enabled = false;
};

// ---------------------------------------------------------------------------
// Result envelopes (journal ReaderOpenResult precedent): value + optional
// structured error. Errors are values, never exceptions across the seam.

template <typename H> struct HandleResult {
    H handle{};
    std::optional<base::Error> error = std::nullopt;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

using BufferResult = HandleResult<BufferHandle>;
using TextureResult = HandleResult<TextureHandle>;
using SamplerResult = HandleResult<SamplerHandle>;
using ShaderResult = HandleResult<ShaderHandle>;
using PipelineResult = HandleResult<PipelineHandle>;
using CommandListResult = HandleResult<CommandListHandle>;

class RhiDevice;

struct DeviceResult {
    std::unique_ptr<RhiDevice> device = nullptr;
    std::optional<base::Error> error = std::nullopt;

    [[nodiscard]] bool ok() const { return device != nullptr; }
};

// Bytes per pixel of a format (all M0 formats are 4).
[[nodiscard]] constexpr std::uint32_t bytes_per_pixel(TextureFormat format) {
    switch (format) {
    case TextureFormat::kRGBA8Unorm:
        return 4;
    }
    return 4; // unreachable: enum is exhaustive
}

[[nodiscard]] std::string_view to_string(ShaderStage stage);

} // namespace midday::rhi

// core/rhi/null_device.h — rhi::NullDevice: the seam without a GPU
// (m0-rhi-vulkan). Purpose, in order:
//
//   1. Prove the SEAM SEMANTICS CPU-only, everywhere: handle generations and
//      LIFO reuse, structured error paths, the record -> submit state
//      machine — the rhi.null selftests run on every platform and lane, ICD
//      or not, and what they prove holds for every backend because the
//      validation is the SHARED CommandListState + HandlePool code.
//   2. Serve as the device-shaped test double for later nodes (m2+ render
//      graph, golden-compare tooling) that need deterministic "rendering"
//      without GPU variance.
//
// Execution model (deliberately minimal, deterministic by construction):
// textures hold CPU pixel memory; begin_render_pass CLEARS the target to the
// pass's clear color (exact round-to-nearest UNORM8); draws are validated
// no-ops (nothing rasterizes); read_texture returns the pixel memory.
// A NullDevice "render" of the clear scene is therefore pixel-exact; scenes
// with geometry come back as their clear color — which is precisely what a
// seam-semantics double should do: exercise every path, invent no pixels.

#pragma once

#include "core/rhi/command_state.h"
#include "core/rhi/device.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace midday::rhi {

class NullDevice final : public RhiDevice {
public:
    NullDevice();

    [[nodiscard]] const DeviceCaps& caps() const override;

    [[nodiscard]] BufferResult create_buffer(const BufferDesc& desc,
                                             std::span<const std::byte> initial_data) override;
    [[nodiscard]] TextureResult create_texture(const TextureDesc& desc,
                                               std::span<const std::byte> initial_data) override;
    [[nodiscard]] SamplerResult create_sampler(const SamplerDesc& desc) override;
    [[nodiscard]] ShaderResult create_shader(const ShaderDesc& desc) override;
    [[nodiscard]] PipelineResult create_pipeline(const PipelineDesc& desc) override;

    std::optional<base::Error> destroy_buffer(BufferHandle handle) override;
    std::optional<base::Error> destroy_texture(TextureHandle handle) override;
    std::optional<base::Error> destroy_sampler(SamplerHandle handle) override;
    std::optional<base::Error> destroy_shader(ShaderHandle handle) override;
    std::optional<base::Error> destroy_pipeline(PipelineHandle handle) override;

    [[nodiscard]] CommandListResult create_command_list() override;
    std::optional<base::Error> destroy_command_list(CommandListHandle handle) override;

    std::optional<base::Error> cmd_begin(CommandListHandle list) override;
    std::optional<base::Error> cmd_begin_render_pass(CommandListHandle list,
                                                     const RenderPassDesc& pass) override;
    std::optional<base::Error> cmd_bind_pipeline(CommandListHandle list,
                                                 PipelineHandle pipeline) override;
    std::optional<base::Error> cmd_bind_vertex_buffer(CommandListHandle list,
                                                      BufferHandle buffer) override;
    std::optional<base::Error> cmd_bind_texture(CommandListHandle list,
                                                std::uint32_t slot,
                                                TextureHandle texture,
                                                SamplerHandle sampler) override;
    std::optional<base::Error> cmd_draw(CommandListHandle list,
                                        std::uint32_t vertex_count,
                                        std::uint32_t first_vertex) override;
    std::optional<base::Error> cmd_end_render_pass(CommandListHandle list) override;
    std::optional<base::Error> cmd_end(CommandListHandle list) override;

    std::optional<base::Error> submit_and_wait(CommandListHandle list) override;
    std::optional<base::Error> read_texture(TextureHandle texture,
                                            std::span<std::byte> out) override;

    // Test introspection: draws recorded since the last cmd_begin on any
    // list that reached submit (cumulative across submits).
    [[nodiscard]] std::uint64_t submitted_draws() const { return submitted_draws_; }

    // Live-resource census (leak assertions in scene/protocol tests).
    struct Counts {
        std::uint32_t buffers = 0;
        std::uint32_t textures = 0;
        std::uint32_t samplers = 0;
        std::uint32_t shaders = 0;
        std::uint32_t pipelines = 0;
        std::uint32_t command_lists = 0;

        [[nodiscard]] std::uint32_t total() const {
            return buffers + textures + samplers + shaders + pipelines + command_lists;
        }
    };

    [[nodiscard]] Counts live_counts() const {
        return Counts{buffers_.live_count(),
                      textures_.live_count(),
                      samplers_.live_count(),
                      shaders_.live_count(),
                      pipelines_.live_count(),
                      lists_.live_count()};
    }

private:
    struct NullBuffer {
        BufferDesc desc;
        std::vector<std::byte> data;
    };

    struct NullTexture {
        TextureDesc desc;
        std::vector<std::byte> pixels; // RGBA8, tightly packed
    };

    struct NullSampler {
        SamplerDesc desc;
    };

    struct NullShader {
        ShaderDesc desc; // source held, never compiled (no glslang dependency)
    };

    struct NullPipeline {
        PipelineDesc desc;
    };

    struct NullCommandList {
        CommandListState state;
        std::uint32_t pending_draws = 0;
    };

    NullCommandList* live_list(CommandListHandle handle, std::optional<base::Error>& error);

    DeviceCaps caps_;
    HandlePool<BufferHandleTag, NullBuffer> buffers_;
    HandlePool<TextureHandleTag, NullTexture> textures_;
    HandlePool<SamplerHandleTag, NullSampler> samplers_;
    HandlePool<ShaderHandleTag, NullShader> shaders_;
    HandlePool<PipelineHandleTag, NullPipeline> pipelines_;
    HandlePool<CommandListHandleTag, NullCommandList> lists_;
    std::uint64_t submitted_draws_ = 0;
};

// The shared "which resource is missing/stale" error builders — used by
// every backend so wording and details are identical tree-wide.
[[nodiscard]] base::Error null_handle_error(std::string_view kind);
[[nodiscard]] base::Error stale_handle_error(std::string_view kind, std::uint64_t bits);

// Exact UNORM8 conversion used by NullDevice clears and pinned in tests:
// round-to-nearest of clamp(value, 0, 1) * 255.
[[nodiscard]] std::uint8_t unorm8_from_float(float value);

} // namespace midday::rhi

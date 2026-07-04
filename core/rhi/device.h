// core/rhi/device.h — RhiDevice, the hard multi-backend boundary (spec
// section 5; m0-rhi-vulkan). Everything the engine ever says to a GPU goes
// through this interface; no code above the seam includes a backend header
// (scripts/check_include_boundaries.py + the CI boundaries lane enforce).
//
// M0 scope — the seam only: resource creation, command recording, ONE
// synchronous record -> submit -> wait -> readback path. Asynchronous
// submission, the render graph, and pipeline caching are later nodes that
// EXTEND this interface; they do not reshape it.
//
// Contract highlights:
//   * All six resource kinds are generational handles (handle.h); every
//     entry point validates handles first and returns structured Errors
//     ("rhi.null_handle" / "rhi.stale_handle") — never UB.
//   * Command lists follow the shared CommandListState machine
//     (command_state.h): begin -> record -> end -> submit_and_wait; the
//     transition errors are identical on every backend by construction.
//   * submit_and_wait() returns only when the GPU work is COMPLETE; a
//     subsequent read_texture() sees the rendered pixels. Headless
//     render -> PNG bytes is a launch requirement, not a mode (spec 15).
//   * Destruction order is the caller's concern only across device death:
//     destroying a device releases every still-live resource (deterministic
//     ascending slot order). Destroying a shader AFTER create_pipeline
//     consumed it is legal (pipelines snapshot their stages).

#pragma once

#include "core/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace midday::rhi {

class RhiDevice {
public:
    RhiDevice() = default;
    RhiDevice(const RhiDevice&) = delete;
    RhiDevice& operator=(const RhiDevice&) = delete;
    RhiDevice(RhiDevice&&) = delete;
    RhiDevice& operator=(RhiDevice&&) = delete;
    virtual ~RhiDevice() = default;

    [[nodiscard]] virtual const DeviceCaps& caps() const = 0;

    // -- Resources ----------------------------------------------------------
    // `initial_data` is copied before return (empty span = uninitialized
    // buffer / zero-filled texture). Texture data is tightly packed RGBA8
    // rows, top-to-bottom (the readback layout, types.h contract).

    [[nodiscard]] virtual BufferResult create_buffer(const BufferDesc& desc,
                                                     std::span<const std::byte> initial_data) = 0;
    [[nodiscard]] virtual TextureResult create_texture(const TextureDesc& desc,
                                                       std::span<const std::byte> initial_data) = 0;
    [[nodiscard]] virtual SamplerResult create_sampler(const SamplerDesc& desc) = 0;

    // Compiles GLSL -> SPIR-V at creation (core/rhi/shadercomp); a front-end
    // refusal is "rhi.shader_compile" with the compiler log in details.
    [[nodiscard]] virtual ShaderResult create_shader(const ShaderDesc& desc) = 0;
    [[nodiscard]] virtual PipelineResult create_pipeline(const PipelineDesc& desc) = 0;

    virtual std::optional<base::Error> destroy_buffer(BufferHandle handle) = 0;
    virtual std::optional<base::Error> destroy_texture(TextureHandle handle) = 0;
    virtual std::optional<base::Error> destroy_sampler(SamplerHandle handle) = 0;
    virtual std::optional<base::Error> destroy_shader(ShaderHandle handle) = 0;
    virtual std::optional<base::Error> destroy_pipeline(PipelineHandle handle) = 0;

    // -- Command recording (CommandListState transitions) --------------------

    [[nodiscard]] virtual CommandListResult create_command_list() = 0;
    virtual std::optional<base::Error> destroy_command_list(CommandListHandle handle) = 0;

    virtual std::optional<base::Error> cmd_begin(CommandListHandle list) = 0;
    virtual std::optional<base::Error> cmd_begin_render_pass(CommandListHandle list,
                                                             const RenderPassDesc& pass) = 0;
    virtual std::optional<base::Error> cmd_bind_pipeline(CommandListHandle list,
                                                         PipelineHandle pipeline) = 0;
    virtual std::optional<base::Error> cmd_bind_vertex_buffer(CommandListHandle list,
                                                              BufferHandle buffer) = 0;
    // M0 binding model: slot 0, combined image sampler, fragment stage.
    virtual std::optional<base::Error> cmd_bind_texture(CommandListHandle list,
                                                        std::uint32_t slot,
                                                        TextureHandle texture,
                                                        SamplerHandle sampler) = 0;
    virtual std::optional<base::Error>
    cmd_draw(CommandListHandle list, std::uint32_t vertex_count, std::uint32_t first_vertex) = 0;
    virtual std::optional<base::Error> cmd_end_render_pass(CommandListHandle list) = 0;
    virtual std::optional<base::Error> cmd_end(CommandListHandle list) = 0;

    // -- Synchronous execution ----------------------------------------------

    // Submits a READY list and blocks until completion; the list returns to
    // its initial state (reusable via cmd_begin).
    virtual std::optional<base::Error> submit_and_wait(CommandListHandle list) = 0;

    // Reads the texture's full contents into `out` (tightly packed RGBA8,
    // top-to-bottom; out.size() must equal width * height * 4 —
    // "rhi.size_mismatch" otherwise). Render targets are readable after the
    // submit_and_wait that drew into them.
    virtual std::optional<base::Error> read_texture(TextureHandle texture,
                                                    std::span<std::byte> out) = 0;
};

} // namespace midday::rhi

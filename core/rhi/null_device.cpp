// core/rhi/null_device.cpp — see null_device.h for the execution model.

#include "core/rhi/null_device.h"

#include "core/base/hex.h"
#include "core/rhi/validate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace midday::rhi {

base::Error null_handle_error(std::string_view kind) {
    return base::Error{.code = "rhi.null_handle",
                       .message = "null " + std::string(kind) + " handle"};
}

base::Error stale_handle_error(std::string_view kind, std::uint64_t bits) {
    base::Error error{.code = "rhi.stale_handle",
                      .message = "stale " + std::string(kind) + " handle (resource was destroyed)"};
    error.details.set("handle", base::hex64(bits));
    return error;
}

std::uint8_t unorm8_from_float(float value) {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

NullDevice::NullDevice() {
    caps_.backend = "null";
    caps_.device_name = "midday NullDevice";
    caps_.driver_info = "seam semantics double (no rasterization)";
    caps_.api_version = "0.0.0";
    caps_.max_texture_size = 16384;
    caps_.software_rasterizer = true;
}

const DeviceCaps& NullDevice::caps() const {
    return caps_;
}

BufferResult NullDevice::create_buffer(const BufferDesc& desc,
                                       std::span<const std::byte> initial_data) {
    if (auto error = validate_buffer_desc(desc))
        return {.error = std::move(error)};
    if (auto error = validate_initial_data(
            initial_data.size(), static_cast<std::size_t>(desc.size_bytes), desc.debug_name))
        return {.error = std::move(error)};
    NullBuffer buffer{.desc = desc, .data = {}};
    buffer.data.assign(initial_data.begin(), initial_data.end());
    buffer.data.resize(static_cast<std::size_t>(desc.size_bytes));
    return {.handle = buffers_.create(std::move(buffer))};
}

TextureResult NullDevice::create_texture(const TextureDesc& desc,
                                         std::span<const std::byte> initial_data) {
    if (auto error = validate_texture_desc(desc, caps_.max_texture_size))
        return {.error = std::move(error)};
    const std::size_t byte_size =
        std::size_t{desc.width} * desc.height * bytes_per_pixel(desc.format);
    if (auto error = validate_initial_data(initial_data.size(), byte_size, desc.debug_name))
        return {.error = std::move(error)};
    NullTexture texture{.desc = desc, .pixels = {}};
    texture.pixels.assign(initial_data.begin(), initial_data.end());
    texture.pixels.resize(byte_size); // zero-filled when no initial data
    return {.handle = textures_.create(std::move(texture))};
}

SamplerResult NullDevice::create_sampler(const SamplerDesc& desc) {
    return {.handle = samplers_.create(NullSampler{.desc = desc})};
}

ShaderResult NullDevice::create_shader(const ShaderDesc& desc) {
    if (desc.glsl.empty())
        return {.error =
                    base::Error{.code = "rhi.invalid_argument",
                                .message = "shader source is empty ('" + desc.debug_name + "')"}};
    return {.handle = shaders_.create(NullShader{.desc = desc})};
}

PipelineResult NullDevice::create_pipeline(const PipelineDesc& desc) {
    std::optional<base::Error> error;
    if (lookup_handle(shaders_, desc.vertex_shader, "vertex shader", error) == nullptr)
        return {.error = std::move(error)};
    if (lookup_handle(shaders_, desc.fragment_shader, "fragment shader", error) == nullptr)
        return {.error = std::move(error)};
    return {.handle = pipelines_.create(NullPipeline{.desc = desc})};
}

std::optional<base::Error> NullDevice::destroy_buffer(BufferHandle handle) {
    return release_handle(buffers_, handle, "buffer");
}

std::optional<base::Error> NullDevice::destroy_texture(TextureHandle handle) {
    return release_handle(textures_, handle, "texture");
}

std::optional<base::Error> NullDevice::destroy_sampler(SamplerHandle handle) {
    return release_handle(samplers_, handle, "sampler");
}

std::optional<base::Error> NullDevice::destroy_shader(ShaderHandle handle) {
    return release_handle(shaders_, handle, "shader");
}

std::optional<base::Error> NullDevice::destroy_pipeline(PipelineHandle handle) {
    return release_handle(pipelines_, handle, "pipeline");
}

CommandListResult NullDevice::create_command_list() {
    return {.handle = lists_.create(NullCommandList{})};
}

std::optional<base::Error> NullDevice::destroy_command_list(CommandListHandle handle) {
    return release_handle(lists_, handle, "command list");
}

NullDevice::NullCommandList* NullDevice::live_list(CommandListHandle handle,
                                                   std::optional<base::Error>& error) {
    return lookup_handle(lists_, handle, "command list", error);
}

std::optional<base::Error> NullDevice::cmd_begin(CommandListHandle list) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    if (auto state_error = cmd->state.begin())
        return state_error;
    cmd->pending_draws = 0;
    return std::nullopt;
}

std::optional<base::Error> NullDevice::cmd_begin_render_pass(CommandListHandle list,
                                                             const RenderPassDesc& pass) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    NullTexture* target = lookup_handle(textures_, pass.color_target, "render target", error);
    if (target == nullptr)
        return error;
    if (auto usage_error = validate_render_target_usage(target->desc))
        return usage_error;
    if (auto state_error = cmd->state.begin_render_pass())
        return state_error;
    // The null execution model applies the clear IMMEDIATELY (recording and
    // executing coincide when nothing rasterizes); the state machine still
    // demands the full record -> submit protocol before readback is
    // meaningful, because tests assert the protocol, not the shortcut.
    const std::uint8_t rgba[4] = {unorm8_from_float(pass.clear.r),
                                  unorm8_from_float(pass.clear.g),
                                  unorm8_from_float(pass.clear.b),
                                  unorm8_from_float(pass.clear.a)};
    for (std::size_t i = 0; i < target->pixels.size(); i += 4)
        std::memcpy(&target->pixels[i], rgba, 4);
    return std::nullopt;
}

std::optional<base::Error> NullDevice::cmd_bind_pipeline(CommandListHandle list,
                                                         PipelineHandle pipeline) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    NullPipeline* pipe = lookup_handle(pipelines_, pipeline, "pipeline", error);
    if (pipe == nullptr)
        return error;
    return cmd->state.bind_pipeline(pipe->desc.uses_texture);
}

std::optional<base::Error> NullDevice::cmd_bind_vertex_buffer(CommandListHandle list,
                                                              BufferHandle buffer) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    if (lookup_handle(buffers_, buffer, "vertex buffer", error) == nullptr)
        return error;
    return cmd->state.bind_vertex_buffer();
}

std::optional<base::Error> NullDevice::cmd_bind_texture(CommandListHandle list,
                                                        std::uint32_t slot,
                                                        TextureHandle texture,
                                                        SamplerHandle sampler) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    if (auto slot_error = validate_texture_slot(slot))
        return slot_error;
    if (lookup_handle(textures_, texture, "texture", error) == nullptr)
        return error;
    if (lookup_handle(samplers_, sampler, "sampler", error) == nullptr)
        return error;
    return cmd->state.bind_texture();
}

std::optional<base::Error> NullDevice::cmd_draw(CommandListHandle list,
                                                std::uint32_t vertex_count,
                                                std::uint32_t first_vertex) {
    (void)first_vertex;
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    if (auto count_error = validate_draw_count(vertex_count))
        return count_error;
    if (auto state_error = cmd->state.draw())
        return state_error;
    ++cmd->pending_draws; // recorded; counted into submitted_draws_ at submit
    return std::nullopt;
}

std::optional<base::Error> NullDevice::cmd_end_render_pass(CommandListHandle list) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    return cmd->state.end_render_pass();
}

std::optional<base::Error> NullDevice::cmd_end(CommandListHandle list) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    return cmd->state.end();
}

std::optional<base::Error> NullDevice::submit_and_wait(CommandListHandle list) {
    std::optional<base::Error> error;
    NullCommandList* cmd = live_list(list, error);
    if (cmd == nullptr)
        return error;
    if (auto state_error = cmd->state.submit())
        return state_error;
    submitted_draws_ += cmd->pending_draws;
    cmd->pending_draws = 0;
    return std::nullopt;
}

std::optional<base::Error> NullDevice::read_texture(TextureHandle texture,
                                                    std::span<std::byte> out) {
    std::optional<base::Error> error;
    NullTexture* tex = lookup_handle(textures_, texture, "texture", error);
    if (tex == nullptr)
        return error;
    if (auto size_error = validate_readback_size(out.size(), tex->pixels.size()))
        return size_error;
    std::memcpy(out.data(), tex->pixels.data(), tex->pixels.size());
    return std::nullopt;
}

std::string_view to_string(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::kVertex:
        return "vertex";
    case ShaderStage::kFragment:
        return "fragment";
    }
    return "unknown";
}

} // namespace midday::rhi

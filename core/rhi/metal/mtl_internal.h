// core/rhi/metal/mtl_internal.h — INTERNAL backend header: the only place
// (with metal/*.mm) where Metal types exist. Objective-C++, ARC-compiled
// TUs only; never include this outside core/rhi/metal/ (boundaries lane
// enforces the import pattern).

#pragma once

#include "core/rhi/command_state.h"
#include "core/rhi/device.h"
#include "core/rhi/handle_lookup.h"
#include "core/rhi/metal/metal_device.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#import <Metal/Metal.h>

namespace midday::rhi::mtl {

// Structured backend fault: "rhi.device_fault" with the NSError description
// (when present) in details. Used for every unexpected Metal refusal.
[[nodiscard]] base::Error mtl_fault(std::string_view what, NSError* error);

// Structured mapping of an escaped Objective-C exception: "rhi.device_fault"
// with the NSException name + reason in details. Metal's validation layer
// and framework internals report misuse by RAISING NSException; the seam
// contract says errors are VALUES (device.h), so every Metal-touching entry
// point body runs inside guarded()/guarded_call() and no ObjC throw can
// cross the seam — on any host, virtual GPUs included.
[[nodiscard]] base::Error nsexception_fault(const char* what, NSException* exception);

template <typename Result, typename Fn> Result guarded(const char* what, Fn&& fn) {
    @try {
        return fn();
    } @catch (NSException* exception) {
        Result result;
        result.error = nsexception_fault(what, exception);
        return result;
    }
}

template <typename Fn> std::optional<base::Error> guarded_call(const char* what, Fn&& fn) {
    @try {
        return fn();
    } @catch (NSException* exception) {
        return nsexception_fault(what, exception);
    }
}

// -- Resource table entries ---------------------------------------------------
// ObjC object references are ARC-managed __strong members: HandlePool slot
// recycling releases the underlying Metal objects deterministically.

struct BufferEntry {
    BufferDesc desc;
    id<MTLBuffer> buffer = nil;
};

struct TextureEntry {
    TextureDesc desc;
    id<MTLTexture> texture = nil;
    bool contents_defined = false; // mirrors the Vulkan layout!=UNDEFINED gate
};

struct SamplerEntry {
    SamplerDesc desc;
    id<MTLSamplerState> sampler = nil;
};

struct ShaderEntry {
    ShaderStage stage = ShaderStage::kVertex;
    id<MTLFunction> function = nil; // holds its MTLLibrary alive
};

struct PipelineEntry {
    id<MTLRenderPipelineState> pipeline = nil;
    bool uses_texture = false;
};

struct CommandListEntry {
    CommandListState state;
    id<MTLCommandBuffer> commands = nil;       // fresh at every cmd_begin
    id<MTLRenderCommandEncoder> encoder = nil; // live between pass begin/end
};

// -- The device ---------------------------------------------------------------

class MetalDevice final : public RhiDevice {
public:
    MetalDevice() = default;
    ~MetalDevice() override;
    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;
    MetalDevice(MetalDevice&&) = delete;
    MetalDevice& operator=(MetalDevice&&) = delete;

    // Full bring-up; on failure only the Error travels (factory in
    // mtl_device.mm) and ARC unwinds whatever was created.
    [[nodiscard]] std::optional<base::Error> initialize(const MetalDeviceOptions& options);

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

private:
    CommandListEntry* live_list(CommandListHandle handle, std::optional<base::Error>& error);

    // Records fn into a transient blit encoder, commits, waits (uploads,
    // zero-fills, readbacks) — the with_transient_commands shape.
    std::optional<base::Error>
    with_transient_blit(const std::function<void(id<MTLBlitCommandEncoder>)>& fn);

    // Staged texture write shared by upload and zero-fill: bytes -> shared
    // staging buffer -> blit into the private texture.
    std::optional<base::Error> upload_texture(TextureEntry& entry, std::span<const std::byte> data);

    // -- Immutable after initialize ------------------------------------------
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    DeviceCaps caps_;

    // -- Resource tables -------------------------------------------------------
    HandlePool<BufferHandleTag, BufferEntry> buffers_;
    HandlePool<TextureHandleTag, TextureEntry> textures_;
    HandlePool<SamplerHandleTag, SamplerEntry> samplers_;
    HandlePool<ShaderHandleTag, ShaderEntry> shaders_;
    HandlePool<PipelineHandleTag, PipelineEntry> pipelines_;
    HandlePool<CommandListHandleTag, CommandListEntry> lists_;
};

} // namespace midday::rhi::mtl

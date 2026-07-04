// core/rhi/metal/mtl_commands.mm — command recording, synchronous
// submission (m0-rhi-metal). Every transition consults the SHARED
// CommandListState first (command_state.h), so protocol errors are
// byte-identical to NullDevice's and Vulkan's; only then does Metal encode.
// Metal's natural shape maps 1:1: cmd_begin = fresh MTLCommandBuffer,
// begin/end_render_pass = one MTLRenderCommandEncoder, submit_and_wait =
// commit + waitUntilCompleted.

#include "core/rhi/metal/mtl_internal.h"
#include "core/rhi/validate.h"

namespace midday::rhi::mtl {

CommandListResult MetalDevice::create_command_list() {
    // The MTLCommandBuffer is single-use by API design; it is minted at
    // every cmd_begin (which is also what makes begin-on-ready a RESET).
    return {.handle = lists_.create(CommandListEntry{})};
}

std::optional<base::Error> MetalDevice::destroy_command_list(CommandListHandle handle) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(handle, error);
    if (entry == nullptr)
        return error;
    if (entry->encoder != nil) // destroyed mid-pass: close before ARC frees
        [entry->encoder endEncoding];
    (void)lists_.release(handle);
    return std::nullopt;
}

CommandListEntry* MetalDevice::live_list(CommandListHandle handle,
                                         std::optional<base::Error>& error) {
    return lookup_handle(lists_, handle, "command list", error);
}

std::optional<base::Error> MetalDevice::cmd_begin(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto state_error = entry->state.begin())
        return state_error;
    entry->commands = [queue_ commandBuffer]; // any un-submitted predecessor drops here
    if (entry->commands == nil)
        return mtl_fault("MTLCommandQueue commandBuffer", nil);
    return std::nullopt;
}

std::optional<base::Error> MetalDevice::cmd_begin_render_pass(CommandListHandle list,
                                                              const RenderPassDesc& pass) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    TextureEntry* target = lookup_handle(textures_, pass.color_target, "render target", error);
    if (target == nullptr)
        return error;
    if (auto usage_error = validate_render_target_usage(target->desc))
        return usage_error;
    if (auto state_error = entry->state.begin_render_pass())
        return state_error;

    MTLRenderPassDescriptor* pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    pass_desc.colorAttachments[0].texture = target->texture;
    pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass_desc.colorAttachments[0].clearColor =
        MTLClearColorMake(pass.clear.r, pass.clear.g, pass.clear.b, pass.clear.a);
    entry->encoder = [entry->commands renderCommandEncoderWithDescriptor:pass_desc];
    if (entry->encoder == nil)
        return mtl_fault("MTLCommandBuffer renderCommandEncoderWithDescriptor", nil);

    // Full-target viewport, depth [0,1] — NO y flip here: the coordinate
    // adaptation is the vertex-stage clip negation (shadercomp), pinned.
    [entry->encoder setViewport:MTLViewport{0.0,
                                            0.0,
                                            static_cast<double>(target->desc.width),
                                            static_cast<double>(target->desc.height),
                                            0.0,
                                            1.0}];
    target->contents_defined = true; // render pass finalizes readable content
    return std::nullopt;
}

std::optional<base::Error> MetalDevice::cmd_bind_pipeline(CommandListHandle list,
                                                          PipelineHandle pipeline) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    PipelineEntry* pipe = lookup_handle(pipelines_, pipeline, "pipeline", error);
    if (pipe == nullptr)
        return error;
    if (auto state_error = entry->state.bind_pipeline(pipe->uses_texture))
        return state_error;
    [entry->encoder setRenderPipelineState:pipe->pipeline];
    [entry->encoder setCullMode:MTLCullModeNone]; // pinned M0 state (types.h)
    return std::nullopt;
}

std::optional<base::Error> MetalDevice::cmd_bind_vertex_buffer(CommandListHandle list,
                                                               BufferHandle buffer) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    BufferEntry* buf = lookup_handle(buffers_, buffer, "vertex buffer", error);
    if (buf == nullptr)
        return error;
    if (auto state_error = entry->state.bind_vertex_buffer())
        return state_error;
    [entry->encoder setVertexBuffer:buf->buffer offset:0 atIndex:0];
    return std::nullopt;
}

std::optional<base::Error> MetalDevice::cmd_bind_texture(CommandListHandle list,
                                                         std::uint32_t slot,
                                                         TextureHandle texture,
                                                         SamplerHandle sampler) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto slot_error = validate_texture_slot(slot))
        return slot_error;
    TextureEntry* tex = lookup_handle(textures_, texture, "texture", error);
    if (tex == nullptr)
        return error;
    SamplerEntry* smp = lookup_handle(samplers_, sampler, "sampler", error);
    if (smp == nullptr)
        return error;
    if (auto state_error = entry->state.bind_texture())
        return state_error;
    // (set 0, binding 0) -> [[texture(0)]] + [[sampler(0)]], the SPIRV-Cross
    // mapping pinned by rhi.shadercomp.msl_combined_sampler_maps_to_index_zero.
    [entry->encoder setFragmentTexture:tex->texture atIndex:0];
    [entry->encoder setFragmentSamplerState:smp->sampler atIndex:0];
    return std::nullopt;
}

std::optional<base::Error> MetalDevice::cmd_draw(CommandListHandle list,
                                                 std::uint32_t vertex_count,
                                                 std::uint32_t first_vertex) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto count_error = validate_draw_count(vertex_count))
        return count_error;
    if (auto state_error = entry->state.draw())
        return state_error;
    [entry->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                       vertexStart:first_vertex
                       vertexCount:vertex_count];
    return std::nullopt;
}

std::optional<base::Error> MetalDevice::cmd_end_render_pass(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto state_error = entry->state.end_render_pass())
        return state_error;
    [entry->encoder endEncoding];
    entry->encoder = nil;
    return std::nullopt;
}

std::optional<base::Error> MetalDevice::cmd_end(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    // Metal needs no explicit end-of-encoding on the command buffer itself;
    // the shared state machine is the whole transition.
    return entry->state.end();
}

std::optional<base::Error> MetalDevice::submit_and_wait(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto state_error = entry->state.submit())
        return state_error;
    id<MTLCommandBuffer> commands = entry->commands;
    entry->commands = nil; // single-use: the list is reusable via cmd_begin
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted)
        return mtl_fault("MTLCommandBuffer commit/waitUntilCompleted", commands.error);
    return std::nullopt;
}

} // namespace midday::rhi::mtl

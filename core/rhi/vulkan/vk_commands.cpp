// core/rhi/vulkan/vk_commands.cpp — command recording, synchronous
// submission, readback (m0-rhi-vulkan). Every transition consults the
// SHARED CommandListState first (command_state.h), so protocol errors are
// byte-identical to NullDevice's; only then does Vulkan record.

#include "core/rhi/null_device.h" // shared error builders
#include "core/rhi/validate.h"
#include "core/rhi/vulkan/vk_internal.h"

#include <cstring>
#include <utility>
#include <vector>

namespace midday::rhi::vk {

CommandListResult VulkanDevice::create_command_list() {
    CommandListEntry entry;
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    if (VkResult result = vkAllocateCommandBuffers(device_, &alloc_info, &entry.commands);
        result != VK_SUCCESS)
        return {.error = vk_fault("vkAllocateCommandBuffers", result)};

    // Per-list descriptor pool: reset at every cmd_begin, so transient sets
    // never dangle across lists (16 textured draws per recording is beyond
    // anything M0 records; growth is a straightforward later bump).
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 16;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 16;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &entry.descriptors);
        result != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &entry.commands);
        return {.error = vk_fault("vkCreateDescriptorPool", result)};
    }
    return {.handle = lists_.create(entry)};
}

std::optional<base::Error> VulkanDevice::destroy_command_list(CommandListHandle handle) {
    std::optional<base::Error> error;
    if (live_list(handle, error) == nullptr)
        return error;
    vkDeviceWaitIdle(device_);
    CommandListEntry entry = lists_.release(handle);
    vkDestroyDescriptorPool(device_, entry.descriptors, nullptr);
    vkFreeCommandBuffers(device_, command_pool_, 1, &entry.commands);
    return std::nullopt;
}

CommandListEntry* VulkanDevice::live_list(CommandListHandle handle,
                                          std::optional<base::Error>& error) {
    return lookup(lists_, handle, "command list", error);
}

std::optional<base::Error> VulkanDevice::cmd_begin(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto state_error = entry->state.begin())
        return state_error;
    vkResetCommandBuffer(entry->commands, 0);
    vkResetDescriptorPool(device_, entry->descriptors, 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (VkResult result = vkBeginCommandBuffer(entry->commands, &begin_info); result != VK_SUCCESS)
        return vk_fault("vkBeginCommandBuffer", result);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::cmd_begin_render_pass(CommandListHandle list,
                                                               const RenderPassDesc& pass) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    TextureEntry* target = lookup(textures_, pass.color_target, "render target", error);
    if (target == nullptr)
        return error;
    if (auto usage_error = validate_render_target_usage(target->desc))
        return usage_error;
    // Lazy framebuffer, cached on the texture for its lifetime.
    if (target->framebuffer == VK_NULL_HANDLE) {
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = render_pass_;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &target->view;
        framebuffer_info.width = target->desc.width;
        framebuffer_info.height = target->desc.height;
        framebuffer_info.layers = 1;
        if (VkResult result =
                vkCreateFramebuffer(device_, &framebuffer_info, nullptr, &target->framebuffer);
            result != VK_SUCCESS)
            return vk_fault("vkCreateFramebuffer", result);
    }
    if (auto state_error = entry->state.begin_render_pass())
        return state_error;

    VkClearValue clear{};
    clear.color = {{pass.clear.r, pass.clear.g, pass.clear.b, pass.clear.a}};
    VkRenderPassBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = render_pass_;
    begin_info.framebuffer = target->framebuffer;
    begin_info.renderArea = {{0, 0}, {target->desc.width, target->desc.height}};
    begin_info.clearValueCount = 1;
    begin_info.pClearValues = &clear;
    vkCmdBeginRenderPass(entry->commands, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

    // Full-target viewport/scissor (pipelines declare them dynamic).
    VkViewport viewport{};
    viewport.width = static_cast<float>(target->desc.width);
    viewport.height = static_cast<float>(target->desc.height);
    viewport.maxDepth = 1.0F;
    vkCmdSetViewport(entry->commands, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {target->desc.width, target->desc.height}};
    vkCmdSetScissor(entry->commands, 0, 1, &scissor);

    target->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // render pass finalLayout
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::cmd_bind_pipeline(CommandListHandle list,
                                                           PipelineHandle pipeline) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    PipelineEntry* pipe = lookup(pipelines_, pipeline, "pipeline", error);
    if (pipe == nullptr)
        return error;
    if (auto state_error = entry->state.bind_pipeline(pipe->uses_texture))
        return state_error;
    vkCmdBindPipeline(entry->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::cmd_bind_vertex_buffer(CommandListHandle list,
                                                                BufferHandle buffer) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    BufferEntry* buf = lookup(buffers_, buffer, "vertex buffer", error);
    if (buf == nullptr)
        return error;
    if (auto state_error = entry->state.bind_vertex_buffer())
        return state_error;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(entry->commands, 0, 1, &buf->buffer, &offset);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::cmd_bind_texture(CommandListHandle list,
                                                          std::uint32_t slot,
                                                          TextureHandle texture,
                                                          SamplerHandle sampler) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto slot_error = validate_texture_slot(slot))
        return slot_error;
    TextureEntry* tex = lookup(textures_, texture, "texture", error);
    if (tex == nullptr)
        return error;
    SamplerEntry* smp = lookup(samplers_, sampler, "sampler", error);
    if (smp == nullptr)
        return error;
    if (auto state_error = entry->state.bind_texture())
        return state_error;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = entry->descriptors;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &texture_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (VkResult result = vkAllocateDescriptorSets(device_, &alloc_info, &set);
        result != VK_SUCCESS)
        return vk_fault("vkAllocateDescriptorSets", result);

    VkDescriptorImageInfo image_info{};
    image_info.sampler = smp->sampler;
    image_info.imageView = tex->view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    vkCmdBindDescriptorSets(
        entry->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, textured_layout_, 0, 1, &set, 0, nullptr);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::cmd_draw(CommandListHandle list,
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
    vkCmdDraw(entry->commands, vertex_count, 1, first_vertex, 0);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::cmd_end_render_pass(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto state_error = entry->state.end_render_pass())
        return state_error;
    vkCmdEndRenderPass(entry->commands);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::cmd_end(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto state_error = entry->state.end())
        return state_error;
    if (VkResult result = vkEndCommandBuffer(entry->commands); result != VK_SUCCESS)
        return vk_fault("vkEndCommandBuffer", result);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::submit_and_wait(CommandListHandle list) {
    std::optional<base::Error> error;
    CommandListEntry* entry = live_list(list, error);
    if (entry == nullptr)
        return error;
    if (auto state_error = entry->state.submit())
        return state_error;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &entry->commands;
    if (VkResult result = vkQueueSubmit(queue_, 1, &submit, fence_); result != VK_SUCCESS)
        return vk_fault("vkQueueSubmit", result);
    if (VkResult result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        result != VK_SUCCESS)
        return vk_fault("vkWaitForFences", result);
    vkResetFences(device_, 1, &fence_);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::read_texture(TextureHandle texture,
                                                      std::span<std::byte> out) {
    std::optional<base::Error> error;
    TextureEntry* tex = lookup(textures_, texture, "texture", error);
    if (tex == nullptr)
        return error;
    const std::size_t expected =
        std::size_t{tex->desc.width} * tex->desc.height * bytes_per_pixel(tex->desc.format);
    if (auto size_error = validate_readback_size(out.size(), expected))
        return size_error;
    if (tex->layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return base::Error{.code = "rhi.invalid_argument",
                           .message = "texture has no defined contents yet (render or upload "
                                      "before reading '" +
                                      tex->desc.debug_name + "')"};

    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_alloc = VK_NULL_HANDLE;
    if (auto make_error = make_buffer(expected,
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                          VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      readback,
                                      readback_alloc))
        return make_error;

    const VkImageLayout resting = tex->layout;
    auto copy_error = with_transient_commands([&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = tex->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (resting != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = resting;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(commands,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr,
                                 1,
                                 &barrier);
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {tex->desc.width, tex->desc.height, 1};
        vkCmdCopyImageToBuffer(
            commands, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        if (resting != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = resting;
            vkCmdPipelineBarrier(commands,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr,
                                 1,
                                 &barrier);
        }
    });
    if (!copy_error) {
        if (VkResult result =
                vmaCopyAllocationToMemory(allocator_, readback_alloc, 0, out.data(), expected);
            result != VK_SUCCESS)
            copy_error = vk_fault("vmaCopyAllocationToMemory", result);
    }
    vmaDestroyBuffer(allocator_, readback, readback_alloc);
    return copy_error;
}

} // namespace midday::rhi::vk

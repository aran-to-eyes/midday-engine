// core/rhi/vulkan/vk_textures.cpp — texture creation, upload, zero-fill,
// release (m0-rhi-vulkan). Split from vk_resources.cpp: images own the
// layout-transition choreography (staging upload, defined-zero content,
// framebuffer cache) and deserve their own translation unit.

#include "core/rhi/validate.h"
#include "core/rhi/vulkan/vk_internal.h"

#include <cstddef>
#include <utility>

namespace midday::rhi::vk {

void VulkanDevice::release_texture(TextureEntry& entry) {
    if (entry.framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device_, entry.framebuffer, nullptr);
    if (entry.view != VK_NULL_HANDLE)
        vkDestroyImageView(device_, entry.view, nullptr);
    if (entry.image != VK_NULL_HANDLE)
        vmaDestroyImage(allocator_, entry.image, entry.allocation);
    entry.framebuffer = VK_NULL_HANDLE;
    entry.view = VK_NULL_HANDLE;
    entry.image = VK_NULL_HANDLE;
    entry.allocation = VK_NULL_HANDLE;
}

TextureResult VulkanDevice::create_texture(const TextureDesc& desc,
                                           std::span<const std::byte> initial_data) {
    if (auto error = validate_texture_desc(desc, caps_.max_texture_size))
        return {.error = std::move(error)};
    const std::size_t byte_size =
        std::size_t{desc.width} * desc.height * bytes_per_pixel(desc.format);
    if (auto error = validate_initial_data(initial_data.size(), byte_size, desc.debug_name))
        return {.error = std::move(error)};

    const bool render_target = desc.usage == TextureUsage::kRenderTarget;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {desc.width, desc.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = render_target
                           ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                           : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    TextureEntry entry{.desc = desc};
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    if (VkResult result = vmaCreateImage(
            allocator_, &image_info, &alloc_info, &entry.image, &entry.allocation, nullptr);
        result != VK_SUCCESS)
        return {.error = vk_fault("vmaCreateImage", result)};

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (VkResult result = vkCreateImageView(device_, &view_info, nullptr, &entry.view);
        result != VK_SUCCESS) {
        release_texture(entry);
        return {.error = vk_fault("vkCreateImageView", result)};
    }

    // Content policy mirrors NullDevice: initial data uploads; absence means
    // DEFINED zero content (sampled textures are cleared, render targets
    // stay UNDEFINED — their first render pass clears them).
    std::optional<base::Error> content_error;
    if (!initial_data.empty())
        content_error = upload_texture(entry, initial_data);
    else if (!render_target)
        content_error = zero_fill_texture(entry);
    if (content_error) {
        release_texture(entry);
        return {.error = std::move(content_error)};
    }
    return {.handle = textures_.create(std::move(entry))};
}

std::optional<base::Error> VulkanDevice::upload_texture(TextureEntry& entry,
                                                        std::span<const std::byte> data) {
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE;
    if (auto error = make_buffer(data.size(),
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                 staging,
                                 staging_alloc))
        return error;
    if (VkResult result =
            vmaCopyMemoryToAllocation(allocator_, data.data(), staging_alloc, 0, data.size());
        result != VK_SUCCESS) {
        vmaDestroyBuffer(allocator_, staging, staging_alloc);
        return vk_fault("vmaCopyMemoryToAllocation(staging)", result);
    }

    auto error = with_transient_commands([&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = entry.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {entry.desc.width, entry.desc.height, 1};
        vkCmdCopyBufferToImage(
            commands, staging, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commands,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
    });
    vmaDestroyBuffer(allocator_, staging, staging_alloc);
    if (!error)
        entry.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return error;
}

std::optional<base::Error> VulkanDevice::zero_fill_texture(TextureEntry& entry) {
    auto error = with_transient_commands([&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = entry.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(
            commands, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commands,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
    });
    if (!error)
        entry.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return error;
}

} // namespace midday::rhi::vk

// core/rhi/vulkan/vk_internal.h — INTERNAL backend header: the only place
// (with vulkan/*.cpp) where Vulkan types exist. Never include this outside
// core/rhi/vulkan/ (boundaries lane enforces the include pattern).

#pragma once

#include "core/rhi/command_state.h"
#include "core/rhi/device.h"
#include "core/rhi/vulkan/vulkan_device.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vk_mem_alloc.h>
#include <volk.h>

namespace midday::rhi::vk {

// Structured backend fault: "rhi.device_fault" with the VkResult spelled in
// details. Used for every unexpected Vulkan return code.
[[nodiscard]] base::Error vk_fault(std::string_view what, VkResult result);

[[nodiscard]] const char* vk_result_name(VkResult result);

// -- Resource table entries ---------------------------------------------------

struct BufferEntry {
    BufferDesc desc;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct TextureEntry {
    TextureDesc desc;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE; // render targets, lazily created
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct SamplerEntry {
    SamplerDesc desc;
    VkSampler sampler = VK_NULL_HANDLE;
};

struct ShaderEntry {
    ShaderStage stage = ShaderStage::kVertex;
    std::string entry_point;
    VkShaderModule module = VK_NULL_HANDLE;
};

struct PipelineEntry {
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool uses_texture = false; // selects the shared pipeline layout
};

struct CommandListEntry {
    CommandListState state;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE; // per-list, reset at begin
};

// -- The device ---------------------------------------------------------------

class VulkanDevice final : public RhiDevice {
public:
    VulkanDevice() = default;
    ~VulkanDevice() override;
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    VulkanDevice(VulkanDevice&&) = delete;
    VulkanDevice& operator=(VulkanDevice&&) = delete;

    // Full bring-up; on failure the partially built device is torn down by
    // the destructor and only the Error travels (factory in vk_device.cpp).
    [[nodiscard]] std::optional<base::Error> initialize(const VulkanDeviceOptions& options);

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
    // Handle lookup returning nullptr + structured error (shared spellings
    // from core/rhi/null_device.h's error builders).
    template <typename Tag, typename T>
    T* lookup(HandlePool<Tag, T>& pool,
              Handle<Tag> handle,
              std::string_view kind,
              std::optional<base::Error>& error);

    CommandListEntry* live_list(CommandListHandle handle, std::optional<base::Error>& error);

    // Records fn into a transient command buffer, submits, waits (uploads,
    // clears, readbacks). fn must record unconditionally.
    std::optional<base::Error>
    with_transient_commands(const std::function<void(VkCommandBuffer)>& fn);

    // Buffer creation shared by vertex buffers, upload staging, readback.
    std::optional<base::Error> make_buffer(VkDeviceSize size,
                                           VkBufferUsageFlags usage,
                                           VmaAllocationCreateFlags alloc_flags,
                                           VkBuffer& out_buffer,
                                           VmaAllocation& out_allocation);

    void release_buffer(BufferEntry& entry);
    void release_texture(TextureEntry& entry);
    std::optional<base::Error> upload_texture(TextureEntry& entry, std::span<const std::byte> data);
    std::optional<base::Error> zero_fill_texture(TextureEntry& entry);

    // -- Immutable after initialize ------------------------------------------
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;   // long-lived list buffers
    VkCommandPool transient_pool_ = VK_NULL_HANDLE; // uploads/readbacks
    VkFence fence_ = VK_NULL_HANDLE;                // the synchronous-submit fence
    VkRenderPass render_pass_ = VK_NULL_HANDLE;     // RGBA8, clear->store->transfer_src
    VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout empty_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout textured_layout_ = VK_NULL_HANDLE;
    DeviceCaps caps_;

    // -- Resource tables -------------------------------------------------------
    HandlePool<BufferHandleTag, BufferEntry> buffers_;
    HandlePool<TextureHandleTag, TextureEntry> textures_;
    HandlePool<SamplerHandleTag, SamplerEntry> samplers_;
    HandlePool<ShaderHandleTag, ShaderEntry> shaders_;
    HandlePool<PipelineHandleTag, PipelineEntry> pipelines_;
    HandlePool<CommandListHandleTag, CommandListEntry> lists_;
};

} // namespace midday::rhi::vk

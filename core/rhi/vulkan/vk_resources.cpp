// core/rhi/vulkan/vk_resources.cpp — resource creation/destruction over VMA
// (m0-rhi-vulkan). Descriptor validation reuses the seam's shared error
// spellings so the Vulkan backend and NullDevice refuse identically.

#include "core/rhi/null_device.h" // shared error builders (null_handle/stale_handle)
#include "core/rhi/shadercomp/shader_compiler.h"
#include "core/rhi/validate.h"
#include "core/rhi/vulkan/vk_internal.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace midday::rhi::vk {

namespace {

VkFilter to_vk(FilterMode filter) {
    return filter == FilterMode::kNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode to_vk(AddressMode address) {
    return address == AddressMode::kClampToEdge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                                : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkFormat to_vk(VertexFormat format) {
    switch (format) {
    case VertexFormat::kFloat2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::kFloat3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::kFloat4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

} // namespace

template <typename Tag, typename T>
T* VulkanDevice::lookup(HandlePool<Tag, T>& pool,
                        Handle<Tag> handle,
                        std::string_view kind,
                        std::optional<base::Error>& error) {
    if (handle.is_null()) {
        error = null_handle_error(kind);
        return nullptr;
    }
    T* value = pool.get(handle);
    if (value == nullptr)
        error = stale_handle_error(kind, handle.to_bits());
    return value;
}

// Explicit instantiations for the TUs sharing the template (commands TU).
template BufferEntry* VulkanDevice::lookup(HandlePool<BufferHandleTag, BufferEntry>&,
                                           BufferHandle,
                                           std::string_view,
                                           std::optional<base::Error>&);
template TextureEntry* VulkanDevice::lookup(HandlePool<TextureHandleTag, TextureEntry>&,
                                            TextureHandle,
                                            std::string_view,
                                            std::optional<base::Error>&);
template SamplerEntry* VulkanDevice::lookup(HandlePool<SamplerHandleTag, SamplerEntry>&,
                                            SamplerHandle,
                                            std::string_view,
                                            std::optional<base::Error>&);
template ShaderEntry* VulkanDevice::lookup(HandlePool<ShaderHandleTag, ShaderEntry>&,
                                           ShaderHandle,
                                           std::string_view,
                                           std::optional<base::Error>&);
template PipelineEntry* VulkanDevice::lookup(HandlePool<PipelineHandleTag, PipelineEntry>&,
                                             PipelineHandle,
                                             std::string_view,
                                             std::optional<base::Error>&);
template CommandListEntry* VulkanDevice::lookup(HandlePool<CommandListHandleTag, CommandListEntry>&,
                                                CommandListHandle,
                                                std::string_view,
                                                std::optional<base::Error>&);

std::optional<base::Error> VulkanDevice::make_buffer(VkDeviceSize size,
                                                     VkBufferUsageFlags usage,
                                                     VmaAllocationCreateFlags alloc_flags,
                                                     VkBuffer& out_buffer,
                                                     VmaAllocation& out_allocation) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = alloc_flags;
    if (VkResult result = vmaCreateBuffer(
            allocator_, &buffer_info, &alloc_info, &out_buffer, &out_allocation, nullptr);
        result != VK_SUCCESS)
        return vk_fault("vmaCreateBuffer", result);
    return std::nullopt;
}

void VulkanDevice::release_buffer(BufferEntry& entry) {
    if (entry.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator_, entry.buffer, entry.allocation);
    entry.buffer = VK_NULL_HANDLE;
    entry.allocation = VK_NULL_HANDLE;
}

BufferResult VulkanDevice::create_buffer(const BufferDesc& desc,
                                         std::span<const std::byte> initial_data) {
    if (auto error = validate_buffer_desc(desc))
        return {.error = std::move(error)};
    if (auto error = validate_initial_data(
            initial_data.size(), static_cast<std::size_t>(desc.size_bytes), desc.debug_name))
        return {.error = std::move(error)};

    BufferEntry entry{.desc = desc};
    // M0 vertex data is tiny and written once: host-visible mapped memory is
    // the simplest correct choice on every device class (and IS device
    // memory on lavapipe/UMA). Dedicated device-local paths come with real
    // mesh streaming, not before.
    if (auto error = make_buffer(desc.size_bytes,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                 entry.buffer,
                                 entry.allocation))
        return {.error = std::move(error)};
    if (!initial_data.empty()) {
        if (VkResult result = vmaCopyMemoryToAllocation(
                allocator_, initial_data.data(), entry.allocation, 0, initial_data.size());
            result != VK_SUCCESS) {
            release_buffer(entry);
            return {.error = vk_fault("vmaCopyMemoryToAllocation", result)};
        }
    }
    return {.handle = buffers_.create(std::move(entry))};
}

SamplerResult VulkanDevice::create_sampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = to_vk(desc.filter);
    sampler_info.minFilter = to_vk(desc.filter);
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = to_vk(desc.address);
    sampler_info.addressModeV = to_vk(desc.address);
    sampler_info.addressModeW = to_vk(desc.address);
    SamplerEntry entry{.desc = desc};
    if (VkResult result = vkCreateSampler(device_, &sampler_info, nullptr, &entry.sampler);
        result != VK_SUCCESS)
        return {.error = vk_fault("vkCreateSampler", result)};
    return {.handle = samplers_.create(std::move(entry))};
}

ShaderResult VulkanDevice::create_shader(const ShaderDesc& desc) {
    shadercomp::SpirvResult spirv =
        shadercomp::compile_glsl(desc.stage, desc.glsl, desc.entry_point, desc.debug_name);
    if (!spirv.ok())
        return {.error = std::move(spirv.error)};
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.words.size() * sizeof(std::uint32_t);
    module_info.pCode = spirv.words.data();
    ShaderEntry entry{.stage = desc.stage,
                      .entry_point = desc.entry_point.empty() ? "main" : desc.entry_point};
    if (VkResult result = vkCreateShaderModule(device_, &module_info, nullptr, &entry.module);
        result != VK_SUCCESS)
        return {.error = vk_fault("vkCreateShaderModule", result)};
    return {.handle = shaders_.create(std::move(entry))};
}

PipelineResult VulkanDevice::create_pipeline(const PipelineDesc& desc) {
    std::optional<base::Error> error;
    ShaderEntry* vert = lookup(shaders_, desc.vertex_shader, "vertex shader", error);
    if (vert == nullptr)
        return {.error = std::move(error)};
    ShaderEntry* frag = lookup(shaders_, desc.fragment_shader, "fragment shader", error);
    if (frag == nullptr)
        return {.error = std::move(error)};

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert->module;
    stages[0].pName = vert->entry_point.c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag->module;
    stages[1].pName = frag->entry_point.c_str();

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = desc.vertex_layout.stride_bytes;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.vertex_layout.attributes.size());
    for (const VertexAttribute& attribute : desc.vertex_layout.attributes)
        attributes.push_back(
            {attribute.location, 0, to_vk(attribute.format), attribute.offset_bytes});
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (!attributes.empty()) {
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();
    }

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // Pinned M0 fixed state (types.h): cull NONE, CCW, fill, no depth/blend.
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = desc.uses_texture ? textured_layout_ : empty_layout_;
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;

    PipelineEntry entry{.uses_texture = desc.uses_texture};
    if (VkResult result = vkCreateGraphicsPipelines(
            device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &entry.pipeline);
        result != VK_SUCCESS)
        return {.error = vk_fault("vkCreateGraphicsPipelines", result)};
    return {.handle = pipelines_.create(entry)};
}

std::optional<base::Error> VulkanDevice::destroy_buffer(BufferHandle handle) {
    std::optional<base::Error> error;
    if (lookup(buffers_, handle, "buffer", error) == nullptr)
        return error;
    vkDeviceWaitIdle(device_); // synchronous model: nothing may be in flight
    BufferEntry entry = buffers_.release(handle);
    release_buffer(entry);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::destroy_texture(TextureHandle handle) {
    std::optional<base::Error> error;
    if (lookup(textures_, handle, "texture", error) == nullptr)
        return error;
    vkDeviceWaitIdle(device_);
    TextureEntry entry = textures_.release(handle);
    release_texture(entry);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::destroy_sampler(SamplerHandle handle) {
    std::optional<base::Error> error;
    if (lookup(samplers_, handle, "sampler", error) == nullptr)
        return error;
    vkDeviceWaitIdle(device_);
    vkDestroySampler(device_, samplers_.release(handle).sampler, nullptr);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::destroy_shader(ShaderHandle handle) {
    std::optional<base::Error> error;
    if (lookup(shaders_, handle, "shader", error) == nullptr)
        return error;
    // Pipelines snapshot their stages (device.h): the module can go now.
    vkDestroyShaderModule(device_, shaders_.release(handle).module, nullptr);
    return std::nullopt;
}

std::optional<base::Error> VulkanDevice::destroy_pipeline(PipelineHandle handle) {
    std::optional<base::Error> error;
    if (lookup(pipelines_, handle, "pipeline", error) == nullptr)
        return error;
    vkDeviceWaitIdle(device_);
    vkDestroyPipeline(device_, pipelines_.release(handle).pipeline, nullptr);
    return std::nullopt;
}

} // namespace midday::rhi::vk

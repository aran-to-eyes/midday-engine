// core/rhi/vulkan/vk_device.cpp — bring-up, teardown, device selection,
// transient submission plumbing (m0-rhi-vulkan). Resources live in
// vk_resources.cpp, recording/submission/readback in vk_commands.cpp.

#include "core/rhi/vulkan/vk_internal.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace midday::rhi::vk {

namespace {

// M0 process model: volk's global function table serves ONE live device
// (vulkan_device.h). Sequential create/destroy is fine; concurrent isn't.
bool g_device_live = false;

bool loader_present() {
    static const bool kPresent = volkInitialize() == VK_SUCCESS;
    return kPresent;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* /*user*/) {
    const char* level =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 ? "error" : "warning";
    if (data != nullptr && data->pMessage != nullptr)
        std::fprintf(stderr, "vulkan validation %s: %s\n", level, data->pMessage);
    return VK_FALSE;
}

int type_rank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 0;
    default:
        return -1;
    }
}

std::string version_string(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

base::Error unavailable(std::string message) {
    return base::Error{.code = "rhi.unavailable", .message = std::move(message)};
}

// First graphics-capable queue family, or none.
std::optional<std::uint32_t> graphics_family(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            return i;
    return std::nullopt;
}

} // namespace

base::Error vk_fault(std::string_view what, VkResult result) {
    base::Error error{.code = "rhi.device_fault",
                      .message = std::string(what) + " failed (" + vk_result_name(result) + ")"};
    error.details.set("vk_result", vk_result_name(result));
    error.details.set("vk_result_code", static_cast<std::int64_t>(result));
    return error;
}

const char* vk_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default:
        return "VK_ERROR_(unlisted)";
    }
}

DeviceResult create_device(const VulkanDeviceOptions& options) {
    if (g_device_live)
        return {.error = base::Error{
                    .code = "rhi.unsupported",
                    .message = "one live VulkanDevice per process in M0 (volk global mode)"}};
    auto device = std::make_unique<VulkanDevice>();
    if (auto error = device->initialize(options))
        return {.error = std::move(error)}; // ~VulkanDevice tears down partial state
    g_device_live = true;
    return {.device = std::move(device)};
}

std::optional<base::Error> VulkanDevice::initialize(const VulkanDeviceOptions& options) {
    if (!loader_present())
        return unavailable("Vulkan loader not found (no libvulkan on this host)");

    const std::uint32_t loader_version = volkGetInstanceVersion();
    if (loader_version < VK_API_VERSION_1_2)
        return unavailable("Vulkan loader/instance version below 1.2 (" +
                           version_string(loader_version) + ")");

    // -- Instance -------------------------------------------------------------
    std::vector<const char*> layers;
    if (options.enable_validation) {
        std::uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        const bool present = std::ranges::any_of(available, [](const VkLayerProperties& layer) {
            return std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
        });
        if (!present)
            return base::Error{.code = "rhi.validation_unavailable",
                               .message =
                                   "VK_LAYER_KHRONOS_validation requested but not installed"};
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    std::uint32_t extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extensions.data());
    const auto has_extension = [&](std::string_view name) {
        return std::ranges::any_of(available_extensions, [&](const VkExtensionProperties& ext) {
            return std::string_view(ext.extensionName) == name;
        });
    };

    std::vector<const char*> extensions;
    VkInstanceCreateFlags instance_flags = 0;
    // Portability drivers (MoltenVK) are HIDDEN by loaders >= 1.3.207 unless
    // the instance opts in — without this, macOS reports INCOMPATIBLE_DRIVER
    // with a perfectly good ICD installed. Opt in whenever the extension
    // exists; it is a no-op on conformant-driver hosts (lavapipe, desktop).
    if (has_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    const bool debug_utils =
        options.enable_validation && has_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "midday";
    app.pEngineName = "midday";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.flags = instance_flags;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    if (VkResult result = vkCreateInstance(&instance_info, nullptr, &instance_);
        result != VK_SUCCESS)
        return result == VK_ERROR_INCOMPATIBLE_DRIVER
                   ? unavailable("no Vulkan ICD accepts a 1.2 instance")
                   : vk_fault("vkCreateInstance", result);
    volkLoadInstance(instance_);

    if (debug_utils) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        messenger_info.pfnUserCallback = debug_callback;
        (void)vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &messenger_);
    }

    // -- Physical device (deterministic selection) -----------------------------
    std::uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0)
        return unavailable("no Vulkan physical devices (no ICD present)");
    std::vector<VkPhysicalDevice> physicals(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, physicals.data());

    int best_score = -1;
    for (std::uint32_t i = 0; i < device_count; ++i) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicals[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_2 || !graphics_family(physicals[i]).has_value())
            continue;
        const bool software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (options.require_software && !software)
            continue;
        // Deterministic: rank by type (inverted when software is required so
        // the CPU device wins), then FIRST enumeration index among equals.
        const int score = options.require_software ? 1 : type_rank(props.deviceType);
        if (score > best_score) {
            best_score = score;
            physical_ = physicals[i];
        }
    }
    if (physical_ == VK_NULL_HANDLE)
        return unavailable(options.require_software
                               ? "no software Vulkan rasterizer (lavapipe class) present"
                               : "no Vulkan 1.2 device with a graphics queue present");
    queue_family_ = *graphics_family(physical_);

    // -- Logical device + queue + VMA ------------------------------------------
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    // Portability-subset devices (MoltenVK) REQUIRE the extension enabled
    // when they advertise it (Vulkan spec rule); harmless elsewhere.
    std::vector<const char*> device_extensions;
    {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, available.data());
        if (std::ranges::any_of(available, [](const VkExtensionProperties& ext) {
                return std::string_view(ext.extensionName) == "VK_KHR_portability_subset";
            }))
            device_extensions.push_back("VK_KHR_portability_subset");
    }

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    if (VkResult result = vkCreateDevice(physical_, &device_info, nullptr, &device_);
        result != VK_SUCCESS)
        return vk_fault("vkCreateDevice", result);
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VmaVulkanFunctions vma_functions{};
    vma_functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = physical_;
    allocator_info.device = device_;
    allocator_info.instance = instance_;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
    allocator_info.pVulkanFunctions = &vma_functions;
    if (VkResult result = vmaCreateAllocator(&allocator_info, &allocator_); result != VK_SUCCESS)
        return vk_fault("vmaCreateAllocator", result);

    // -- Fixed objects ----------------------------------------------------------
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    if (VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
        result != VK_SUCCESS)
        return vk_fault("vkCreateCommandPool", result);
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &transient_pool_);
        result != VK_SUCCESS)
        return vk_fault("vkCreateCommandPool(transient)", result);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (VkResult result = vkCreateFence(device_, &fence_info, nullptr, &fence_);
        result != VK_SUCCESS)
        return vk_fault("vkCreateFence", result);

    // The one M0 render pass: RGBA8, clear -> store, ending TRANSFER_SRC so
    // rendered targets are readback-ready with no extra barrier.
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &color;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = deps;
    if (VkResult result = vkCreateRenderPass(device_, &pass_info, nullptr, &render_pass_);
        result != VK_SUCCESS)
        return vk_fault("vkCreateRenderPass", result);

    // M0 binding model: one combined image sampler at (set 0, binding 0).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    if (VkResult result =
            vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &texture_set_layout_);
        result != VK_SUCCESS)
        return vk_fault("vkCreateDescriptorSetLayout", result);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (VkResult result = vkCreatePipelineLayout(device_, &layout_info, nullptr, &empty_layout_);
        result != VK_SUCCESS)
        return vk_fault("vkCreatePipelineLayout", result);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &texture_set_layout_;
    if (VkResult result = vkCreatePipelineLayout(device_, &layout_info, nullptr, &textured_layout_);
        result != VK_SUCCESS)
        return vk_fault("vkCreatePipelineLayout(textured)", result);

    // -- Caps --------------------------------------------------------------------
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical_, &props2);
    caps_.backend = "vulkan";
    caps_.device_name = props2.properties.deviceName;
    caps_.driver_info =
        std::string(driver.driverName) +
        (driver.driverInfo[0] != '\0' ? std::string(" ") + driver.driverInfo : std::string());
    caps_.api_version = version_string(props2.properties.apiVersion);
    caps_.max_texture_size = props2.properties.limits.maxImageDimension2D;
    caps_.software_rasterizer = props2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    caps_.validation_enabled = options.enable_validation;
    return std::nullopt;
}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        // Live-resource sweep, ascending slot order per pool (deterministic).
        lists_.for_each_live([this](CommandListEntry& entry) {
            if (entry.descriptors != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device_, entry.descriptors, nullptr);
            if (entry.commands != VK_NULL_HANDLE)
                vkFreeCommandBuffers(device_, command_pool_, 1, &entry.commands);
        });
        pipelines_.for_each_live(
            [this](PipelineEntry& entry) { vkDestroyPipeline(device_, entry.pipeline, nullptr); });
        shaders_.for_each_live(
            [this](ShaderEntry& entry) { vkDestroyShaderModule(device_, entry.module, nullptr); });
        samplers_.for_each_live(
            [this](SamplerEntry& entry) { vkDestroySampler(device_, entry.sampler, nullptr); });
        textures_.for_each_live([this](TextureEntry& entry) { release_texture(entry); });
        buffers_.for_each_live([this](BufferEntry& entry) { release_buffer(entry); });

        vkDestroyPipelineLayout(device_, textured_layout_, nullptr);
        vkDestroyPipelineLayout(device_, empty_layout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, texture_set_layout_, nullptr);
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        vkDestroyFence(device_, fence_, nullptr);
        vkDestroyCommandPool(device_, transient_pool_, nullptr);
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        if (allocator_ != VK_NULL_HANDLE)
            vmaDestroyAllocator(allocator_);
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE)
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
    g_device_live = false;
}

const DeviceCaps& VulkanDevice::caps() const {
    return caps_;
}

std::optional<base::Error>
VulkanDevice::with_transient_commands(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = transient_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (VkResult result = vkAllocateCommandBuffers(device_, &alloc_info, &commands);
        result != VK_SUCCESS)
        return vk_fault("vkAllocateCommandBuffers(transient)", result);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin_info);
    fn(commands);
    vkEndCommandBuffer(commands);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    std::optional<base::Error> error;
    if (VkResult result = vkQueueSubmit(queue_, 1, &submit, fence_); result != VK_SUCCESS)
        error = vk_fault("vkQueueSubmit(transient)", result);
    if (!error) {
        if (VkResult result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
            result != VK_SUCCESS)
            error = vk_fault("vkWaitForFences(transient)", result);
        vkResetFences(device_, 1, &fence_);
    }
    vkFreeCommandBuffers(device_, transient_pool_, 1, &commands);
    return error;
}

} // namespace midday::rhi::vk

namespace midday::rhi {

DeviceResult create_vulkan_device(const VulkanDeviceOptions& options) {
    return vk::create_device(options);
}

} // namespace midday::rhi

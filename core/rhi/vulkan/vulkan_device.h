// core/rhi/vulkan/vulkan_device.h — the Vulkan backend's PUBLIC face
// (m0-rhi-vulkan). This header is deliberately Vulkan-free: consumers (CLI
// verbs, tests, the renderer later) get a factory returning rhi::RhiDevice
// and never see a VkAnything — the physics/Jolt pimpl-seam discipline.
// Backend headers (volk, VMA, vulkan_core.h) live only in vulkan/*.cpp and
// vk_internal.h; scripts/check_include_boundaries.py enforces it.
//
// Backend shape (documented here, implemented behind the seam):
//   * volk meta-loader: no libvulkan link — a host with no Vulkan loader or
//     no ICD yields a structured "rhi.unavailable" DeviceResult, never a
//     dynamic-linker failure. That is what makes `midday rhi probe` safe on
//     every machine.
//   * OFFSCREEN ONLY: no swapchain, no surface, no display-server
//     connection — headless render -> readback is the product requirement
//     (spec section 15); presentation is a much later concern.
//   * Vulkan 1.2 minimum, SPIR-V 1.5; GLSL compiles through
//     core/rhi/shadercomp (glslang) at create_shader.
//   * VMA owns allocations; single graphics queue; synchronous
//     submit_and_wait with a fence.
//   * M0 process model: ONE live VulkanDevice at a time (volk's global
//     function table). A second concurrent create returns
//     "rhi.unsupported"; sequential create/destroy cycles are fine.
//     Lifted later with per-device volk tables when multi-device arrives.

#pragma once

#include "core/rhi/device.h"

namespace midday::rhi {

struct VulkanDeviceOptions {
    // Enable VK_LAYER_KHRONOS_validation. Explicitly requesting validation
    // on a host without the layer is "rhi.validation_unavailable" — an
    // explicit ask is a hard contract, never a silent downgrade.
    bool enable_validation = false;

    // Require a software rasterizer (lavapipe/SwiftShader class). The
    // golden-software CI lane sets this so goldens can never be minted
    // against a surprise hardware GPU on a runner.
    bool require_software = false;
};

// Probes the loader, picks a physical device (discrete > integrated >
// virtual > cpu, unless require_software), and brings up the full backend.
// No ICD / no loader / no 1.2 device => {"rhi.unavailable", reason} —
// callers (selftest skip logic, `midday rhi probe`) branch on the code.
[[nodiscard]] DeviceResult create_vulkan_device(const VulkanDeviceOptions& options = {});

} // namespace midday::rhi

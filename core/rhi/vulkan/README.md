# core/rhi/vulkan

The Vulkan backend (m0-rhi-vulkan): volk meta-loader (no libvulkan link —
ICD-less hosts probe as unavailable instead of failing to start), VMA
allocation, offscreen-only (no swapchain/surface/display), Vulkan 1.2+,
GLSL via core/rhi/shadercomp at create_shader. Public face:
`vulkan_device.h` (Vulkan-free factory header — the pimpl seam). Vulkan
types exist only in `vk_internal.h` + `vk_*.cpp` (boundaries lane enforced).
Proven headless on Mesa lavapipe by the golden-software CI lane.

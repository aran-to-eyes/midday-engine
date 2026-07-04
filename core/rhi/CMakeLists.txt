# core/rhi — the Render Hardware Interface seam (m0-rhi-vulkan).
# Layering is the boundary story:
#   midday_rhi            seam + NullDevice + image + scenes; NO GPU or
#                         compiler dependency — every later node that needs a
#                         device-shaped test double links only this.
#   midday_rhi_shadercomp the one glslang consumer (GLSL -> SPIR-V);
#                         SPIRV-Cross joins here at m0-rhi-metal.
#   midday_rhi_vulkan     the Vulkan backend (volk + VMA), offscreen-only;
#                         backend headers never escape vulkan/*.cpp
#                         (scripts/check_include_boundaries.py enforces).

add_library(midday_rhi STATIC
    compare.cpp
    image.cpp
    null_device.cpp
    scenes.cpp
)
add_library(midday::rhi ALIAS midday_rhi)
target_link_libraries(midday_rhi
    PUBLIC midday::options midday::base
    PRIVATE xxhash::xxhash stb::image_write stb::image)
# Clear-color conversion and scene constants are render-side, not sim state,
# but the FP contract applies uniformly (one flag story tree-wide).
midday_apply_deterministic_fp(midday_rhi)

add_library(midday_rhi_shadercomp STATIC
    shadercomp/shader_compiler.cpp
)
add_library(midday::rhi_shadercomp ALIAS midday_rhi_shadercomp)
target_link_libraries(midday_rhi_shadercomp
    PUBLIC midday::options midday::base midday::rhi
    PRIVATE glslang::glslang)
midday_apply_deterministic_fp(midday_rhi_shadercomp)

add_library(midday_rhi_vulkan STATIC
    vulkan/vk_device.cpp
    vulkan/vk_resources.cpp
    vulkan/vk_textures.cpp
    vulkan/vk_commands.cpp
)
add_library(midday::rhi_vulkan ALIAS midday_rhi_vulkan)
target_link_libraries(midday_rhi_vulkan
    PUBLIC midday::options midday::base midday::rhi
    PRIVATE midday::rhi_shadercomp volk::volk vma::vma)
midday_apply_deterministic_fp(midday_rhi_vulkan)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_rhi_tests OBJECT
    compare_test.cpp
    image_test.cpp
    null_device_test.cpp
    scenes_test.cpp
    shadercomp/shadercomp_test.cpp
    vulkan/vulkan_test.cpp
)
target_link_libraries(midday_rhi_tests
    PRIVATE midday::rhi midday::rhi_shadercomp midday::rhi_vulkan doctest::doctest)
midday_apply_deterministic_fp(midday_rhi_tests)

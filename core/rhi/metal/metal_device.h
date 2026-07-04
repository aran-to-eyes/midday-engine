// core/rhi/metal/metal_device.h — the Metal backend's PUBLIC face
// (m0-rhi-metal). Deliberately Metal-free, like vulkan_device.h: consumers
// get a factory returning rhi::RhiDevice and never see an id<MTLAnything> —
// Metal/Foundation imports live only in metal/*.mm and mtl_internal.h
// (scripts/check_include_boundaries.py enforces).
//
// Backend shape (documented here, implemented behind the seam):
//   * NATIVE Metal (spec section 2, locked): MTLDevice + MTLCommandQueue,
//     offscreen render targets only, synchronous commit -> wait -> readback.
//     MoltenVK remains the VULKAN backend's macOS transport; this backend is
//     the proof the seam spans two dissimilar APIs (MILESTONE_0 item 23).
//   * Coordinate contract (types.h, pinned): the seam is Vulkan-convention
//     y-DOWN clip space. The adaptation to Metal's y-up NDC happens in the
//     VERTEX STAGE — shadercomp's SPIR-V -> MSL translation appends one
//     exact clip-space y negation (flip_vert_y) — never in viewports and
//     never in readback. Depth range ([0,1]) and texture row order (row 0 =
//     top) already agree, so nothing else adapts.
//   * Shaders: GLSL -> SPIR-V (glslang) -> MSL (SPIRV-Cross) through
//     core/rhi/shadercomp — one front end, two outputs.
//   * Resources are MTLStorageModePrivate textures with staged blit
//     upload/readback (the Vulkan backend's staging shape, portable across
//     unified and discrete memory) and shared-memory vertex buffers.
//   * Off macOS this factory exists and returns a structured
//     "rhi.unavailable" (metal_unavailable.cpp) — the seam builds on every
//     platform; only the implementation is Apple-only.

#pragma once

#include "core/rhi/device.h"

namespace midday::rhi {

struct MetalDeviceOptions {
    // Deliberately empty in M0: Metal has no loadable validation layer (its
    // API validation is process-environment driven) and no software
    // rasterizer class to require. The struct exists so the factory shape
    // matches the Vulkan backend and options can grow without reshaping.
};

// Picks the Metal device with the lowest registryID (deterministic on
// multi-GPU hosts) and brings up the full backend. No Metal device — or a
// non-macOS build — yields {"rhi.unavailable", reason}; callers (selftest
// skip logic, `midday rhi probe --backend metal`) branch on the code.
[[nodiscard]] DeviceResult create_metal_device(const MetalDeviceOptions& options = {});

} // namespace midday::rhi

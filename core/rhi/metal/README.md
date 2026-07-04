# core/rhi/metal

The NATIVE Metal backend (m0-rhi-metal; spec section 2 locked decision —
MoltenVK stays the Vulkan backend's macOS transport, this is the second,
dissimilar API under the seam). MTLDevice + MTLCommandQueue, offscreen
render targets only, synchronous commit -> waitUntilCompleted -> blit
readback. Shaders travel GLSL -> SPIR-V (glslang) -> MSL (SPIRV-Cross)
through core/rhi/shadercomp — one front end, two outputs.

Coordinate contract (types.h, pinned): the seam is Vulkan-convention y-DOWN
clip space; the ONE adaptation is a vertex-stage clip-space y negation
emitted by the MSL translation (flip_vert_y) — never a viewport flip, never
a readback flip (Metal's depth range and top-row-first texture order
already match the seam).

Public face: `metal_device.h` (Metal-free factory header — the pimpl seam).
Metal types exist only in `mtl_internal.h` + `mtl_*.mm` (ARC-compiled;
boundaries lane enforced). Off macOS, `metal_unavailable.cpp` keeps the
factory linkable and returns a structured `rhi.unavailable`.

Proven by the conformance corpus (core/rhi/conformance_test.cpp) on every
backend and by the cross-backend compare — the same triangle/textured-quad
scenes through Vulkan AND Metal within tier-2 tolerance — locally (Apple M4
vs MoltenVK) and in the build-macos lane.

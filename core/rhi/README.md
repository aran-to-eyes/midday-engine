# core/rhi

The Render Hardware Interface seam (m0-rhi-vulkan; spec section 5). Start at
`rhi.h` — it maps the directory. The boundary is mechanical: no backend
header (vulkan/volk/VMA, glslang/SPIRV, Metal) may appear outside its
backend directory; `scripts/check_include_boundaries.py` + the CI
`boundaries` lane enforce it.

- seam: `handle.h` (generational typed handles), `types.h` (descriptors,
  caps, errors), `command_state.h` (shared record->submit machine),
  `validate.h` (shared refusals), `device.h` (RhiDevice)
- `null_device.*` — the no-GPU seam double (protocol truth, CPU-only)
- `image.*` — RGBA8 readback container, decoded-pixel XXH3 hash, PNG
- `scenes.*` + `goldens.h` — the three pinned M0 scenes + golden file format
- `shadercomp/` — GLSL -> SPIR-V (glslang); SPIRV-Cross joins at m0-rhi-metal
- `vulkan/` — the Vulkan backend (volk + VMA, offscreen only)
- `metal/` — arrives at m0-rhi-metal

# core/rhi

The Render Hardware Interface seam (m0-rhi-vulkan; spec section 5). Start at
`rhi.h` — it maps the directory. The boundary is mechanical: no backend
header (vulkan/volk/VMA, glslang/SPIRV, Metal) may appear outside its
backend directory; `scripts/check_include_boundaries.py` + the CI
`boundaries` lane enforce it.

- seam: `handle.h` (generational typed handles), `types.h` (descriptors,
  caps, errors), `command_state.h` (shared record->submit machine),
  `validate.h` (shared refusals), `handle_lookup.h` (shared null/stale
  lookup), `device.h` (RhiDevice)
- `null_device.*` — the no-GPU seam double (protocol truth, CPU-only)
- `image.*` — RGBA8 readback container, decoded-pixel XXH3 hash, PNG
- `compare.*` — the two-tier comparator (`midday shot compare`)
- `scenes.*` + `goldens.h` — the three pinned M0 scenes + golden file format
- `conformance_test.cpp` — ONE corpus, every backend (the conformance claim)
- `shadercomp/` — GLSL -> SPIR-V (glslang) and SPIR-V -> MSL (SPIRV-Cross)
- `vulkan/` — the Vulkan backend (volk + VMA, offscreen only)
- `metal/` — the native Metal backend (macOS; offscreen only)

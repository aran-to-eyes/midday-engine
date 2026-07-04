// core/rhi/rhi.h — the Render Hardware Interface seam, one include for
// consumers (m0-rhi-vulkan; spec section 5).
//
// The seam is the HARD multi-backend boundary: engine code — renderer,
// editor, tests, CLI verbs — includes this header (or its parts) and speaks
// RhiDevice; it NEVER includes a Vulkan/Metal/D3D header. The boundary is
// mechanical, not conventional: scripts/check_include_boundaries.py fails
// the build when a backend header appears outside its backend directory
// (CI lane `boundaries`).
//
// Layout:
//   handle.h         generational typed handles + the shared HandlePool
//   types.h          formats, descriptors, caps, result envelopes, errors
//   command_state.h  the record -> submit state machine (shared validation)
//   device.h         the RhiDevice interface
//   null_device.h    rhi::NullDevice — no GPU: proves seam semantics
//                    (handles, state machine, error paths) CPU-only and
//                    serves every later node that needs a device-shaped test
//                    double
//   image.h          ImageRgba8 + PNG transport + decoded-pixel XXH3 hashing
//   scenes.h         the three pinned M0 scenes (clear/triangle/quad)
//   shadercomp/      GLSL -> SPIR-V (glslang), the one compiler seam
//   vulkan/          the Vulkan backend (volk + VMA, offscreen only)
//   metal/           the Metal backend (m0-rhi-metal)
//
// Backends are obtained through per-backend factory headers whose own
// includes stay clean (core/rhi/vulkan/vulkan_device.h) — the pimpl seam
// discipline core/physics established for Jolt.

#pragma once

#include "core/rhi/command_state.h" // IWYU pragma: export
#include "core/rhi/device.h"        // IWYU pragma: export
#include "core/rhi/handle.h"        // IWYU pragma: export
#include "core/rhi/types.h"         // IWYU pragma: export

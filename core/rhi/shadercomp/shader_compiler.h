// core/rhi/shadercomp/shader_compiler.h — the ONE shader compilation seam:
// GLSL -> SPIR-V (glslang, m0-rhi-vulkan) and SPIR-V -> MSL (SPIRV-Cross,
// m0-rhi-metal). One front end, two outputs — shader authors never learn a
// second dialect (spec section 5: GLSL via glslang, no custom shading DSL,
// ever; MSL via SPIRV-Cross). glslang and SPIRV-Cross headers live only in
// this directory's TUs (boundaries lane enforces).
//
// Determinism: compilation is pure — same (stage, source, entry) => same
// SPIR-V words on every platform. No optimizer runs (SPIRV-Tools is not
// vendored), no debug info is generated, and glslang itself compiles under
// the repo's deterministic-FP contract (its constant folding is FP work).
// The rhi.shadercomp selftests pin dual-compile byte identity.
//
// The runtime-compile-at-creation shape is M0; the shader/pipeline CACHE is
// a later SRP node that layers on top of this function, keyed by content
// hash (the ts/toolchain precedent).

#pragma once

#include "core/base/error.h"
#include "core/rhi/types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midday::rhi::shadercomp {

struct SpirvResult {
    std::vector<std::uint32_t> words{}; // valid SPIR-V (magic 0x07230203) iff ok()
    std::optional<base::Error> error = std::nullopt;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Compiles Vulkan-flavored GLSL (#version 450) to SPIR-V 1.5 for a
// Vulkan 1.2 client. Front-end refusal returns "rhi.shader_compile" with
// the glslang log under details {stage, debug_name, log}; the compiler
// process is initialized lazily on first use (process-lifetime, the Godot
// pattern — glslang's global pools are refcounted, never torn down early).
[[nodiscard]] SpirvResult compile_glsl(ShaderStage stage,
                                       std::string_view source,
                                       std::string_view entry_point,
                                       std::string_view debug_name);

struct MslResult {
    std::string source{};      // MSL text, non-empty iff ok()
    std::string entry_point{}; // MSL-side function name (SPIRV-Cross cleanses
                               // reserved names: GLSL "main" becomes "main0")
    std::optional<base::Error> error = std::nullopt;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Translates seam SPIR-V (compile_glsl output) to Metal Shading Language
// (macOS, MSL 2.2). The pinned coordinate contract (types.h: clip space
// y-DOWN) is preserved by construction: vertex stages emit a final
// clip-space y negation (SPIRV-Cross flip_vert_y), so the SAME SPIR-V
// rasterizes identically under Metal's y-up NDC — the adaptation lives HERE
// in the vertex stage, never in viewports or readback, and float negation is
// exact. Combined image samplers keep their binding number as the MSL
// texture/sampler index: (set 0, binding 0) -> [[texture(0)]]/[[sampler(0)]]
// (the M0 binding model cmd_bind_texture relies on). Refusal is
// "rhi.shader_compile" with the SPIRV-Cross message under details
// {stage, debug_name, log}. Translation is pure: same words, same MSL bytes
// on every platform (deterministic-FP contract, dual-translate pinned).
[[nodiscard]] MslResult msl_from_spirv(ShaderStage stage,
                                       std::span<const std::uint32_t> words,
                                       std::string_view debug_name);

} // namespace midday::rhi::shadercomp

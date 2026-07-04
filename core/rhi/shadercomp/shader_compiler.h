// core/rhi/shadercomp/shader_compiler.h — GLSL -> SPIR-V, the ONE shader
// compilation seam (m0-rhi-vulkan). glslang lives behind this header; when
// m0-rhi-metal arrives, SPIRV-Cross joins HERE (SPIR-V -> MSL) so both
// backends share a single front end and shader authors never learn a second
// dialect (spec section 5: GLSL via glslang, no custom shading DSL, ever).
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

} // namespace midday::rhi::shadercomp

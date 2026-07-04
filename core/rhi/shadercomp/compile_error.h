// core/rhi/shadercomp/compile_error.h — the structured "rhi.shader_compile"
// refusal, shared by the seam's two compiler TUs (glslang front end,
// SPIRV-Cross MSL translation): one details shape {stage, debug_name, log}
// whichever phase refuses, so agents debug every shader failure from the
// same envelope fields.

#pragma once

#include "core/base/error.h"
#include "core/rhi/types.h"

#include <string>
#include <string_view>
#include <utility>

namespace midday::rhi::shadercomp {

inline base::Error compile_error(ShaderStage stage,
                                 std::string_view debug_name,
                                 std::string log,
                                 std::string_view phase) {
    base::Error error{.code = "rhi.shader_compile",
                      .message = std::string(phase) + " failed for " +
                                 std::string(to_string(stage)) + " shader '" +
                                 std::string(debug_name) + "'"};
    error.details.set("stage", to_string(stage));
    error.details.set("debug_name", debug_name);
    error.details.set("log", std::move(log));
    return error;
}

} // namespace midday::rhi::shadercomp

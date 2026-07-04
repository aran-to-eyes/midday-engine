// core/rhi/shadercomp/msl_translator.cpp — the only translation unit in the
// tree that includes SPIRV-Cross headers (boundaries lane enforced).
// SPIR-V -> MSL for the native Metal backend; see shader_compiler.h for the
// coordinate-contract adaptation this TU owns (vertex-stage y flip).

#include "core/rhi/shadercomp/compile_error.h"
#include "core/rhi/shadercomp/shader_compiler.h"

#include <exception>
#include <spirv_cross/spirv_msl.hpp>
#include <string>

namespace midday::rhi::shadercomp {

MslResult msl_from_spirv(ShaderStage stage,
                         std::span<const std::uint32_t> words,
                         std::string_view debug_name) {
    if (words.empty())
        return {.error = base::Error{.code = "rhi.invalid_argument",
                                     .message = "SPIR-V word stream is empty ('" +
                                                std::string(debug_name) + "')"}};
    // SPIRV-Cross reports refusals (malformed words, unsupported constructs)
    // by throwing CompilerError; the seam speaks structured Errors, so the
    // exception stops HERE — nothing above this frame ever sees a throw.
    try {
        spirv_cross::CompilerMSL compiler(words.data(), words.size());

        spirv_cross::CompilerMSL::Options msl_options = compiler.get_msl_options();
        msl_options.platform = spirv_cross::CompilerMSL::Options::macOS;
        msl_options.set_msl_version(2, 2); // macOS 10.15+ class, well below any M0 host
        compiler.set_msl_options(msl_options);

        // The pinned clip contract (types.h): the seam is y-DOWN everywhere;
        // Metal's NDC is y-up, so vertex stages get one exact negation of
        // gl_Position.y appended by the translator. Fragment stages need no
        // adaptation (Metal's framebuffer row 0 is the top row, like the
        // seam's readback contract).
        spirv_cross::CompilerGLSL::Options common = compiler.get_common_options();
        common.vertex.flip_vert_y = stage == ShaderStage::kVertex;
        compiler.set_common_options(common);

        const auto entry_points = compiler.get_entry_points_and_stages();
        if (entry_points.empty())
            return {.error = compile_error(
                        stage, debug_name, "module declares no entry points", "MSL translation")};

        MslResult result;
        result.source = compiler.compile();
        // MSL reserves "main"; SPIRV-Cross cleanses the name ("main0") and
        // the backend must ask for the function under the cleansed name.
        result.entry_point = compiler.get_cleansed_entry_point_name(
            entry_points.front().name, entry_points.front().execution_model);
        return result;
    } catch (const std::exception& error) {
        return {.error = compile_error(stage, debug_name, error.what(), "MSL translation")};
    }
}

} // namespace midday::rhi::shadercomp

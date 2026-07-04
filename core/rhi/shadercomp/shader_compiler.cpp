// core/rhi/shadercomp/shader_compiler.cpp — the only translation unit in the
// tree that includes glslang headers (boundaries lane enforced).

#include "core/rhi/shadercomp/shader_compiler.h"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <string>
#include <utility>

namespace midday::rhi::shadercomp {

namespace {

// glslang's process-global pools: initialize once, keep for process
// lifetime (single-threaded boot contract, reflect D-BUILD-023 ethos;
// ShInitialize/ShFinalize are refcounted but tearing down between compiles
// would only add nondeterministic allocator churn).
bool ensure_initialized() {
    static const bool kInitialized = glslang::InitializeProcess();
    return kInitialized;
}

EShLanguage to_glslang(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::kVertex:
        return EShLangVertex;
    case ShaderStage::kFragment:
        return EShLangFragment;
    }
    return EShLangVertex; // unreachable: enum is exhaustive
}

base::Error compile_error(ShaderStage stage,
                          std::string_view debug_name,
                          std::string log,
                          std::string_view phase) {
    base::Error error{.code = "rhi.shader_compile",
                      .message = "GLSL " + std::string(phase) + " failed for " +
                                 std::string(to_string(stage)) + " shader '" +
                                 std::string(debug_name) + "'"};
    error.details.set("stage", to_string(stage));
    error.details.set("debug_name", debug_name);
    error.details.set("log", std::move(log));
    return error;
}

} // namespace

SpirvResult compile_glsl(ShaderStage stage,
                         std::string_view source,
                         std::string_view entry_point,
                         std::string_view debug_name) {
    if (!ensure_initialized())
        return {.error = base::Error{.code = "rhi.shader_compile",
                                     .message = "glslang process initialization failed"}};
    if (source.empty())
        return {.error = base::Error{.code = "rhi.invalid_argument",
                                     .message = "shader source is empty ('" +
                                                std::string(debug_name) + "')"}};

    const EShLanguage language = to_glslang(stage);
    glslang::TShader shader(language);

    const char* source_ptr = source.data();
    const int source_len = static_cast<int>(source.size());
    const char* name_ptr = debug_name.empty() ? "shader" : debug_name.data();
    // Lengths given explicitly: string_views need not be NUL-terminated.
    // (debug_name comes from ShaderDesc::debug_name, a std::string — its
    // data() IS terminated; source uses the length-aware setter.)
    shader.setStringsWithLengths(&source_ptr, &source_len, 1);
    const std::string entry(entry_point.empty() ? "main" : entry_point);
    shader.setEntryPoint(entry.c_str());
    shader.setSourceEntryPoint(entry.c_str());

    // The seam contract (types.h): Vulkan 1.2 client, SPIR-V 1.5.
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 100, false, messages))
        return {.error = compile_error(stage, name_ptr, shader.getInfoLog(), "parse")};

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
        return {.error = compile_error(stage, name_ptr, program.getInfoLog(), "link")};

    glslang::TIntermediate* intermediate = program.getIntermediate(language);
    if (intermediate == nullptr)
        return {.error = compile_error(stage, name_ptr, program.getInfoLog(), "lowering")};

    SpirvResult result;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.generateDebugInfo = false; // deterministic words: no paths, no text
    options.stripDebugInfo = true;
    options.disableOptimizer = true; // no SPIRV-Tools in the tree (ENABLE_OPT off)
    options.validate = false;
    glslang::GlslangToSpv(*intermediate, result.words, &logger, &options);
    if (result.words.empty())
        return {.error = compile_error(stage, name_ptr, logger.getAllMessages(), "SPIR-V emit")};
    return result;
}

} // namespace midday::rhi::shadercomp

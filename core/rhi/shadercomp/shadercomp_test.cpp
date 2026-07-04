// core/rhi/shadercomp/shadercomp_test.cpp — GLSL -> SPIR-V front end
// (rhi.shadercomp.*): valid output shape, deterministic words, structured
// refusals with the compiler log. Pure CPU — runs on every platform/lane.

#include "core/rhi/shadercomp/shader_compiler.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <cstdint>
#include <optional>
#include <string>

namespace {

using namespace midday::rhi;

// tidy-clean optional access: the code of an expected error, or a marker
// that fails the string compare when the optional is empty.
std::string err_code(std::optional<midday::base::Error> error) {
    return error.has_value() ? error->code : std::string("(no error)");
}

constexpr std::string_view kMinimalVert = R"(#version 450
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";

constexpr std::string_view kMinimalFrag = R"(#version 450
layout(location = 0) out vec4 o_color;
void main() { o_color = vec4(1.0); }
)";

TEST_CASE("rhi.shadercomp.valid_glsl_yields_spirv_1_5") {
    shadercomp::SpirvResult vert =
        shadercomp::compile_glsl(ShaderStage::kVertex, kMinimalVert, "main", "t.vert");
    REQUIRE(vert.ok());
    REQUIRE(vert.words.size() > 5);
    CHECK(vert.words[0] == 0x07230203u); // SPIR-V magic
    CHECK(vert.words[1] == 0x00010500u); // version 1.5 (Vulkan 1.2 target)

    shadercomp::SpirvResult frag =
        shadercomp::compile_glsl(ShaderStage::kFragment, kMinimalFrag, "main", "t.frag");
    REQUIRE(frag.ok());
    CHECK(frag.words[0] == 0x07230203u);
}

TEST_CASE("rhi.shadercomp.compilation_is_deterministic") {
    // Two independent compiles, identical words — the property the future
    // shader cache keys on (and the golden lane leans on transitively).
    shadercomp::SpirvResult first =
        shadercomp::compile_glsl(ShaderStage::kVertex, kMinimalVert, "main", "a");
    shadercomp::SpirvResult second =
        shadercomp::compile_glsl(ShaderStage::kVertex, kMinimalVert, "main", "b");
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(first.words == second.words); // debug_name must not reach the words
}

TEST_CASE("rhi.shadercomp.parse_error_is_structured") {
    const std::string bad = "#version 450\nvoid main() { this is not glsl; }\n";
    shadercomp::SpirvResult result =
        shadercomp::compile_glsl(ShaderStage::kFragment, bad, "main", "broken.frag");
    REQUIRE_FALSE(result.ok());
    const midday::base::Error& error = midday::testkit::unwrap(result.error);
    CHECK(error.code == "rhi.shader_compile");
    const midday::base::Json* stage = error.details.find("stage");
    const midday::base::Json* name = error.details.find("debug_name");
    const midday::base::Json* log = error.details.find("log");
    REQUIRE(stage != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(log != nullptr);
    CHECK(stage->as_string() == "fragment");
    CHECK(name->as_string() == "broken.frag");
    // The glslang log reaches the caller (agents debug from the envelope).
    CHECK_FALSE(log->as_string().empty());
}

TEST_CASE("rhi.shadercomp.empty_source_refuses") {
    shadercomp::SpirvResult result =
        shadercomp::compile_glsl(ShaderStage::kVertex, "", "main", "empty");
    REQUIRE_FALSE(result.ok());
    CHECK(err_code(result.error) == "rhi.invalid_argument");
}

} // namespace

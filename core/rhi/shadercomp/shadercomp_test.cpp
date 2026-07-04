// core/rhi/shadercomp/shadercomp_test.cpp — the shader seam's two outputs
// (rhi.shadercomp.*): GLSL -> SPIR-V (valid shape, deterministic words,
// structured refusals with the compiler log) and SPIR-V -> MSL (translation
// pins the Metal backend leans on: vertex y flip, cleansed entry point,
// texture/sampler index mapping). Pure CPU — runs on every platform/lane.

#include "core/rhi/shadercomp/shader_compiler.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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

// -- SPIR-V -> MSL (the Metal backend's shader path) --------------------------

constexpr std::string_view kTexturedFrag = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() { o_color = texture(u_texture, v_uv); }
)";

shadercomp::MslResult translate(ShaderStage stage, std::string_view glsl, const char* name) {
    shadercomp::SpirvResult spirv = shadercomp::compile_glsl(stage, glsl, "main", name);
    REQUIRE(spirv.ok());
    return shadercomp::msl_from_spirv(stage, spirv.words, name);
}

TEST_CASE("rhi.shadercomp.msl_translates_both_stages_with_cleansed_entry") {
    shadercomp::MslResult vert = translate(ShaderStage::kVertex, kMinimalVert, "t.vert");
    REQUIRE(vert.ok());
    CHECK(vert.source.find("#include <metal_stdlib>") != std::string::npos);
    CHECK(vert.source.find("vertex ") != std::string::npos);
    // MSL reserves "main": the backend must look the function up under the
    // cleansed name or newFunctionWithName returns nil.
    CHECK(vert.entry_point == "main0");

    shadercomp::MslResult frag = translate(ShaderStage::kFragment, kMinimalFrag, "t.frag");
    REQUIRE(frag.ok());
    CHECK(frag.source.find("fragment ") != std::string::npos);
    CHECK(frag.entry_point == "main0");
}

TEST_CASE("rhi.shadercomp.msl_vertex_stage_flips_clip_y") {
    // The coordinate contract's ONE adaptation point (types.h: seam clip
    // space is y-DOWN, Metal NDC is y-up): the translated vertex stage must
    // negate gl_Position.y; fragment stages must not be touched.
    shadercomp::MslResult vert = translate(ShaderStage::kVertex, kMinimalVert, "flip.vert");
    REQUIRE(vert.ok());
    CHECK(vert.source.find(".y = -(") != std::string::npos);

    shadercomp::MslResult frag = translate(ShaderStage::kFragment, kMinimalFrag, "flip.frag");
    REQUIRE(frag.ok());
    CHECK(frag.source.find(".y = -(") == std::string::npos);
}

TEST_CASE("rhi.shadercomp.msl_combined_sampler_maps_to_index_zero") {
    // (set 0, binding 0) -> [[texture(0)]] + [[sampler(0)]] — the M0 binding
    // model's Metal spelling (cmd_bind_texture binds both at index 0).
    shadercomp::MslResult frag = translate(ShaderStage::kFragment, kTexturedFrag, "tex.frag");
    REQUIRE(frag.ok());
    CHECK(frag.source.find("[[texture(0)]]") != std::string::npos);
    CHECK(frag.source.find("[[sampler(0)]]") != std::string::npos);
}

TEST_CASE("rhi.shadercomp.msl_translation_is_deterministic") {
    shadercomp::MslResult first = translate(ShaderStage::kVertex, kMinimalVert, "a");
    shadercomp::MslResult second = translate(ShaderStage::kVertex, kMinimalVert, "b");
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(first.source == second.source); // debug_name must not reach the text
}

TEST_CASE("rhi.shadercomp.msl_refusals_are_structured") {
    shadercomp::MslResult empty = shadercomp::msl_from_spirv(ShaderStage::kVertex, {}, "empty");
    REQUIRE_FALSE(empty.ok());
    CHECK(err_code(empty.error) == "rhi.invalid_argument");

    const std::array<std::uint32_t, 4> garbage = {0xDEADBEEFu, 1u, 2u, 3u};
    shadercomp::MslResult bad =
        shadercomp::msl_from_spirv(ShaderStage::kFragment, garbage, "garbage");
    REQUIRE_FALSE(bad.ok());
    const midday::base::Error& error = midday::testkit::unwrap(bad.error);
    CHECK(error.code == "rhi.shader_compile");
    const midday::base::Json* log = error.details.find("log");
    REQUIRE(log != nullptr);
    CHECK_FALSE(log->as_string().empty()); // the SPIRV-Cross message travels
}

} // namespace

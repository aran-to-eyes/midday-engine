// core/rhi/null_device_test.cpp — the seam semantics, proven CPU-only
// (rhi.null.*): generational handles, LIFO reuse determinism, structured
// error paths, and the shared record -> submit state machine. What passes
// here holds for every backend, because the validated code (HandlePool +
// CommandListState) IS the code the backends run.

#include "core/rhi/null_device.h"
#include "core/rhi/rhi.h"
#include "testkit/doctest.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace midday;
using namespace midday::rhi;

// tidy-clean optional access: the code of an expected error, or a marker
// that fails the string compare when the optional is empty.
std::string err_code(std::optional<midday::base::Error> error) {
    return error.has_value() ? error->code : std::string("(no error)");
}

// Typed handles: cross-kind assignment must not compile.
static_assert(!std::is_same_v<BufferHandle, TextureHandle>);
static_assert(!std::is_same_v<PipelineHandle, ShaderHandle>);

BufferHandle make_buffer(NullDevice& device, std::uint64_t size = 64) {
    BufferResult result = device.create_buffer({.size_bytes = size, .debug_name = "t"}, {});
    REQUIRE(result.ok());
    return result.handle;
}

TextureHandle make_target(NullDevice& device, std::uint32_t extent = 4) {
    TextureResult result = device.create_texture(
        {.width = extent, .height = extent, .usage = TextureUsage::kRenderTarget}, {});
    REQUIRE(result.ok());
    return result.handle;
}

struct Drawable {
    PipelineHandle pipeline;
    BufferHandle vertices;
};

Drawable make_drawable(NullDevice& device, bool uses_texture = false) {
    ShaderResult vert =
        device.create_shader({.stage = ShaderStage::kVertex, .glsl = "v", .debug_name = "v"});
    ShaderResult frag =
        device.create_shader({.stage = ShaderStage::kFragment, .glsl = "f", .debug_name = "f"});
    REQUIRE(vert.ok());
    REQUIRE(frag.ok());
    PipelineDesc desc;
    desc.vertex_shader = vert.handle;
    desc.fragment_shader = frag.handle;
    desc.uses_texture = uses_texture;
    PipelineResult pipeline = device.create_pipeline(desc);
    REQUIRE(pipeline.ok());
    return {pipeline.handle, make_buffer(device)};
}

TEST_CASE("rhi.null.handle_generations_and_lifo_reuse") {
    NullDevice device;

    const BufferHandle first = make_buffer(device);
    CHECK(first.generation == 0);
    CHECK_FALSE(first.is_null());

    // Destroy stales the handle forever; the slot recycles with generation+1.
    CHECK_FALSE(device.destroy_buffer(first).has_value());
    const BufferHandle second = make_buffer(device);
    CHECK(second.index == first.index); // LIFO reuse
    CHECK(second.generation == first.generation + 1);

    // The stale handle is DETECTED, structurally.
    std::array<std::byte, 64> scratch{};
    auto error = device.destroy_buffer(first);
    REQUIRE(error.has_value());
    CHECK(err_code(error) == "rhi.stale_handle");
    (void)scratch;

    // Null handles have their own code.
    error = device.destroy_buffer(BufferHandle{});
    REQUIRE(error.has_value());
    CHECK(err_code(error) == "rhi.null_handle");

    // to_bits round trip (journal form, generation high).
    const std::uint64_t bits = second.to_bits();
    CHECK(BufferHandle::from_bits(bits) == second);
    CHECK((bits >> 32u) == second.generation);
}

TEST_CASE("rhi.null.handle_assignment_is_deterministic") {
    // Two identical op scripts yield identical handle sequences: allocation
    // is a pure function of the create/destroy order (LIFO free list).
    std::vector<std::uint64_t> runs[2];
    for (auto& run : runs) {
        NullDevice device;
        const BufferHandle a = make_buffer(device);
        const BufferHandle b = make_buffer(device);
        (void)device.destroy_buffer(a);
        const BufferHandle c = make_buffer(device); // reuses a's slot
        (void)device.destroy_buffer(b);
        const BufferHandle d = make_buffer(device); // reuses b's slot
        run = {a.to_bits(), b.to_bits(), c.to_bits(), d.to_bits()};
    }
    CHECK(runs[0] == runs[1]);
}

TEST_CASE("rhi.null.descriptor_validation") {
    NullDevice device;

    auto zero_buffer = device.create_buffer({.size_bytes = 0}, {});
    REQUIRE_FALSE(zero_buffer.ok());
    CHECK(err_code(zero_buffer.error) == "rhi.invalid_argument");

    auto zero_texture = device.create_texture({.width = 0, .height = 4}, {});
    REQUIRE_FALSE(zero_texture.ok());
    CHECK(err_code(zero_texture.error) == "rhi.invalid_argument");

    auto huge = device.create_texture({.width = 1u << 20u, .height = 4}, {});
    REQUIRE_FALSE(huge.ok());
    CHECK(err_code(huge.error) == "rhi.unsupported");

    // Initial data must match the resource size exactly (or be absent).
    std::array<std::byte, 3> three{};
    auto mismatched = device.create_buffer({.size_bytes = 8}, three);
    REQUIRE_FALSE(mismatched.ok());
    CHECK(err_code(mismatched.error) == "rhi.size_mismatch");

    // Pipelines validate their shader handles.
    PipelineDesc desc;
    auto no_shaders = device.create_pipeline(desc);
    REQUIRE_FALSE(no_shaders.ok());
    CHECK(err_code(no_shaders.error) == "rhi.null_handle");
}

TEST_CASE("rhi.null.record_submit_state_machine") {
    NullDevice device;
    const TextureHandle target = make_target(device);
    const Drawable drawable = make_drawable(device);
    CommandListResult list = device.create_command_list();
    REQUIRE(list.ok());
    const CommandListHandle cmd = list.handle;
    const RenderPassDesc pass{.color_target = target, .clear = {}};

    // Recording ops before begin: rhi.not_recording.
    CHECK(err_code(device.cmd_begin_render_pass(cmd, pass)) == "rhi.not_recording");
    CHECK(err_code(device.cmd_end(cmd)) == "rhi.not_recording");
    // Submit before end: rhi.not_ready.
    CHECK(err_code(device.submit_and_wait(cmd)) == "rhi.not_ready");

    CHECK_FALSE(device.cmd_begin(cmd).has_value());
    CHECK(err_code(device.cmd_begin(cmd)) == "rhi.already_recording");

    // Pass-scoped ops before a pass is open: rhi.no_pass.
    CHECK(err_code(device.cmd_bind_pipeline(cmd, drawable.pipeline)) == "rhi.no_pass");
    CHECK(err_code(device.cmd_draw(cmd, 3, 0)) == "rhi.no_pass");
    CHECK(err_code(device.cmd_end_render_pass(cmd)) == "rhi.no_pass");

    CHECK_FALSE(device.cmd_begin_render_pass(cmd, pass).has_value());
    CHECK(err_code(device.cmd_begin_render_pass(cmd, pass)) == "rhi.pass_active");
    CHECK(err_code(device.cmd_end(cmd)) == "rhi.pass_active");

    // Draw prerequisites, in dependency order.
    CHECK(err_code(device.cmd_draw(cmd, 3, 0)) == "rhi.no_pipeline");
    CHECK_FALSE(device.cmd_bind_pipeline(cmd, drawable.pipeline).has_value());
    CHECK(err_code(device.cmd_draw(cmd, 3, 0)) == "rhi.no_vertex_buffer");
    CHECK_FALSE(device.cmd_bind_vertex_buffer(cmd, drawable.vertices).has_value());
    CHECK(err_code(device.cmd_draw(cmd, 0, 0)) == "rhi.invalid_argument");
    CHECK_FALSE(device.cmd_draw(cmd, 3, 0).has_value());

    CHECK_FALSE(device.cmd_end_render_pass(cmd).has_value());
    CHECK(err_code(device.cmd_end_render_pass(cmd)) == "rhi.no_pass");
    CHECK_FALSE(device.cmd_end(cmd).has_value());

    // Ready: recording ops refuse, exactly one submit consumes.
    CHECK(err_code(device.cmd_draw(cmd, 3, 0)) == "rhi.not_recording");
    CHECK_FALSE(device.submit_and_wait(cmd).has_value());
    CHECK(err_code(device.submit_and_wait(cmd)) == "rhi.not_ready");
    CHECK(device.submitted_draws() == 1);

    // The list is reusable: begin resets it.
    CHECK_FALSE(device.cmd_begin(cmd).has_value());
    CHECK_FALSE(device.cmd_begin_render_pass(cmd, pass).has_value());
    CHECK_FALSE(device.cmd_end_render_pass(cmd).has_value());
    CHECK_FALSE(device.cmd_end(cmd).has_value());
    CHECK_FALSE(device.submit_and_wait(cmd).has_value());
}

TEST_CASE("rhi.null.textured_pipeline_requires_texture") {
    NullDevice device;
    const TextureHandle target = make_target(device);
    const Drawable drawable = make_drawable(device, /*uses_texture=*/true);
    TextureResult sampled = device.create_texture({.width = 2, .height = 2}, {});
    SamplerResult sampler = device.create_sampler({});
    REQUIRE(sampled.ok());
    REQUIRE(sampler.ok());
    const CommandListHandle cmd = device.create_command_list().handle;

    REQUIRE_FALSE(device.cmd_begin(cmd).has_value());
    REQUIRE_FALSE(
        device.cmd_begin_render_pass(cmd, {.color_target = target, .clear = {}}).has_value());
    REQUIRE_FALSE(device.cmd_bind_pipeline(cmd, drawable.pipeline).has_value());
    REQUIRE_FALSE(device.cmd_bind_vertex_buffer(cmd, drawable.vertices).has_value());

    CHECK(err_code(device.cmd_draw(cmd, 3, 0)) == "rhi.texture_missing");
    CHECK(err_code(device.cmd_bind_texture(cmd, 1, sampled.handle, sampler.handle)) ==
          "rhi.unsupported"); // M0: slot 0 only
    CHECK_FALSE(device.cmd_bind_texture(cmd, 0, sampled.handle, sampler.handle).has_value());
    CHECK_FALSE(device.cmd_draw(cmd, 3, 0).has_value());
}

TEST_CASE("rhi.null.render_pass_target_validation") {
    NullDevice device;
    TextureResult sampled = device.create_texture({.width = 2, .height = 2}, {});
    REQUIRE(sampled.ok());
    const CommandListHandle cmd = device.create_command_list().handle;
    REQUIRE_FALSE(device.cmd_begin(cmd).has_value());
    auto error = device.cmd_begin_render_pass(cmd, {.color_target = sampled.handle, .clear = {}});
    REQUIRE(error.has_value());
    CHECK(err_code(error) == "rhi.invalid_argument");
}

TEST_CASE("rhi.null.clear_and_readback_pins") {
    // unorm8 conversion pins (the scene clear bytes rest on these).
    CHECK(unorm8_from_float(0.0F) == 0);
    CHECK(unorm8_from_float(1.0F) == 255);
    CHECK(unorm8_from_float(0.2F) == 51);
    CHECK(unorm8_from_float(0.4F) == 102);
    CHECK(unorm8_from_float(0.6F) == 153);
    CHECK(unorm8_from_float(-1.0F) == 0);  // clamped
    CHECK(unorm8_from_float(2.0F) == 255); // clamped

    NullDevice device;
    const TextureHandle target = make_target(device, 2);
    const CommandListHandle cmd = device.create_command_list().handle;
    REQUIRE_FALSE(device.cmd_begin(cmd).has_value());
    REQUIRE_FALSE(
        device
            .cmd_begin_render_pass(cmd, {.color_target = target, .clear = {0.2F, 0.4F, 0.6F, 1.0F}})
            .has_value());
    REQUIRE_FALSE(device.cmd_end_render_pass(cmd).has_value());
    REQUIRE_FALSE(device.cmd_end(cmd).has_value());
    REQUIRE_FALSE(device.submit_and_wait(cmd).has_value());

    std::vector<std::byte> pixels(std::size_t{2} * 2 * 4);
    // Wrong size first: structured refusal.
    std::vector<std::byte> wrong(3);
    CHECK(err_code(device.read_texture(target, wrong)) == "rhi.size_mismatch");
    REQUIRE_FALSE(device.read_texture(target, pixels).has_value());
    for (std::size_t px = 0; px < 4; ++px) {
        CHECK(std::to_integer<int>(pixels[px * 4 + 0]) == 51);
        CHECK(std::to_integer<int>(pixels[px * 4 + 1]) == 102);
        CHECK(std::to_integer<int>(pixels[px * 4 + 2]) == 153);
        CHECK(std::to_integer<int>(pixels[px * 4 + 3]) == 255);
    }
}

TEST_CASE("rhi.null.shader_destroy_after_pipeline_is_legal") {
    NullDevice device;
    const Drawable drawable = make_drawable(device);
    // Destroy both shaders; the pipeline snapshotted them (device.h contract).
    NullDevice::Counts counts = device.live_counts();
    CHECK(counts.shaders == 2);
    // (Handles for the shaders are inside make_drawable — re-derive via a
    // fresh pipeline: destroying its shaders must not invalidate drawing.)
    ShaderResult vert = device.create_shader({.stage = ShaderStage::kVertex, .glsl = "v"});
    ShaderResult frag = device.create_shader({.stage = ShaderStage::kFragment, .glsl = "f"});
    PipelineDesc desc;
    desc.vertex_shader = vert.handle;
    desc.fragment_shader = frag.handle;
    PipelineResult pipeline = device.create_pipeline(desc);
    REQUIRE(pipeline.ok());
    CHECK_FALSE(device.destroy_shader(vert.handle).has_value());
    CHECK_FALSE(device.destroy_shader(frag.handle).has_value());

    const TextureHandle target = make_target(device);
    const CommandListHandle cmd = device.create_command_list().handle;
    REQUIRE_FALSE(device.cmd_begin(cmd).has_value());
    REQUIRE_FALSE(
        device.cmd_begin_render_pass(cmd, {.color_target = target, .clear = {}}).has_value());
    CHECK_FALSE(device.cmd_bind_pipeline(cmd, pipeline.handle).has_value());
    CHECK_FALSE(device.cmd_bind_vertex_buffer(cmd, drawable.vertices).has_value());
    CHECK_FALSE(device.cmd_draw(cmd, 3, 0).has_value());
    REQUIRE_FALSE(device.cmd_end_render_pass(cmd).has_value());
    REQUIRE_FALSE(device.cmd_end(cmd).has_value());
    CHECK_FALSE(device.submit_and_wait(cmd).has_value());
}

} // namespace

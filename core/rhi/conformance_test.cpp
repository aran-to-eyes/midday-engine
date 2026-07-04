// core/rhi/conformance_test.cpp — THE cross-backend conformance corpus
// (rhi.conformance.*): backend-independent semantic invariants of the seam,
// driven through rhi::RhiDevice only, parameterized over every device the
// host can bring up (testkit/rhi_backends.h: NullDevice always, Vulkan when
// an ICD exists, Metal on macOS). ONE corpus file asserting the SAME truths
// on every backend is the conformance claim (MILESTONE_0 item 23):
//   * protocol truths (handles, state machine, shared refusal spellings)
//     hold verbatim everywhere — the backends run the same validation code;
//   * pixel truths are two-tier (D-BUILD-090): exactness that any conformant
//     rasterizer must produce (clear bytes, texel-center sampling, corner
//     color dominance) is asserted on rasterizing backends; NullDevice — a
//     double that invents no pixels — must return the untouched clear.

#include "core/rhi/rhi.h"
#include "core/rhi/scenes.h"
#include "testkit/doctest.h"
#include "testkit/rhi_backends.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace midday;
using namespace midday::rhi;
using midday::testkit::RhiBackend;

// tidy-clean optional access: the code of an expected error, or a marker
// that fails the string compare when the optional is empty.
std::string err_code(const std::optional<midday::base::Error>& error) {
    return error.has_value() ? error->code : std::string("(no error)");
}

// The corpus loop: every available backend, loud skip otherwise. INFO stays
// in scope around fn, so failures name the backend they happened on.
template <typename Fn> void for_each_backend(const char* test_name, Fn&& fn) {
    for (RhiBackend& backend : testkit::rhi_backends()) {
        INFO("backend: " << backend.name);
        RhiDevice* device = testkit::acquire(backend, test_name);
        if (device != nullptr)
            fn(backend, *device);
    }
}

bool channel_dominant(const std::array<std::uint8_t, 4>& px, int channel) {
    for (int c = 0; c < 3; ++c)
        if (c != channel &&
            px[static_cast<std::size_t>(channel)] <= px[static_cast<std::size_t>(c)])
            return false;
    return true;
}

// Corpus shaders: valid #version 450 GLSL so compiling backends accept them
// (NullDevice stores the text without compiling — same call, same handles).
constexpr std::string_view kCorpusVert = R"(#version 450
layout(location = 0) in vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }
)";
constexpr std::string_view kCorpusFrag = R"(#version 450
layout(location = 0) out vec4 o_color;
void main() { o_color = vec4(1.0, 0.0, 0.0, 1.0); }
)";
constexpr std::string_view kCorpusTexVert = R"(#version 450
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); v_uv = a_uv; }
)";
constexpr std::string_view kCorpusTexFrag = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() { o_color = texture(u_texture, v_uv); }
)";

// Everything a protocol walk creates on the SHARED device, destroyed on
// every exit path so corpus cases never leak state into each other.
struct ProtocolRig {
    RhiDevice& device;
    TextureHandle target{};
    ShaderHandle vert{};
    ShaderHandle frag{};
    PipelineHandle pipeline{};
    BufferHandle vertices{};
    TextureHandle sampled{};
    SamplerHandle sampler{};
    CommandListHandle list{};

    explicit ProtocolRig(RhiDevice& dev) : device(dev) {}

    ProtocolRig(const ProtocolRig&) = delete;
    ProtocolRig& operator=(const ProtocolRig&) = delete;
    ProtocolRig(ProtocolRig&&) = delete;
    ProtocolRig& operator=(ProtocolRig&&) = delete;

    ~ProtocolRig() {
        if (!list.is_null())
            (void)device.destroy_command_list(list);
        if (!sampler.is_null())
            (void)device.destroy_sampler(sampler);
        if (!sampled.is_null())
            (void)device.destroy_texture(sampled);
        if (!vertices.is_null())
            (void)device.destroy_buffer(vertices);
        if (!pipeline.is_null())
            (void)device.destroy_pipeline(pipeline);
        if (!frag.is_null())
            (void)device.destroy_shader(frag);
        if (!vert.is_null())
            (void)device.destroy_shader(vert);
        if (!target.is_null())
            (void)device.destroy_texture(target);
    }

    // Builds target + shaders + pipeline + a zero-filled vertex buffer (a
    // degenerate triangle: valid GPU work that rasterizes nothing) and, when
    // textured, the sampled texture + sampler + uv vertex layout.
    void build(bool textured) {
        TextureResult target_result = device.create_texture(
            {.width = 8, .height = 8, .usage = TextureUsage::kRenderTarget, .debug_name = "c.t"},
            {});
        REQUIRE(target_result.ok());
        target = target_result.handle;

        ShaderResult vert_result =
            device.create_shader({.stage = ShaderStage::kVertex,
                                  .glsl = std::string(textured ? kCorpusTexVert : kCorpusVert),
                                  .debug_name = "c.vert"});
        REQUIRE(vert_result.ok());
        vert = vert_result.handle;
        ShaderResult frag_result =
            device.create_shader({.stage = ShaderStage::kFragment,
                                  .glsl = std::string(textured ? kCorpusTexFrag : kCorpusFrag),
                                  .debug_name = "c.frag"});
        REQUIRE(frag_result.ok());
        frag = frag_result.handle;

        PipelineDesc desc;
        desc.vertex_shader = vert;
        desc.fragment_shader = frag;
        desc.uses_texture = textured;
        desc.debug_name = "c.pipeline";
        const std::uint32_t stride = textured ? 4 * sizeof(float) : 2 * sizeof(float);
        desc.vertex_layout = {
            .stride_bytes = stride,
            .attributes = {{.location = 0, .format = VertexFormat::kFloat2, .offset_bytes = 0}}};
        if (textured)
            desc.vertex_layout.attributes.push_back({.location = 1,
                                                     .format = VertexFormat::kFloat2,
                                                     .offset_bytes = 2 * sizeof(float)});
        PipelineResult pipeline_result = device.create_pipeline(desc);
        REQUIRE(pipeline_result.ok());
        pipeline = pipeline_result.handle;

        BufferResult buffer_result = device.create_buffer(
            {.size_bytes = std::uint64_t{3} * stride, .debug_name = "c.vb"}, {});
        REQUIRE(buffer_result.ok());
        vertices = buffer_result.handle;

        if (textured) {
            TextureResult sampled_result = device.create_texture(
                {.width = 2, .height = 2, .usage = TextureUsage::kSampled, .debug_name = "c.tex"},
                {});
            REQUIRE(sampled_result.ok());
            sampled = sampled_result.handle;
            SamplerResult sampler_result = device.create_sampler({.debug_name = "c.smp"});
            REQUIRE(sampler_result.ok());
            sampler = sampler_result.handle;
        }

        CommandListResult list_result = device.create_command_list();
        REQUIRE(list_result.ok());
        list = list_result.handle;
    }
};

TEST_CASE("rhi.conformance.caps_are_honest") {
    for_each_backend("rhi.conformance.caps_are_honest", [](RhiBackend& backend, RhiDevice& device) {
        const DeviceCaps& caps = device.caps();
        CHECK(caps.backend == backend.name);
        CHECK_FALSE(caps.device_name.empty());
        CHECK_FALSE(caps.api_version.empty());
        CHECK(caps.max_texture_size >= kSceneExtent);
        MESSAGE(backend.name << " device: " << caps.device_name << " | driver: " << caps.driver_info
                             << " | api: " << caps.api_version
                             << " | software: " << caps.software_rasterizer);
    });
}

TEST_CASE("rhi.conformance.handle_generations_and_lifo_reuse") {
    for_each_backend(
        "rhi.conformance.handle_generations_and_lifo_reuse", [](RhiBackend&, RhiDevice& device) {
            BufferResult a = device.create_buffer({.size_bytes = 16, .debug_name = "a"}, {});
            BufferResult b = device.create_buffer({.size_bytes = 16, .debug_name = "b"}, {});
            REQUIRE(a.ok());
            REQUIRE(b.ok());

            // Destroy stales the handle forever; LIFO recycles the slot with a
            // bumped generation (pure function of the op sequence, handle.h).
            CHECK_FALSE(device.destroy_buffer(a.handle).has_value());
            BufferResult c = device.create_buffer({.size_bytes = 16, .debug_name = "c"}, {});
            REQUIRE(c.ok());
            CHECK(c.handle.index == a.handle.index);
            CHECK(c.handle.generation == a.handle.generation + 1);

            CHECK(err_code(device.destroy_buffer(a.handle)) == "rhi.stale_handle");
            CHECK(err_code(device.destroy_buffer(BufferHandle{})) == "rhi.null_handle");
            CHECK(BufferHandle::from_bits(c.handle.to_bits()) == c.handle);

            CHECK_FALSE(device.destroy_buffer(b.handle).has_value());
            CHECK_FALSE(device.destroy_buffer(c.handle).has_value());
        });
}

TEST_CASE("rhi.conformance.shared_refusal_spellings") {
    for_each_backend(
        "rhi.conformance.shared_refusal_spellings", [](RhiBackend&, RhiDevice& device) {
            CHECK(err_code(device.create_buffer({.size_bytes = 0}, {}).error) ==
                  "rhi.invalid_argument");
            CHECK(err_code(device.create_texture({.width = 0, .height = 4}, {}).error) ==
                  "rhi.invalid_argument");
            CHECK(err_code(device
                               .create_texture(
                                   {.width = device.caps().max_texture_size + 1, .height = 4}, {})
                               .error) == "rhi.unsupported");

            std::array<std::byte, 3> three{};
            CHECK(err_code(device.create_buffer({.size_bytes = 8}, three).error) ==
                  "rhi.size_mismatch");
            CHECK(
                err_code(device.create_shader({.stage = ShaderStage::kVertex, .glsl = ""}).error) ==
                "rhi.invalid_argument");
            CHECK(err_code(device.create_pipeline({}).error) == "rhi.null_handle");

            // Sampled textures created without data have DEFINED zero content,
            // and readback validates the span size before anything else.
            TextureResult sampled = device.create_texture(
                {.width = 2, .height = 2, .usage = TextureUsage::kSampled, .debug_name = "zero"},
                {});
            REQUIRE(sampled.ok());
            std::array<std::byte, 3> wrong{};
            CHECK(err_code(device.read_texture(sampled.handle, wrong)) == "rhi.size_mismatch");
            std::array<std::byte, std::size_t{2} * 2 * 4> pixels{std::byte{0xFF}};
            REQUIRE_FALSE(device.read_texture(sampled.handle, pixels).has_value());
            for (std::byte byte : pixels)
                CHECK(std::to_integer<int>(byte) == 0);
            CHECK_FALSE(device.destroy_texture(sampled.handle).has_value());
        });
}

TEST_CASE("rhi.conformance.record_submit_protocol") {
    for_each_backend("rhi.conformance.record_submit_protocol", [](RhiBackend&, RhiDevice& dev) {
        ProtocolRig rig(dev);
        rig.build(/*textured=*/false);
        const CommandListHandle cmd = rig.list;
        const RenderPassDesc pass{.color_target = rig.target, .clear = kClearColor};

        // Recording ops before begin; submit before end.
        CHECK(err_code(dev.cmd_begin_render_pass(cmd, pass)) == "rhi.not_recording");
        CHECK(err_code(dev.cmd_end(cmd)) == "rhi.not_recording");
        CHECK(err_code(dev.submit_and_wait(cmd)) == "rhi.not_ready");

        CHECK_FALSE(dev.cmd_begin(cmd).has_value());
        CHECK(err_code(dev.cmd_begin(cmd)) == "rhi.already_recording");

        // Pass-scoped ops before a pass is open.
        CHECK(err_code(dev.cmd_bind_pipeline(cmd, rig.pipeline)) == "rhi.no_pass");
        CHECK(err_code(dev.cmd_draw(cmd, 3, 0)) == "rhi.no_pass");
        CHECK(err_code(dev.cmd_end_render_pass(cmd)) == "rhi.no_pass");

        // A non-render-target texture refuses as a pass target.
        TextureResult sampled = dev.create_texture(
            {.width = 2, .height = 2, .usage = TextureUsage::kSampled, .debug_name = "c.nrt"}, {});
        REQUIRE(sampled.ok());
        CHECK(err_code(dev.cmd_begin_render_pass(
                  cmd, {.color_target = sampled.handle, .clear = {}})) == "rhi.invalid_argument");
        CHECK_FALSE(dev.destroy_texture(sampled.handle).has_value());

        CHECK_FALSE(dev.cmd_begin_render_pass(cmd, pass).has_value());
        CHECK(err_code(dev.cmd_begin_render_pass(cmd, pass)) == "rhi.pass_active");
        CHECK(err_code(dev.cmd_end(cmd)) == "rhi.pass_active");

        // Draw prerequisites, in dependency order.
        CHECK(err_code(dev.cmd_draw(cmd, 3, 0)) == "rhi.no_pipeline");
        CHECK_FALSE(dev.cmd_bind_pipeline(cmd, rig.pipeline).has_value());
        CHECK(err_code(dev.cmd_draw(cmd, 3, 0)) == "rhi.no_vertex_buffer");
        CHECK_FALSE(dev.cmd_bind_vertex_buffer(cmd, rig.vertices).has_value());
        CHECK(err_code(dev.cmd_draw(cmd, 0, 0)) == "rhi.invalid_argument");
        CHECK_FALSE(dev.cmd_draw(cmd, 3, 0).has_value());

        CHECK_FALSE(dev.cmd_end_render_pass(cmd).has_value());
        CHECK(err_code(dev.cmd_end_render_pass(cmd)) == "rhi.no_pass");
        CHECK_FALSE(dev.cmd_end(cmd).has_value());

        // Ready: recording refuses; exactly one submit consumes; reusable.
        CHECK(err_code(dev.cmd_draw(cmd, 3, 0)) == "rhi.not_recording");
        CHECK_FALSE(dev.submit_and_wait(cmd).has_value());
        CHECK(err_code(dev.submit_and_wait(cmd)) == "rhi.not_ready");
        CHECK_FALSE(dev.cmd_begin(cmd).has_value());
        CHECK_FALSE(dev.cmd_begin_render_pass(cmd, pass).has_value());
        CHECK_FALSE(dev.cmd_end_render_pass(cmd).has_value());
        CHECK_FALSE(dev.cmd_end(cmd).has_value());
        CHECK_FALSE(dev.submit_and_wait(cmd).has_value());
    });
}

TEST_CASE("rhi.conformance.textured_pipeline_requires_texture") {
    for_each_backend(
        "rhi.conformance.textured_pipeline_requires_texture", [](RhiBackend&, RhiDevice& dev) {
            ProtocolRig rig(dev);
            rig.build(/*textured=*/true);

            REQUIRE_FALSE(dev.cmd_begin(rig.list).has_value());
            REQUIRE_FALSE(dev.cmd_begin_render_pass(
                                 rig.list, {.color_target = rig.target, .clear = kClearColor})
                              .has_value());
            REQUIRE_FALSE(dev.cmd_bind_pipeline(rig.list, rig.pipeline).has_value());
            REQUIRE_FALSE(dev.cmd_bind_vertex_buffer(rig.list, rig.vertices).has_value());
            CHECK(err_code(dev.cmd_draw(rig.list, 3, 0)) == "rhi.texture_missing");
            CHECK(err_code(dev.cmd_bind_texture(rig.list, 1, rig.sampled, rig.sampler)) ==
                  "rhi.unsupported"); // M0 binding model: slot 0 only
            CHECK_FALSE(dev.cmd_bind_texture(rig.list, 0, rig.sampled, rig.sampler).has_value());
            CHECK_FALSE(dev.cmd_draw(rig.list, 3, 0).has_value());
            REQUIRE_FALSE(dev.cmd_end_render_pass(rig.list).has_value());
            REQUIRE_FALSE(dev.cmd_end(rig.list).has_value());
            CHECK_FALSE(dev.submit_and_wait(rig.list).has_value());
        });
}

TEST_CASE("rhi.conformance.pipelines_snapshot_their_shaders") {
    for_each_backend(
        "rhi.conformance.pipelines_snapshot_their_shaders", [](RhiBackend&, RhiDevice& dev) {
            ProtocolRig rig(dev);
            rig.build(/*textured=*/false);

            // Destroying the stages after create_pipeline is legal
            // (device.h): the pipeline snapshotted them; a full record ->
            // submit still works.
            CHECK_FALSE(dev.destroy_shader(rig.vert).has_value());
            CHECK_FALSE(dev.destroy_shader(rig.frag).has_value());
            rig.vert = ShaderHandle{};
            rig.frag = ShaderHandle{};

            REQUIRE_FALSE(dev.cmd_begin(rig.list).has_value());
            REQUIRE_FALSE(dev.cmd_begin_render_pass(
                                 rig.list, {.color_target = rig.target, .clear = kClearColor})
                              .has_value());
            CHECK_FALSE(dev.cmd_bind_pipeline(rig.list, rig.pipeline).has_value());
            CHECK_FALSE(dev.cmd_bind_vertex_buffer(rig.list, rig.vertices).has_value());
            CHECK_FALSE(dev.cmd_draw(rig.list, 3, 0).has_value());
            REQUIRE_FALSE(dev.cmd_end_render_pass(rig.list).has_value());
            REQUIRE_FALSE(dev.cmd_end(rig.list).has_value());
            CHECK_FALSE(dev.submit_and_wait(rig.list).has_value());
        });
}

TEST_CASE("rhi.conformance.clear_scene_is_exact_on_every_pixel") {
    for_each_backend("rhi.conformance.clear_scene_is_exact_on_every_pixel",
                     [](RhiBackend&, RhiDevice& device) {
                         SceneRender render = render_scene(device, SceneId::kClear);
                         REQUIRE(render.ok());
                         REQUIRE(render.image.width == kSceneExtent);
                         REQUIRE(render.image.height == kSceneExtent);
                         // The pinned clear color has no UNORM rounding ties (D-BUILD-
                         // 090): every pixel is exactly (51,102,153,255) on every backend.
                         std::size_t mismatches = 0;
                         for (std::uint32_t y = 0; y < render.image.height; ++y)
                             for (std::uint32_t x = 0; x < render.image.width; ++x)
                                 if (render.image.at(x, y) != kClearRgba)
                                     ++mismatches;
                         CHECK(mismatches == 0);
                     });
}

TEST_CASE("rhi.conformance.triangle_scene_pixel_truths") {
    for_each_backend("rhi.conformance.triangle_scene_pixel_truths",
                     [](RhiBackend& backend, RhiDevice& device) {
                         SceneRender render = render_scene(device, SceneId::kTriangle);
                         REQUIRE(render.ok());
                         const ImageRgba8& img = render.image;
                         // Corners lie outside the triangle on ANY backend: exact background.
                         CHECK(img.at(2, 2) == kClearRgba);
                         CHECK(img.at(kSceneExtent - 3, 2) == kClearRgba);
                         CHECK(img.at(2, kSceneExtent - 3) == kClearRgba);
                         CHECK(img.at(kSceneExtent - 3, kSceneExtent - 3) == kClearRgba);
                         if (backend.rasterizes) {
                             // Vertex-color corners, CPU-oracle probes (y-DOWN seam NDC):
                             // apex (0,-0.6) -> row ~51 red; (0.6,0.6) green; (-0.6,0.6) blue.
                             CHECK(channel_dominant(img.at(128, 70), 0));
                             CHECK(channel_dominant(img.at(195, 195), 1));
                             CHECK(channel_dominant(img.at(61, 195), 2));
                             CHECK_FALSE(img.at(128, 128) == kClearRgba);
                         } else {
                             // A seam double invents no pixels: the draw is a validated
                             // no-op and the interior stays background (D-BUILD-092).
                             CHECK(img.at(128, 128) == kClearRgba);
                         }
                     });
}

TEST_CASE("rhi.conformance.textured_quad_samples_exact_texels") {
    for_each_backend("rhi.conformance.textured_quad_samples_exact_texels",
                     [](RhiBackend& backend, RhiDevice& device) {
                         SceneRender render = render_scene(device, SceneId::kTexturedQuad);
                         REQUIRE(render.ok());
                         const ImageRgba8& img = render.image;
                         // Outside the quad ([-0.75,0.75] -> pixels [32,224)): background.
                         CHECK(img.at(10, 128) == kClearRgba);
                         CHECK(img.at(128, 10) == kClearRgba);
                         CHECK(img.at(245, 128) == kClearRgba);
                         if (backend.rasterizes) {
                             // Checker cell centers land on texel centers (24px per
                             // cell): nearest sampling returns the exact texel bytes on
                             // any conformant driver — and an RGBA/BGRA swap anywhere in
                             // a backend breaks these channel-asymmetric pins hard.
                             CHECK(img.at(44, 44) == kCheckerColorA);
                             CHECK(img.at(68, 44) == kCheckerColorB);
                             CHECK(img.at(44, 68) == kCheckerColorB);
                             CHECK(img.at(68, 68) == kCheckerColorA);
                             CHECK(img.at(44 + 24 * 7, 44 + 24 * 7) == kCheckerColorA);
                         } else {
                             CHECK(img.at(128, 128) == kClearRgba);
                         }
                     });
}

TEST_CASE("rhi.conformance.dual_render_hashes_identical") {
    for_each_backend("rhi.conformance.dual_render_hashes_identical",
                     [](RhiBackend&, RhiDevice& device) {
                         // Two INDEPENDENT renders (fresh resources each) diffed — same
                         // device, same bytes. Cross-device identity is deliberately NOT
                         // claimed here (two-tier semantics; the cross-backend compare is
                         // tier 2, `midday shot compare`).
                         SceneRender first = render_scene(device, SceneId::kTriangle);
                         SceneRender second = render_scene(device, SceneId::kTriangle);
                         REQUIRE(first.ok());
                         REQUIRE(second.ok());
                         CHECK(pixel_hash(first.image) == pixel_hash(second.image));
                     });
}

TEST_CASE("rhi.conformance.glsl_refusals_are_structured") {
    for_each_backend("rhi.conformance.glsl_refusals_are_structured",
                     [](RhiBackend& backend, RhiDevice& device) {
                         if (!backend.compiles_glsl)
                             return; // NullDevice stores source without compiling (by design)
                         ShaderResult bad = device.create_shader({.stage = ShaderStage::kVertex,
                                                                  .glsl = "#version 450\nbroken",
                                                                  .debug_name = "bad"});
                         REQUIRE_FALSE(bad.ok());
                         CHECK(err_code(bad.error) == "rhi.shader_compile");
                     });
}

// Keep LAST in this file: accounts for every skip loudly. Skips are a
// per-host fact on ICD-less Linux dev machines; they are FORBIDDEN for
// Metal on macOS (native Metal is the platform's point) and forbidden for
// Vulkan in the golden-software lane (lavapipe guaranteed).
TEST_CASE("rhi.conformance.zz_backend_report") {
    for (RhiBackend& backend : testkit::rhi_backends()) {
        INFO("backend: " << backend.name);
        if (backend.result.ok()) {
            MESSAGE(backend.name << " corpus ran against: "
                                 << backend.result.device->caps().device_name);
            CHECK(backend.skips == 0);
            continue;
        }
        const std::string reason =
            backend.result.error ? backend.result.error->message : std::string("unknown");
        std::fprintf(stderr,
                     "rhi.conformance: %d case(s) SKIPPED on backend %s — %s\n",
                     backend.skips,
                     backend.name.c_str(),
                     reason.c_str());
        MESSAGE(backend.name << " skipped " << backend.skips << " case(s): " << reason);
#if defined(__APPLE__)
        // macOS MUST bring up native Metal — a Mac without a Metal device is
        // a broken assumption worth a red build, not a skip.
        CHECK_MESSAGE(backend.name != "metal", "Metal must be available on macOS");
#endif
    }
}

} // namespace

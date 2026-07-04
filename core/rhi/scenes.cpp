// core/rhi/scenes.cpp — the three pinned M0 scenes over the RhiDevice seam
// (scenes.h). GLSL lives here as source: the seam compiles it per backend
// (glslang -> SPIR-V on Vulkan; MSL translation at m0-rhi-metal).

#include "core/rhi/scenes.h"

#include <cstddef>
#include <span>
#include <string>
#include <utility>

namespace midday::rhi {

namespace {

constexpr std::string_view kColorVert = R"(#version 450
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec3 a_color;
layout(location = 0) out vec3 v_color;
void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_color = a_color;
}
)";

constexpr std::string_view kColorFrag = R"(#version 450
layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(v_color, 1.0);
}
)";

constexpr std::string_view kTexturedVert = R"(#version 450
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_uv = a_uv;
}
)";

constexpr std::string_view kTexturedFrag = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = texture(u_texture, v_uv);
}
)";

// Everything a scene creates, destroyed in reverse-creation order on every
// exit path. Handles start null; destroy skips nulls.
struct SceneResources {
    RhiDevice& device;
    TextureHandle target;
    CommandListHandle list;
    ShaderHandle vert;
    ShaderHandle frag;
    PipelineHandle pipeline;
    BufferHandle vertices;
    TextureHandle texture;
    SamplerHandle sampler;

    explicit SceneResources(RhiDevice& dev) : device(dev) {}

    SceneResources(const SceneResources&) = delete;
    SceneResources& operator=(const SceneResources&) = delete;
    SceneResources(SceneResources&&) = delete;
    SceneResources& operator=(SceneResources&&) = delete;

    ~SceneResources() {
        // Reverse creation order; errors here would mean seam bookkeeping
        // corruption, which the selftests catch — destruction stays quiet.
        if (!sampler.is_null())
            (void)device.destroy_sampler(sampler);
        if (!texture.is_null())
            (void)device.destroy_texture(texture);
        if (!vertices.is_null())
            (void)device.destroy_buffer(vertices);
        if (!pipeline.is_null())
            (void)device.destroy_pipeline(pipeline);
        if (!frag.is_null())
            (void)device.destroy_shader(frag);
        if (!vert.is_null())
            (void)device.destroy_shader(vert);
        if (!list.is_null())
            (void)device.destroy_command_list(list);
        if (!target.is_null())
            (void)device.destroy_texture(target);
    }
};

SceneRender fail(base::Error error) {
    return SceneRender{.error = std::move(error)};
}

std::optional<base::Error> build_geometry(SceneResources& res, SceneId scene) {
    const bool textured = scene == SceneId::kTexturedQuad;

    ShaderResult vert =
        res.device.create_shader({.stage = ShaderStage::kVertex,
                                  .glsl = std::string(textured ? kTexturedVert : kColorVert),
                                  .debug_name = textured ? "m0.textured.vert" : "m0.color.vert"});
    if (!vert.ok())
        return vert.error;
    res.vert = vert.handle;

    ShaderResult frag =
        res.device.create_shader({.stage = ShaderStage::kFragment,
                                  .glsl = std::string(textured ? kTexturedFrag : kColorFrag),
                                  .debug_name = textured ? "m0.textured.frag" : "m0.color.frag"});
    if (!frag.ok())
        return frag.error;
    res.frag = frag.handle;

    PipelineDesc pipeline_desc;
    pipeline_desc.vertex_shader = res.vert;
    pipeline_desc.fragment_shader = res.frag;
    pipeline_desc.uses_texture = textured;
    pipeline_desc.debug_name = textured ? "m0.textured_quad" : "m0.triangle";
    if (textured) {
        pipeline_desc.vertex_layout = {
            .stride_bytes = 4 * sizeof(float),
            .attributes = {{.location = 0, .format = VertexFormat::kFloat2, .offset_bytes = 0},
                           {.location = 1,
                            .format = VertexFormat::kFloat2,
                            .offset_bytes = 2 * sizeof(float)}}};
    } else {
        pipeline_desc.vertex_layout = {
            .stride_bytes = 5 * sizeof(float),
            .attributes = {{.location = 0, .format = VertexFormat::kFloat2, .offset_bytes = 0},
                           {.location = 1,
                            .format = VertexFormat::kFloat3,
                            .offset_bytes = 2 * sizeof(float)}}};
    }
    PipelineResult pipeline = res.device.create_pipeline(pipeline_desc);
    if (!pipeline.ok())
        return pipeline.error;
    res.pipeline = pipeline.handle;

    const std::span<const float> vertex_data = textured ? std::span<const float>(kQuadVertices)
                                                        : std::span<const float>(kTriangleVertices);
    BufferResult buffer = res.device.create_buffer(
        {.size_bytes = vertex_data.size_bytes(),
         .usage = BufferUsage::kVertex,
         .debug_name = textured ? "m0.quad.vertices" : "m0.triangle.vertices"},
        std::as_bytes(vertex_data));
    if (!buffer.ok())
        return buffer.error;
    res.vertices = buffer.handle;

    if (textured) {
        const ImageRgba8 checker = checkerboard_image();
        TextureResult texture = res.device.create_texture({.width = checker.width,
                                                           .height = checker.height,
                                                           .format = TextureFormat::kRGBA8Unorm,
                                                           .usage = TextureUsage::kSampled,
                                                           .debug_name = "m0.checkerboard"},
                                                          std::as_bytes(std::span(checker.pixels)));
        if (!texture.ok())
            return texture.error;
        res.texture = texture.handle;

        SamplerResult sampler = res.device.create_sampler({.filter = FilterMode::kNearest,
                                                           .address = AddressMode::kClampToEdge,
                                                           .debug_name = "m0.nearest"});
        if (!sampler.ok())
            return sampler.error;
        res.sampler = sampler.handle;
    }
    return std::nullopt;
}

std::optional<base::Error>
record_and_submit(SceneResources& res, SceneId scene, std::uint32_t vertex_count) {
    RhiDevice& dev = res.device;
    if (auto error = dev.cmd_begin(res.list))
        return error;
    if (auto error =
            dev.cmd_begin_render_pass(res.list, {.color_target = res.target, .clear = kClearColor}))
        return error;
    if (scene != SceneId::kClear) {
        if (auto error = dev.cmd_bind_pipeline(res.list, res.pipeline))
            return error;
        if (auto error = dev.cmd_bind_vertex_buffer(res.list, res.vertices))
            return error;
        if (scene == SceneId::kTexturedQuad)
            if (auto error = dev.cmd_bind_texture(res.list, 0, res.texture, res.sampler))
                return error;
        if (auto error = dev.cmd_draw(res.list, vertex_count, 0))
            return error;
    }
    if (auto error = dev.cmd_end_render_pass(res.list))
        return error;
    if (auto error = dev.cmd_end(res.list))
        return error;
    return dev.submit_and_wait(res.list);
}

} // namespace

std::string_view to_string(SceneId scene) {
    switch (scene) {
    case SceneId::kClear:
        return "clear";
    case SceneId::kTriangle:
        return "triangle";
    case SceneId::kTexturedQuad:
        return "textured_quad";
    }
    return "unknown";
}

std::optional<SceneId> scene_from_name(std::string_view name) {
    for (SceneId scene : kAllScenes)
        if (to_string(scene) == name)
            return scene;
    return std::nullopt;
}

ImageRgba8 checkerboard_image() {
    ImageRgba8 image{.width = kCheckerTextureExtent, .height = kCheckerTextureExtent, .pixels = {}};
    image.pixels.resize(image.byte_size());
    constexpr std::uint32_t kCellExtent = kCheckerTextureExtent / kCheckerCellsPerSide;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const bool a = ((x / kCellExtent) + (y / kCellExtent)) % 2 == 0;
            const std::array<std::uint8_t, 4>& color = a ? kCheckerColorA : kCheckerColorB;
            const std::size_t offset = (std::size_t{y} * image.width + x) * 4;
            image.pixels[offset] = color[0];
            image.pixels[offset + 1] = color[1];
            image.pixels[offset + 2] = color[2];
            image.pixels[offset + 3] = color[3];
        }
    }
    return image;
}

SceneRender render_scene(RhiDevice& device, SceneId scene) {
    SceneResources res(device);

    TextureResult target = device.create_texture({.width = kSceneExtent,
                                                  .height = kSceneExtent,
                                                  .format = TextureFormat::kRGBA8Unorm,
                                                  .usage = TextureUsage::kRenderTarget,
                                                  .debug_name = "m0.target"},
                                                 {});
    if (target.error.has_value())
        return fail(std::move(*target.error));
    res.target = target.handle;

    CommandListResult list = device.create_command_list();
    if (list.error.has_value())
        return fail(std::move(*list.error));
    res.list = list.handle;

    std::uint32_t vertex_count = 0;
    if (scene != SceneId::kClear) {
        if (auto error = build_geometry(res, scene))
            return fail(std::move(*error));
        vertex_count = scene == SceneId::kTriangle ? 3 : 6;
    }

    if (auto error = record_and_submit(res, scene, vertex_count))
        return fail(std::move(*error));

    ImageRgba8 image{.width = kSceneExtent, .height = kSceneExtent, .pixels = {}};
    image.pixels.resize(image.byte_size());
    if (auto error =
            device.read_texture(res.target, std::as_writable_bytes(std::span(image.pixels))))
        return fail(std::move(*error));
    return SceneRender{.image = std::move(image)};
}

} // namespace midday::rhi

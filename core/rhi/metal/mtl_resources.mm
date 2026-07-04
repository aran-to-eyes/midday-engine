// core/rhi/metal/mtl_resources.mm — buffers, samplers, shaders, pipelines
// (m0-rhi-metal). Descriptor validation reuses the seam's shared code so
// Metal refuses byte-identically to NullDevice and Vulkan; shaders travel
// GLSL -> SPIR-V -> MSL through the one shadercomp seam.

#include "core/rhi/metal/mtl_internal.h"
#include "core/rhi/shadercomp/shader_compiler.h"
#include "core/rhi/validate.h"

#include <cstring>
#include <utility>

namespace midday::rhi::mtl {

namespace {

MTLSamplerMinMagFilter to_mtl(FilterMode filter) {
    return filter == FilterMode::kNearest ? MTLSamplerMinMagFilterNearest
                                          : MTLSamplerMinMagFilterLinear;
}

MTLSamplerAddressMode to_mtl(AddressMode address) {
    return address == AddressMode::kClampToEdge ? MTLSamplerAddressModeClampToEdge
                                                : MTLSamplerAddressModeRepeat;
}

MTLVertexFormat to_mtl(VertexFormat format) {
    switch (format) {
    case VertexFormat::kFloat2:
        return MTLVertexFormatFloat2;
    case VertexFormat::kFloat3:
        return MTLVertexFormatFloat3;
    case VertexFormat::kFloat4:
        return MTLVertexFormatFloat4;
    }
    return MTLVertexFormatInvalid;
}

} // namespace

BufferResult MetalDevice::create_buffer(const BufferDesc& desc,
                                        std::span<const std::byte> initial_data) {
    if (auto error = validate_buffer_desc(desc))
        return {.error = std::move(error)};
    if (auto error = validate_initial_data(
            initial_data.size(), static_cast<std::size_t>(desc.size_bytes), desc.debug_name))
        return {.error = std::move(error)};

    // M0 vertex data is tiny and write-once: shared storage IS device memory
    // on Apple-silicon unified memory (the host-visible-mapped choice the
    // Vulkan backend makes for the same reason).
    BufferEntry entry{.desc = desc};
    entry.buffer = [device_ newBufferWithLength:desc.size_bytes
                                        options:MTLResourceStorageModeShared];
    if (entry.buffer == nil)
        return {.error = mtl_fault("MTLDevice newBufferWithLength", nil)};
    entry.buffer.label = [NSString stringWithUTF8String:desc.debug_name.c_str()];
    if (!initial_data.empty())
        std::memcpy(entry.buffer.contents, initial_data.data(), initial_data.size());
    return {.handle = buffers_.create(std::move(entry))};
}

SamplerResult MetalDevice::create_sampler(const SamplerDesc& desc) {
    MTLSamplerDescriptor* sampler_desc = [MTLSamplerDescriptor new];
    sampler_desc.minFilter = to_mtl(desc.filter);
    sampler_desc.magFilter = to_mtl(desc.filter);
    sampler_desc.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sampler_desc.sAddressMode = to_mtl(desc.address);
    sampler_desc.tAddressMode = to_mtl(desc.address);
    sampler_desc.rAddressMode = to_mtl(desc.address);
    SamplerEntry entry{.desc = desc};
    entry.sampler = [device_ newSamplerStateWithDescriptor:sampler_desc];
    if (entry.sampler == nil)
        return {.error = mtl_fault("MTLDevice newSamplerStateWithDescriptor", nil)};
    return {.handle = samplers_.create(std::move(entry))};
}

ShaderResult MetalDevice::create_shader(const ShaderDesc& desc) {
    // One front end, two outputs (shadercomp): the same GLSL the Vulkan
    // backend compiles, translated to MSL with the vertex-stage y flip that
    // implements the pinned coordinate contract.
    shadercomp::SpirvResult spirv =
        shadercomp::compile_glsl(desc.stage, desc.glsl, desc.entry_point, desc.debug_name);
    if (!spirv.ok())
        return {.error = std::move(spirv.error)};
    shadercomp::MslResult msl =
        shadercomp::msl_from_spirv(desc.stage, spirv.words, desc.debug_name);
    if (!msl.ok())
        return {.error = std::move(msl.error)};

    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_2; // matches shadercomp's target
    NSError* ns_error = nil;
    id<MTLLibrary> library =
        [device_ newLibraryWithSource:[NSString stringWithUTF8String:msl.source.c_str()]
                              options:options
                                error:&ns_error];
    if (library == nil)
        return {.error = mtl_fault("MTLDevice newLibraryWithSource", ns_error)};
    ShaderEntry entry{.stage = desc.stage};
    entry.function =
        [library newFunctionWithName:[NSString stringWithUTF8String:msl.entry_point.c_str()]];
    if (entry.function == nil)
        return {.error = mtl_fault("MTLLibrary newFunctionWithName", nil)};
    return {.handle = shaders_.create(std::move(entry))};
}

PipelineResult MetalDevice::create_pipeline(const PipelineDesc& desc) {
    std::optional<base::Error> error;
    ShaderEntry* vert = lookup_handle(shaders_, desc.vertex_shader, "vertex shader", error);
    if (vert == nullptr)
        return {.error = std::move(error)};
    ShaderEntry* frag = lookup_handle(shaders_, desc.fragment_shader, "fragment shader", error);
    if (frag == nullptr)
        return {.error = std::move(error)};

    MTLRenderPipelineDescriptor* pipeline_desc = [MTLRenderPipelineDescriptor new];
    pipeline_desc.label = [NSString stringWithUTF8String:desc.debug_name.c_str()];
    pipeline_desc.vertexFunction = vert->function;   // the descriptor retains the
    pipeline_desc.fragmentFunction = frag->function; // functions: stages snapshot
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    // Pinned M0 fixed state (types.h): no depth, no blend — descriptor
    // defaults already say exactly that; cull/winding are encoder state and
    // Metal's defaults (cull none) match the pin.

    if (!desc.vertex_layout.attributes.empty()) {
        // One interleaved vertex buffer at Metal buffer index 0 — free in
        // the vertex stage: the M0 binding model has no other vertex-stage
        // buffers, and SPIRV-Cross routes attributes through stage_in.
        MTLVertexDescriptor* vertex_desc = [MTLVertexDescriptor vertexDescriptor];
        for (const VertexAttribute& attribute : desc.vertex_layout.attributes) {
            vertex_desc.attributes[attribute.location].format = to_mtl(attribute.format);
            vertex_desc.attributes[attribute.location].offset = attribute.offset_bytes;
            vertex_desc.attributes[attribute.location].bufferIndex = 0;
        }
        vertex_desc.layouts[0].stride = desc.vertex_layout.stride_bytes;
        vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        pipeline_desc.vertexDescriptor = vertex_desc;
    }

    NSError* ns_error = nil;
    PipelineEntry entry{.uses_texture = desc.uses_texture};
    entry.pipeline = [device_ newRenderPipelineStateWithDescriptor:pipeline_desc error:&ns_error];
    if (entry.pipeline == nil)
        return {.error = mtl_fault("MTLDevice newRenderPipelineStateWithDescriptor", ns_error)};
    return {.handle = pipelines_.create(std::move(entry))};
}

// Synchronous model: every submit waited, so nothing is in flight and ARC
// releases the Metal objects with the recycled slot (release_handle).

std::optional<base::Error> MetalDevice::destroy_buffer(BufferHandle handle) {
    return release_handle(buffers_, handle, "buffer");
}

std::optional<base::Error> MetalDevice::destroy_texture(TextureHandle handle) {
    return release_handle(textures_, handle, "texture");
}

std::optional<base::Error> MetalDevice::destroy_sampler(SamplerHandle handle) {
    return release_handle(samplers_, handle, "sampler");
}

std::optional<base::Error> MetalDevice::destroy_shader(ShaderHandle handle) {
    return release_handle(shaders_, handle, "shader");
}

std::optional<base::Error> MetalDevice::destroy_pipeline(PipelineHandle handle) {
    return release_handle(pipelines_, handle, "pipeline");
}

} // namespace midday::rhi::mtl

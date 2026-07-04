// core/rhi/metal/mtl_textures.mm — texture creation, staged upload,
// zero-fill, readback (m0-rhi-metal). Textures are MTLStorageModePrivate
// with blit-staged transfer both ways: one shape that is correct on unified
// AND discrete memory (the Vulkan backend's staging choreography, minus the
// layout transitions Metal tracks itself).
//
// Coordinate contract (types.h): Metal's texture row 0 IS the top image row
// and blit copies are row-order preserving, so upload takes the seam's
// top-to-bottom rows verbatim and readback returns them verbatim — no flip
// exists anywhere in this file, by design (the one adaptation is the vertex
// -stage clip flip in shadercomp).

#include "core/rhi/metal/mtl_internal.h"
#include "core/rhi/validate.h"

#include <cstring>
#include <utility>

namespace midday::rhi::mtl {

TextureResult MetalDevice::create_texture(const TextureDesc& desc,
                                          std::span<const std::byte> initial_data) {
    if (auto error = validate_texture_desc(desc, caps_.max_texture_size))
        return {.error = std::move(error)};
    const std::size_t byte_size =
        std::size_t{desc.width} * desc.height * bytes_per_pixel(desc.format);
    if (auto error = validate_initial_data(initial_data.size(), byte_size, desc.debug_name))
        return {.error = std::move(error)};

    const bool render_target = desc.usage == TextureUsage::kRenderTarget;
    MTLTextureDescriptor* texture_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:desc.width
                                                          height:desc.height
                                                       mipmapped:NO];
    texture_desc.storageMode = MTLStorageModePrivate;
    texture_desc.usage = render_target ? MTLTextureUsageRenderTarget : MTLTextureUsageShaderRead;

    TextureEntry entry{.desc = desc};
    entry.texture = [device_ newTextureWithDescriptor:texture_desc];
    if (entry.texture == nil)
        return {.error = mtl_fault("MTLDevice newTextureWithDescriptor", nil)};
    entry.texture.label = [NSString stringWithUTF8String:desc.debug_name.c_str()];

    // Content policy mirrors NullDevice and Vulkan: initial data uploads;
    // absence means DEFINED zero content for sampled textures, while render
    // targets stay undefined until their first pass clears them.
    if (!initial_data.empty() || !render_target) {
        if (auto error = upload_texture(entry, initial_data))
            return {.error = std::move(error)};
    }
    return {.handle = textures_.create(std::move(entry))};
}

std::optional<base::Error> MetalDevice::upload_texture(TextureEntry& entry,
                                                       std::span<const std::byte> data) {
    const std::size_t byte_size =
        std::size_t{entry.desc.width} * entry.desc.height * bytes_per_pixel(entry.desc.format);
    // newBufferWithLength allocates zero-filled shared memory: the empty-
    // span call IS the zero-fill path.
    id<MTLBuffer> staging = [device_ newBufferWithLength:byte_size
                                                 options:MTLResourceStorageModeShared];
    if (staging == nil)
        return mtl_fault("MTLDevice newBufferWithLength (staging)", nil);
    if (!data.empty())
        std::memcpy(staging.contents, data.data(), data.size());

    auto error = with_transient_blit([&](id<MTLBlitCommandEncoder> blit) {
        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:std::size_t{entry.desc.width} * bytes_per_pixel(entry.desc.format)
            sourceBytesPerImage:byte_size
                     sourceSize:MTLSizeMake(entry.desc.width, entry.desc.height, 1)
                      toTexture:entry.texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    });
    if (!error)
        entry.contents_defined = true;
    return error;
}

std::optional<base::Error> MetalDevice::read_texture(TextureHandle texture,
                                                     std::span<std::byte> out) {
    std::optional<base::Error> error;
    TextureEntry* entry = lookup_handle(textures_, texture, "texture", error);
    if (entry == nullptr)
        return error;
    const std::size_t expected =
        std::size_t{entry->desc.width} * entry->desc.height * bytes_per_pixel(entry->desc.format);
    if (auto size_error = validate_readback_size(out.size(), expected))
        return size_error;
    if (!entry->contents_defined)
        return base::Error{.code = "rhi.invalid_argument",
                           .message = "texture has no defined contents yet (render or upload "
                                      "before reading '" +
                                      entry->desc.debug_name + "')"};

    id<MTLBuffer> readback = [device_ newBufferWithLength:expected
                                                  options:MTLResourceStorageModeShared];
    if (readback == nil)
        return mtl_fault("MTLDevice newBufferWithLength (readback)", nil);
    auto copy_error = with_transient_blit([&](id<MTLBlitCommandEncoder> blit) {
        [blit copyFromTexture:entry->texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(entry->desc.width, entry->desc.height, 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:std::size_t{entry->desc.width} *
                                     bytes_per_pixel(entry->desc.format)
            destinationBytesPerImage:expected];
    });
    if (copy_error)
        return copy_error;
    std::memcpy(out.data(), readback.contents, expected);
    return std::nullopt;
}

} // namespace midday::rhi::mtl

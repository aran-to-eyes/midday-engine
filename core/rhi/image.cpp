// core/rhi/image.cpp — decoded-pixel hashing + PNG transport (image.h).

#include "core/rhi/image.h"

#define XXH_INLINE_ALL
#include <cassert>
#include <cstring>
#include <stb_image_write.h>
#include <xxhash.h>

namespace midday::rhi {

std::array<std::uint8_t, 4> ImageRgba8::at(std::uint32_t x, std::uint32_t y) const {
    assert(x < width && y < height);
    const std::size_t offset = (std::size_t{y} * width + x) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

std::uint64_t pixel_hash(const ImageRgba8& image) {
    // Streaming state so the header and the pixel bytes hash as ONE domain
    // without copying the image. Header bytes are little-endian u32s,
    // spelled explicitly — never memcpy'd from host integers.
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    const std::uint8_t header[8] = {static_cast<std::uint8_t>(image.width),
                                    static_cast<std::uint8_t>(image.width >> 8u),
                                    static_cast<std::uint8_t>(image.width >> 16u),
                                    static_cast<std::uint8_t>(image.width >> 24u),
                                    static_cast<std::uint8_t>(image.height),
                                    static_cast<std::uint8_t>(image.height >> 8u),
                                    static_cast<std::uint8_t>(image.height >> 16u),
                                    static_cast<std::uint8_t>(image.height >> 24u)};
    XXH3_64bits_update(&state, header, sizeof header);
    XXH3_64bits_update(&state, image.pixels.data(), image.pixels.size());
    return XXH3_64bits_digest(&state);
}

std::optional<base::Error> write_png(const ImageRgba8& image, const std::string& path) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() != image.byte_size())
        return base::Error{.code = "rhi.invalid_argument",
                           .message = "cannot encode an empty or inconsistent image"};
    const int ok = stbi_write_png(path.c_str(),
                                  static_cast<int>(image.width),
                                  static_cast<int>(image.height),
                                  4,
                                  image.pixels.data(),
                                  static_cast<int>(image.width) * 4);
    if (ok == 0) {
        base::Error error{.code = "rhi.image_write", .message = "PNG encode/write failed"};
        error.details.set("path", path);
        return error;
    }
    return std::nullopt;
}

} // namespace midday::rhi

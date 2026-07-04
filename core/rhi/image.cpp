// core/rhi/image.cpp — decoded-pixel hashing + PNG transport (image.h).

#include "core/rhi/image.h"

#include "core/base/file_io.h"

#define XXH_INLINE_ALL
#include <cassert>
#include <climits>
#include <cstring>
#include <stb_image.h>
#include <stb_image_write.h>
#include <xxhash.h>

namespace midday::rhi {
namespace {

std::optional<base::Error> encode_png(const ImageRgba8& image, const std::string& path) {
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

base::Error read_error(const std::string& path, const std::string& detail) {
    base::Error error{.code = "rhi.image_read", .message = "cannot decode PNG: " + detail};
    error.details.set("path", path);
    return error;
}

} // namespace

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
    return encode_png(image, path);
}

std::optional<base::Error> write_png_refiltered(const ImageRgba8& image, const std::string& path) {
    // Pinned alternate configuration: force the None row filter and a lower
    // deflate level — legitimate encoder output, different bytes. The stb
    // knobs are globals (single-threaded CLI/test usage); saved + restored
    // so ordinary write_png bytes stay a pure function of the pixels.
    const int saved_filter = stbi_write_force_png_filter;
    const int saved_level = stbi_write_png_compression_level;
    stbi_write_force_png_filter = 0;
    stbi_write_png_compression_level = 3;
    std::optional<base::Error> error = encode_png(image, path);
    stbi_write_force_png_filter = saved_filter;
    stbi_write_png_compression_level = saved_level;
    return error;
}

ImageReadResult read_png(const std::string& path) {
    base::ReadFileResult file = base::read_file(path, "rhi.image_read");
    if (file.error.has_value())
        return {.image = {}, .error = std::move(file.error)};
    if (file.bytes.size() > static_cast<std::size_t>(INT_MAX))
        return {.image = {}, .error = read_error(path, "file exceeds the decoder's size limit")};

    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    stbi_uc* decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file.bytes.data()),
                                             static_cast<int>(file.bytes.size()),
                                             &width,
                                             &height,
                                             &channels_in_file,
                                             4);
    if (decoded == nullptr) {
        const char* reason = stbi_failure_reason();
        return {.image = {},
                .error = read_error(path, reason != nullptr ? reason : "decoder refused")};
    }

    ImageRgba8 image{.width = static_cast<std::uint32_t>(width),
                     .height = static_cast<std::uint32_t>(height),
                     .pixels = {}};
    image.pixels.resize(image.byte_size());
    std::memcpy(image.pixels.data(), decoded, image.pixels.size());
    stbi_image_free(decoded);
    return {.image = std::move(image), .error = std::nullopt};
}

} // namespace midday::rhi

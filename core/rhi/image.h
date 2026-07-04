// core/rhi/image.h — CPU-side RGBA8 images: the readback container, the
// decoded-pixel hash, and PNG transport (write: m0-rhi-vulkan, read:
// m0-golden-compare).
//
// THE HASH RULE (Aurora D-14, binding): golden hashes are computed over the
// DECODED PIXELS — the width/height header and the raw RGBA bytes — never
// over an encoded PNG file. Encoders are free to change their compression;
// pixels are the contract. PNG files exist for humans (visual inspection,
// CI artifacts); pixel_hash() exists for gates.

#pragma once

#include "core/base/error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace midday::rhi {

struct ImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels{}; // RGBA rows, top-to-bottom, tightly packed

    [[nodiscard]] std::size_t byte_size() const { return std::size_t{width} * height * 4; }

    // Pre: x < width && y < height (test/probe convenience).
    [[nodiscard]] std::array<std::uint8_t, 4> at(std::uint32_t x, std::uint32_t y) const;
};

// XXH3-64 over (width LE u32, height LE u32, RGBA bytes) — dimensions are
// part of the decoded identity, so a 2x8 and an 8x2 image with equal bytes
// hash apart. Spelled with base::hex64 wherever it reaches JSON or files.
[[nodiscard]] std::uint64_t pixel_hash(const ImageRgba8& image);

// Writes `image` as a PNG file (stb_image_write). Errors: "rhi.image_write"
// (unwritable path / encoder refusal), "rhi.invalid_argument" (empty image).
std::optional<base::Error> write_png(const ImageRgba8& image, const std::string& path);

// Writes the SAME pixels through a pinned alternate encoder configuration
// (forced row filter + different compression level): different PNG bytes,
// identical decoded pixels. Exists for the golden-compare fixtures, whose
// identical pair proves Aurora D-14 in committed form — tier-1 hashes are a
// property of decoded pixels, never of file bytes. Same errors as write_png.
std::optional<base::Error> write_png_refiltered(const ImageRgba8& image, const std::string& path);

// Reads and decodes a PNG file to RGBA8 (stb_image; 16-bit inputs quantize,
// gray/palette expand — output is always tightly packed RGBA rows). PNG is
// lossless in this direction, so write_png -> read_png round-trips pixels
// exactly and pixel_hash survives the file trip.
struct ImageReadResult {
    ImageRgba8 image;
    std::optional<base::Error> error; // "rhi.image_read": unreadable / undecodable

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

ImageReadResult read_png(const std::string& path);

} // namespace midday::rhi

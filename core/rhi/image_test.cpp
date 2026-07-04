// core/rhi/image_test.cpp — decoded-pixel hashing + PNG transport
// (rhi.image.*). The hash domain is (width, height, RGBA bytes) — NEVER an
// encoded file (Aurora D-14); the KAT pins the domain layout forever.

#include "core/rhi/image.h"
#include "testkit/doctest.h"
#include "testkit/temp_dir.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace midday;
using namespace midday::rhi;

// tidy-clean optional access: the code of an expected error, or a marker
// that fails the string compare when the optional is empty.
std::string err_code(std::optional<midday::base::Error> error) {
    return error.has_value() ? error->code : std::string("(no error)");
}

ImageRgba8 gradient(std::uint32_t width, std::uint32_t height) {
    ImageRgba8 image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(image.byte_size());
    for (std::uint32_t i = 0; i < width * height; ++i) {
        image.pixels[std::size_t{i} * 4 + 0] = static_cast<std::uint8_t>(i);
        image.pixels[std::size_t{i} * 4 + 1] = static_cast<std::uint8_t>(i * 3);
        image.pixels[std::size_t{i} * 4 + 2] = static_cast<std::uint8_t>(255 - i);
        image.pixels[std::size_t{i} * 4 + 3] = 255;
    }
    return image;
}

TEST_CASE("rhi.image.pixel_hash_known_answer") {
    // KAT: pins the hash domain (LE u32 width, LE u32 height, RGBA bytes)
    // and XXH3-64 as the algorithm. If this value ever changes, every
    // committed golden hash silently means something else — hence the pin.
    const ImageRgba8 image = gradient(4, 2);
    CHECK(pixel_hash(image) == UINT64_C(0x8544F1F677981E95));

    // Same bytes, transposed dimensions: the domain includes the header.
    const ImageRgba8 transposed = gradient(2, 4);
    CHECK(transposed.pixels == image.pixels);
    CHECK(pixel_hash(transposed) != pixel_hash(image));

    // One byte flipped, one hash flipped.
    ImageRgba8 tweaked = image;
    tweaked.pixels[5] ^= 1;
    CHECK(pixel_hash(tweaked) != pixel_hash(image));
}

TEST_CASE("rhi.image.at_accessor") {
    const ImageRgba8 image = gradient(3, 2);
    const auto pixel = image.at(1, 1); // linear index 4
    CHECK(pixel[0] == 4);
    CHECK(pixel[1] == 12);
    CHECK(pixel[2] == 251);
    CHECK(pixel[3] == 255);
}

TEST_CASE("rhi.image.write_png") {
    testkit::TempDir dir("rhi-png");
    const ImageRgba8 image = gradient(8, 8);
    const std::string path = dir.file("out.png");
    REQUIRE_FALSE(write_png(image, path).has_value());

    // The file exists and is a PNG (magic); its CONTENT is verified by eyes
    // and lavapipe goldens — the hash contract deliberately ignores it.
    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    unsigned char magic[8] = {};
    const std::size_t got = std::fread(magic, 1, sizeof magic, file);
    std::fclose(file);
    REQUIRE(got == 8);
    CHECK(magic[0] == 0x89);
    CHECK(magic[1] == 'P');
    CHECK(magic[2] == 'N');
    CHECK(magic[3] == 'G');

    // Structured refusals: empty image, unwritable path.
    CHECK(err_code(write_png(ImageRgba8{}, dir.file("empty.png"))) == "rhi.invalid_argument");
    CHECK(err_code(write_png(image, dir.file("no/such/dir/out.png"))) == "rhi.image_write");
}

} // namespace

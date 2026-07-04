// core/rhi/null_device_test.cpp — NullDevice-specific truths (rhi.null.*):
// fresh-device handle determinism (absolute generations, dual-script
// identity) and the exact unorm8/clear execution-model pins. The
// backend-independent seam semantics (protocol, refusal spellings, scene
// truths) moved to the conformance corpus (core/rhi/conformance_test.cpp),
// which drives NullDevice alongside every real backend.

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

} // namespace

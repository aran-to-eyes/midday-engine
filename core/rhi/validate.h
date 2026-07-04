// core/rhi/validate.h — shared descriptor/argument validation for every
// backend (m0-rhi-vulkan). One authority: NullDevice, Vulkan, and Metal
// refuse the same inputs with byte-identical structured errors, because
// they run THIS code — the rhi.null tests prove the wording for all of them
// (command_state.h is the same idea for the protocol).

#pragma once

#include "core/base/error.h"
#include "core/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace midday::rhi {

inline std::optional<base::Error> validate_buffer_desc(const BufferDesc& desc) {
    if (desc.size_bytes == 0)
        return base::Error{.code = "rhi.invalid_argument",
                           .message = "buffer size must be nonzero ('" + desc.debug_name + "')"};
    return std::nullopt;
}

inline std::optional<base::Error> validate_texture_desc(const TextureDesc& desc,
                                                        std::uint32_t max_extent) {
    if (desc.width == 0 || desc.height == 0)
        return base::Error{.code = "rhi.invalid_argument",
                           .message = "texture extent must be nonzero ('" + desc.debug_name + "')"};
    if (desc.width > max_extent || desc.height > max_extent) {
        base::Error error{.code = "rhi.unsupported",
                          .message = "texture extent exceeds max_texture_size ('" +
                                     desc.debug_name + "')"};
        error.details.set("max_texture_size", static_cast<std::int64_t>(max_extent));
        return error;
    }
    return std::nullopt;
}

inline std::optional<base::Error>
validate_initial_data(std::size_t provided, std::size_t expected, const std::string& debug_name) {
    if (provided != 0 && provided != expected) {
        base::Error error{.code = "rhi.size_mismatch",
                          .message = "initial data size does not match resource size ('" +
                                     debug_name + "')"};
        error.details.set("provided", static_cast<std::int64_t>(provided));
        error.details.set("expected", static_cast<std::int64_t>(expected));
        return error;
    }
    return std::nullopt;
}

inline std::optional<base::Error> validate_readback_size(std::size_t provided,
                                                         std::size_t expected) {
    if (provided != expected) {
        base::Error error{.code = "rhi.size_mismatch",
                          .message = "readback span size does not match texture byte size"};
        error.details.set("provided", static_cast<std::int64_t>(provided));
        error.details.set("expected", static_cast<std::int64_t>(expected));
        return error;
    }
    return std::nullopt;
}

// M0 binding model: exactly one combined-image-sampler slot.
inline std::optional<base::Error> validate_texture_slot(std::uint32_t slot) {
    if (slot != 0)
        return base::Error{.code = "rhi.unsupported",
                           .message = "M0 binding model has exactly one texture slot (slot 0)"};
    return std::nullopt;
}

inline std::optional<base::Error> validate_render_target_usage(const TextureDesc& desc) {
    if (desc.usage != TextureUsage::kRenderTarget)
        return base::Error{.code = "rhi.invalid_argument",
                           .message = "render pass target was not created as kRenderTarget ('" +
                                      desc.debug_name + "')"};
    return std::nullopt;
}

inline std::optional<base::Error> validate_draw_count(std::uint32_t vertex_count) {
    if (vertex_count == 0)
        return base::Error{.code = "rhi.invalid_argument",
                           .message = "draw vertex_count must be nonzero"};
    return std::nullopt;
}

} // namespace midday::rhi

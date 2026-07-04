// core/rhi/goldens.h — golden-hash file conventions for the M0 scenes
// (m0-rhi-vulkan; header-only, shared by the rhi.vk selftests and the
// `midday rhi render` verb so the lane and the tests read ONE format).
//
// Layout under testkit/goldens/m0/ (see that directory's README for the
// bootstrap protocol):
//   <scene>.hash     16-digit lowercase hex (base::hex64 of pixel_hash),
//                    one line — the DECODED-pixel hash, never a file hash
//   DRIVER_PIN.txt   the DeviceCaps::driver_info string the hashes were
//                    minted on. Hash-equal comparison is only meaningful
//                    within this pinned driver class (spec section 5);
//                    other drivers assert structure, not hashes.

#pragma once

#include "core/base/file_io.h"
#include "core/rhi/scenes.h"

#include <optional>
#include <string>
#include <string_view>

namespace midday::rhi {

inline std::string trimmed(std::string text) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && is_space(text.back()))
        text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && is_space(text[start]))
        ++start;
    return text.substr(start);
}

// Contents of <dir>/<name>, whitespace-trimmed; nullopt when absent/empty.
inline std::optional<std::string> read_golden_file(std::string_view dir, std::string_view name) {
    base::ReadFileResult read =
        base::read_file(std::string(dir) + "/" + std::string(name), "rhi.golden_io");
    if (read.error.has_value())
        return std::nullopt;
    std::string value = trimmed(std::move(read.bytes));
    if (value.empty())
        return std::nullopt;
    return value;
}

inline std::optional<std::string> read_golden_hash(std::string_view dir, SceneId scene) {
    return read_golden_file(dir, std::string(to_string(scene)) + ".hash");
}

inline std::optional<std::string> read_driver_pin(std::string_view dir) {
    return read_golden_file(dir, "DRIVER_PIN.txt");
}

} // namespace midday::rhi

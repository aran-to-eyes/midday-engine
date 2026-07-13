// core/base/hex.h — the canonical hex spellings. hex64: 16-digit lowercase
// hex for 64-bit values, used everywhere a hash reaches JSON output or CI
// byte-compares (selftest's math_fixture_hash, reflection compat hashes,
// journal content hashes later). bytes_to_hex / hex_to_bytes: lowercase hex
// for byte strings — the journal embedding of canonical payload bytes
// (M2 node 0B). One spelling, tree-wide; parsing is strict (lowercase only,
// even length) so the spelling stays canonical both ways.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midday::base {

inline constexpr char kHexDigits[] = "0123456789abcdef";

inline std::string hex64(std::uint64_t value) {
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHexDigits[value & 0xF];
        value >>= 4;
    }
    return out;
}

// Two lowercase digits per byte; empty input -> "".
inline std::string bytes_to_hex(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        out.push_back(kHexDigits[std::to_integer<unsigned>(b) >> 4]);
        out.push_back(kHexDigits[std::to_integer<unsigned>(b) & 0xF]);
    }
    return out;
}

// Strict inverse of bytes_to_hex: even length, lowercase [0-9a-f] only —
// anything else (uppercase included) -> nullopt. Agent formats are exact.
inline std::optional<std::vector<std::byte>> hex_to_bytes(std::string_view text) {
    if (text.size() % 2 != 0)
        return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = digit(text[i]);
        const int low = digit(text[i + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        out.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return out;
}

} // namespace midday::base

// core/base/hex.h — the canonical hash spelling: 16-digit lowercase hex for
// 64-bit values. Used everywhere a hash reaches JSON output or CI byte-
// compares (selftest's math_fixture_hash, reflection compat hashes, journal
// content hashes later) — one spelling, tree-wide.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace midday::base {

inline std::string hex64(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
        value >>= 4;
    }
    return out;
}

} // namespace midday::base

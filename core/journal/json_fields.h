// core/journal/json_fields.h — internal strict-field extraction helpers shared
// by the header/index/record parsers (bundle.cpp, record.cpp). Journal files
// are parsed the way Error::from_json parses: exact keys, exact types, no
// silent tolerance. Not part of the public journal API.

#pragma once

#include "core/base/json.h"

#include <cstdint>
#include <optional>
#include <string>

namespace midday::journal::detail {

// Non-negative integer field -> uint64; nullopt if absent, non-int, or negative.
inline std::optional<std::uint64_t> get_u64(const base::Json& object, std::string_view key) {
    const base::Json* field = object.find(key);
    if (field == nullptr || !field->is_int() || field->as_int() < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(field->as_int());
}

inline std::optional<bool> get_bool(const base::Json& object, std::string_view key) {
    const base::Json* field = object.find(key);
    if (field == nullptr || !field->is_bool())
        return std::nullopt;
    return field->as_bool();
}

// String field; nullopt if absent or not a string (empty strings are legal —
// callers add their own non-empty constraints).
inline std::optional<std::string> get_string(const base::Json& object, std::string_view key) {
    const base::Json* field = object.find(key);
    if (field == nullptr || !field->is_string())
        return std::nullopt;
    return field->as_string();
}

// Strictness backstop: every present key must be one of the expected keys.
// (Required-key presence is checked by the typed getters above.)
template <std::size_t N>
bool only_keys(const base::Json& object, const std::string_view (&expected)[N]) {
    for (const auto& [key, value] : object.items()) {
        bool known = false;
        for (const std::string_view allowed : expected)
            known = known || key == allowed;
        if (!known)
            return false;
    }
    return true;
}

} // namespace midday::journal::detail

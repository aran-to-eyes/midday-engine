// core/reflect/payload_codec_wire.h — INTERNAL wire vocabulary shared by the
// codec's two TUs (payload_codec_encode.cpp / payload_codec_decode.cpp): the
// pinned type tags, little-endian primitives, UTF-8 strictness, and the one
// diagnostic shape. The public contract lives in payload_codec.h; nothing
// outside the codec may include this header — tags are wire bytes, not API
// (the fatal.h internal-header precedent).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/reflect/type_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::reflect::detail {

// Pinned wire values, deliberately decoupled from TypeKind's enum order: a
// reordered enum must never move journal bytes.
inline constexpr std::uint8_t kTagNull = 0x00;
inline constexpr std::uint8_t kTagBool = 0x01;
inline constexpr std::uint8_t kTagInt = 0x02;
inline constexpr std::uint8_t kTagFloat = 0x03;
inline constexpr std::uint8_t kTagString = 0x04;
inline constexpr std::uint8_t kTagName = 0x05;
inline constexpr std::uint8_t kTagVec2 = 0x06;
inline constexpr std::uint8_t kTagVec3 = 0x07;
inline constexpr std::uint8_t kTagVec4 = 0x08;
inline constexpr std::uint8_t kTagQuat = 0x09;
inline constexpr std::uint8_t kTagColor = 0x0a;
inline constexpr std::uint8_t kTagEntityRef = 0x0b;
inline constexpr std::uint8_t kTagAssetRef = 0x0c;
inline constexpr std::uint8_t kTagArray = 0x0d;
inline constexpr std::uint8_t kTagMap = 0x0e;

inline constexpr std::uint64_t kNegativeZeroBits = 0x8000000000000000ULL;

inline constexpr std::string_view kEncodeCode = "reflect.payload_invalid";
inline constexpr std::string_view kEncodeMessage = "payload does not inhabit the event's schema";
inline constexpr std::string_view kDecodeCode = "reflect.payload_decode";
inline constexpr std::string_view kDecodeMessage = "bytes are not a canonical v1 payload encoding";

// EVERY TypeKind carries a distinct pinned tag — this switch IS the contract.
inline std::uint8_t tag_of(TypeKind kind) {
    switch (kind) {
    case TypeKind::kBool:
        return kTagBool;
    case TypeKind::kInt:
        return kTagInt;
    case TypeKind::kFloat:
        return kTagFloat;
    case TypeKind::kString:
        return kTagString;
    case TypeKind::kName:
        return kTagName;
    case TypeKind::kVec2:
        return kTagVec2;
    case TypeKind::kVec3:
        return kTagVec3;
    case TypeKind::kVec4:
        return kTagVec4;
    case TypeKind::kQuat:
        return kTagQuat;
    case TypeKind::kColor:
        return kTagColor;
    case TypeKind::kEntityRef:
        return kTagEntityRef;
    case TypeKind::kAssetRef:
        return kTagAssetRef;
    case TypeKind::kArray:
        return kTagArray;
    case TypeKind::kMap:
        return kTagMap;
    }
    return kTagNull; // unreachable; kept for MSVC's control-path analysis
}

inline std::size_t vec_arity(TypeKind kind) {
    if (kind == TypeKind::kVec2)
        return 2;
    return kind == TypeKind::kVec3 ? 3 : 4; // vec4 / quat / color
}

inline bool is_text_kind(TypeKind kind) {
    return kind == TypeKind::kString || kind == TypeKind::kName || kind == TypeKind::kAssetRef;
}

inline void put_le(std::vector<std::byte>& out, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
}

inline std::string indexed(const std::string& path, std::size_t index) {
    return path + "[" + std::to_string(index) + "]";
}

// Strict UTF-8 (RFC 3629): rejects overlongs, surrogates, > U+10FFFF. The
// JSON parser enforces the same rule at parse time; the codec re-checks so
// programmatically built payloads (and decoded bytes) meet one contract.
inline bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0)
                low = 0xA0; // overlong 3-byte forms
            if (lead == 0xED)
                high = 0x9F; // UTF-8-encoded surrogates
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0)
                low = 0x90; // overlong 4-byte forms
            if (lead == 0xF4)
                high = 0x8F; // > U+10FFFF
        } else {
            return false; // stray continuation, overlong 2-byte lead, or 0xF5+
        }
        if (text.size() - i < length)
            return false;
        for (std::size_t k = 1; k < length; ++k) {
            const auto byte = static_cast<unsigned char>(text[i + k]);
            const unsigned char floor = k == 1 ? low : 0x80;
            const unsigned char ceiling = k == 1 ? high : 0xBF;
            if (byte < floor || byte > ceiling)
                return false;
        }
        i += length;
    }
    return true;
}

// One diagnostic shape for both directions (the bus.payload_invalid diag
// ethos): {event, reason[, field][, expected]} — empty parts are omitted.
inline base::Error codec_error(std::string_view code,
                               std::string_view message,
                               base::Name event,
                               std::string_view reason,
                               std::string_view field,
                               std::string_view expected) {
    base::Json details = base::Json::object();
    if (!event.empty())
        details.set("event", event.view());
    details.set("reason", reason);
    if (!field.empty())
        details.set("field", field);
    if (!expected.empty())
        details.set("expected", expected);
    return base::Error{std::string(code), std::string(message), std::move(details)};
}

// Shared refusal plumbing for the Encoder/Decoder: stores ONE structured
// error; refuse() reports failure in the bool-chaining style both use.
class ErrorSink {
public:
    ErrorSink(std::string_view code, std::string_view message, base::Name event)
        : code_(code), message_(message), event_(event) {}

    ErrorSink(const ErrorSink&) = default;
    ErrorSink& operator=(const ErrorSink&) = default;
    ErrorSink(ErrorSink&&) = default;
    ErrorSink& operator=(ErrorSink&&) = default;

    std::optional<base::Error> take_error() { return std::move(error_); }

protected:
    ~ErrorSink() = default; // never deleted through this base

    bool
    refuse(std::string_view reason, std::string_view field = {}, std::string_view expected = {}) {
        error_ = codec_error(code_, message_, event_, reason, field, expected);
        return false;
    }

    void set_error(base::Error error) { error_ = std::move(error); }

private:
    std::string_view code_;
    std::string_view message_;
    base::Name event_;
    std::optional<base::Error> error_;
};

} // namespace midday::reflect::detail

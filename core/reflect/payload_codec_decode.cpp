// core/reflect/payload_codec_decode.cpp — canonical payload DECODE (v1).
// Layout contract: payload_codec.h; wire vocabulary: payload_codec_wire.h.
// Decode is the strict inverse: version and compat-hash pinned first, then
// every non-canonical byte spelling refuses (-0/NaN/inf bit patterns,
// unsorted map keys, invalid UTF-8, bitmap padding, trailing bytes) — no two
// distinct byte strings decode to the same projection.

#include "core/base/hex.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/reflect/payload_codec.h"
#include "core/reflect/payload_codec_wire.h"
#include "core/reflect/type_model.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace midday::reflect {
namespace {

using base::Json;

class Decoder : public detail::ErrorSink {
public:
    Decoder(const EventDesc& desc, std::span<const std::byte> bytes)
        : ErrorSink(detail::kDecodeCode, detail::kDecodeMessage, desc.name), desc_(desc),
          bytes_(bytes) {}

    bool run(Json& out);

private:
    bool take_le(std::size_t width, std::uint64_t& out, const std::string& path) {
        if (bytes_.size() - pos_ < width)
            return refuse("truncated", path);
        out = 0;
        for (std::size_t i = 0; i < width; ++i)
            out |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[pos_ + i]))
                   << (8 * i);
        pos_ += width;
        return true;
    }

    [[nodiscard]] bool present(std::size_t bitmap_at, std::size_t bit) const {
        const auto byte = std::to_integer<unsigned>(bytes_[bitmap_at + bit / 8]);
        return ((byte >> (bit % 8)) & 1U) != 0;
    }

    bool take_value(const TypeDesc& type, std::uint8_t tag, Json& out, const std::string& path);
    bool take_float(Json& out, const std::string& path);
    bool take_text(std::string& out, const std::string& path);

    const EventDesc& desc_;
    std::span<const std::byte> bytes_;
    std::size_t pos_ = 0;
};

bool Decoder::run(Json& out) {
    std::uint64_t version = 0;
    if (!take_le(1, version, "codec_version"))
        return false;
    if (version != kPayloadCodecVersion)
        return refuse("version", "codec_version", std::to_string(kPayloadCodecVersion));
    std::uint64_t hash = 0;
    if (!take_le(8, hash, "payload_compat_hash"))
        return false;
    if (hash != desc_.compat_hash) {
        // Event A's bytes under event B's schema: the hash pin refuses before
        // any field is trusted (the wrong-schema decode contract).
        base::Error error = detail::codec_error(detail::kDecodeCode,
                                                detail::kDecodeMessage,
                                                desc_.name,
                                                "schema_mismatch",
                                                "payload_compat_hash",
                                                base::hex64(desc_.compat_hash));
        error.details.set("found", base::hex64(hash));
        set_error(std::move(error));
        return false;
    }
    std::uint64_t count = 0;
    if (!take_le(4, count, "field_count"))
        return false;
    if (count != desc_.payload.size()) // hash-pinned already; defense in depth
        return refuse("field_count", "field_count", std::to_string(desc_.payload.size()));
    const std::size_t bitmap_at = pos_;
    const std::size_t bitmap_bytes = (desc_.payload.size() + 7) / 8;
    if (bytes_.size() - pos_ < bitmap_bytes)
        return refuse("truncated", "presence_bitmap");
    pos_ += bitmap_bytes;
    for (std::size_t bit = desc_.payload.size(); bit < bitmap_bytes * 8; ++bit)
        if (present(bitmap_at, bit)) // padding bits are not spare storage
            return refuse("bitmap_padding", "presence_bitmap");
    out = Json::object();
    for (std::size_t i = 0; i < desc_.payload.size(); ++i) {
        if (!present(bitmap_at, i))
            continue;
        const EventFieldDesc& field = desc_.payload[i];
        const std::string path(field.name.view());
        std::uint64_t tag = 0;
        if (!take_le(1, tag, path))
            return false;
        if (tag == detail::kTagNull) {
            out.set(field.name.view(), Json(nullptr)); // explicit null ≠ absent
            continue;
        }
        Json value;
        if (!take_value(field.type, static_cast<std::uint8_t>(tag), value, path))
            return false;
        out.set(field.name.view(), std::move(value));
    }
    if (pos_ != bytes_.size())
        return refuse("trailing_bytes");
    return true;
}

bool Decoder::take_value(const TypeDesc& type,
                         std::uint8_t tag,
                         Json& out,
                         const std::string& path) {
    if (tag != detail::tag_of(type.kind())) // also refuses null inside composites
        return refuse("tag_mismatch", path, type.canonical());
    switch (type.kind()) {
    case TypeKind::kBool: {
        std::uint64_t value = 0;
        if (!take_le(1, value, path))
            return false;
        if (value > 1)
            return refuse("non_canonical", path);
        out = Json(value == 1);
        return true;
    }
    case TypeKind::kInt: {
        std::uint64_t value = 0;
        if (!take_le(8, value, path))
            return false;
        out = Json(static_cast<std::int64_t>(value));
        return true;
    }
    case TypeKind::kFloat:
        return take_float(out, path);
    case TypeKind::kEntityRef: {
        std::uint64_t value = 0;
        if (!take_le(8, value, path))
            return false;
        // Runtime refs are non-negative int64 (bus contract); a high bit set
        // is no encoding this codec ever wrote.
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return refuse("non_canonical", path);
        out = Json(static_cast<std::int64_t>(value));
        return true;
    }
    case TypeKind::kArray: {
        std::uint64_t count = 0;
        if (!take_le(4, count, path))
            return false;
        Json list = Json::array();
        for (std::size_t i = 0; i < count; ++i) {
            const std::string element_path = detail::indexed(path, i);
            std::uint64_t element_tag = 0;
            if (!take_le(1, element_tag, element_path))
                return false;
            Json element;
            if (!take_value(
                    type.element(), static_cast<std::uint8_t>(element_tag), element, element_path))
                return false;
            list.push(std::move(element));
        }
        out = std::move(list);
        return true;
    }
    case TypeKind::kMap: {
        std::uint64_t count = 0;
        if (!take_le(4, count, path))
            return false;
        Json object = Json::object();
        std::string previous;
        for (std::size_t i = 0; i < count; ++i) {
            std::string key;
            if (!take_text(key, path))
                return false;
            std::string entry_path = path;
            entry_path += '.';
            entry_path += key;
            if (i > 0 && previous >= key) // strictly ascending: sorted, no dupes
                return refuse("map_key_order", entry_path);
            std::uint64_t entry_tag = 0;
            if (!take_le(1, entry_tag, entry_path))
                return false;
            Json entry;
            if (!take_value(
                    type.element(), static_cast<std::uint8_t>(entry_tag), entry, entry_path))
                return false;
            object.set(key, std::move(entry));
            previous = std::move(key);
        }
        out = std::move(object);
        return true;
    }
    default:
        break;
    }
    if (detail::is_text_kind(type.kind())) {
        std::string text;
        if (!take_text(text, path))
            return false;
        out = Json(std::move(text));
        return true;
    }
    // vec2/vec3/vec4/quat/color
    Json list = Json::array();
    for (std::size_t i = 0; i < detail::vec_arity(type.kind()); ++i) {
        Json part;
        if (!take_float(part, detail::indexed(path, i)))
            return false;
        list.push(std::move(part));
    }
    out = std::move(list);
    return true;
}

bool Decoder::take_float(Json& out, const std::string& path) {
    std::uint64_t bits = 0;
    if (!take_le(8, bits, path))
        return false;
    const auto value = std::bit_cast<double>(bits);
    if (!std::isfinite(value) || bits == detail::kNegativeZeroBits)
        return refuse("non_canonical", path); // -0/NaN/±inf are never canonical
    out = Json(value);
    return true;
}

bool Decoder::take_text(std::string& out, const std::string& path) {
    std::uint64_t length = 0;
    if (!take_le(4, length, path))
        return false;
    if (bytes_.size() - pos_ < length)
        return refuse("truncated", path);
    out.assign(reinterpret_cast<const char*>(bytes_.data()) + pos_,
               static_cast<std::size_t>(length));
    pos_ += length;
    if (!detail::valid_utf8(out))
        return refuse("invalid_utf8", path);
    return true;
}

} // namespace

DecodeResult decode_payload(const EventDesc* schema, std::span<const std::byte> bytes) {
    DecodeResult result;
    if (schema == nullptr) {
        result.error = detail::codec_error(
            detail::kDecodeCode, detail::kDecodeMessage, base::Name(), "no_schema", {}, {});
        return result;
    }
    Decoder decoder(*schema, bytes);
    if (!decoder.run(result.payload))
        result.error = decoder.take_error();
    return result;
}

} // namespace midday::reflect

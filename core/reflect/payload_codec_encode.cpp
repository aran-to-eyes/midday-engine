// core/reflect/payload_codec_encode.cpp — canonical payload ENCODE (v1).
// Layout contract: payload_codec.h; wire vocabulary: payload_codec_wire.h.
// Encode is fail-closed: unknown fields, wrong shapes, and non-finite floats
// refuse with the exact field path — nothing is silently dropped or coerced
// into the canonical bytes.

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/reflect/fatal.h"
#include "core/reflect/payload_codec.h"
#include "core/reflect/payload_codec_wire.h"
#include "core/reflect/type_model.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::reflect {
namespace {

using base::Json;

class Encoder : public detail::ErrorSink {
public:
    explicit Encoder(const EventDesc& desc)
        : ErrorSink(detail::kEncodeCode, detail::kEncodeMessage, desc.name), desc_(desc) {}

    bool run(const Json& payload, std::vector<std::byte>& out);

private:
    bool put_value(const TypeDesc& type, const Json& value, const std::string& path);
    bool put_float(double value, const std::string& path);
    bool put_text(const std::string& text, const std::string& path);
    bool put_count(std::size_t count, const std::string& path);

    const EventDesc& desc_;
    std::vector<std::byte> out_;
};

bool Encoder::run(const Json& payload, std::vector<std::byte>& out) {
    if (!payload.is_object())
        return refuse("not_object");
    // Fail closed on vocabulary the schema never declared — nothing is
    // silently dropped into (or out of) the canonical bytes.
    for (const auto& [key, value] : payload.items()) {
        bool declared = false;
        for (const EventFieldDesc& field : desc_.payload)
            declared = declared || key == field.name.view();
        if (!declared)
            return refuse("unknown_field", key);
    }
    out_.push_back(std::byte{kPayloadCodecVersion});
    detail::put_le(out_, desc_.compat_hash, 8);
    const std::size_t count = desc_.payload.size();
    detail::put_le(out_, count, 4);
    const std::size_t bitmap_at = out_.size();
    out_.resize(out_.size() + (count + 7) / 8, std::byte{0});
    for (std::size_t i = 0; i < count; ++i) {
        const EventFieldDesc& field = desc_.payload[i];
        const Json* value = payload.find(field.name.view());
        if (value == nullptr)
            continue; // absent: the presence bit stays clear
        out_[bitmap_at + i / 8] |= static_cast<std::byte>(1U << (i % 8));
        if (value->is_null()) {
            out_.push_back(std::byte{detail::kTagNull}); // explicit null ≠ absent
            continue;
        }
        if (!put_value(field.type, *value, std::string(field.name.view())))
            return false;
    }
    out = std::move(out_);
    return true;
}

bool Encoder::put_value(const TypeDesc& type, const Json& value, const std::string& path) {
    const TypeKind kind = type.kind();
    switch (kind) {
    case TypeKind::kEntityRef:
        // Runtime form: EntityRef::to_bits() integers, never the authoring
        // symbolic strings (bus dispatch contract, D-BUILD-046).
        if (!value.is_int() || value.as_int() < 0)
            return refuse("field_type", path, type.canonical());
        out_.push_back(std::byte{detail::kTagEntityRef});
        detail::put_le(out_, static_cast<std::uint64_t>(value.as_int()), 8);
        return true;
    case TypeKind::kArray: {
        if (!value.is_array())
            return refuse("field_type", path, type.canonical());
        const Json::Array& elements = value.elements();
        out_.push_back(std::byte{detail::kTagArray});
        if (!put_count(elements.size(), path))
            return false;
        for (std::size_t i = 0; i < elements.size(); ++i)
            if (!put_value(type.element(), elements[i], detail::indexed(path, i)))
                return false;
        return true;
    }
    case TypeKind::kMap: {
        if (!value.is_object())
            return refuse("field_type", path, type.canonical());
        // Canonical bytes sort entries by key UTF-8 byte order — authoring
        // insertion order must never leak into the journal.
        std::vector<const std::pair<std::string, Json>*> entries;
        entries.reserve(value.items().size());
        for (const auto& item : value.items())
            entries.push_back(&item);
        std::ranges::sort(entries,
                          [](const auto* a, const auto* b) { return a->first < b->first; });
        for (std::size_t i = 1; i < entries.size(); ++i)
            if (entries[i - 1]->first == entries[i]->first)
                return refuse("duplicate_key", path + "." + entries[i]->first);
        out_.push_back(std::byte{detail::kTagMap});
        if (!put_count(entries.size(), path))
            return false;
        for (const auto* entry : entries) {
            if (!put_text(entry->first, path))
                return false;
            if (!put_value(type.element(), entry->second, path + "." + entry->first))
                return false;
        }
        return true;
    }
    default:
        break;
    }
    // Scalar leaves share the ONE authoring shape test (TypeDesc::accepts);
    // JSON null never inhabits — inside composites there is no null tag.
    if (!type.accepts(value))
        return refuse("field_type", path, type.canonical());
    out_.push_back(std::byte{detail::tag_of(kind)});
    if (kind == TypeKind::kBool) {
        out_.push_back(std::byte{value.as_bool() ? std::uint8_t{1} : std::uint8_t{0}});
        return true;
    }
    if (kind == TypeKind::kInt) {
        detail::put_le(out_, static_cast<std::uint64_t>(value.as_int()), 8);
        return true;
    }
    if (kind == TypeKind::kFloat)
        return put_float(value.as_double(), path);
    if (detail::is_text_kind(kind))
        return put_text(value.as_string(), path);
    // vec2/vec3/vec4/quat/color: accepts() proved the exact-arity number tuple.
    const Json::Array& parts = value.elements();
    for (std::size_t i = 0; i < parts.size(); ++i)
        if (!put_float(parts[i].as_double(), detail::indexed(path, i)))
            return false;
    return true;
}

bool Encoder::put_float(double value, const std::string& path) {
    if (!std::isfinite(value))
        return refuse("non_finite", path);
    auto bits = std::bit_cast<std::uint64_t>(value);
    if (bits == detail::kNegativeZeroBits)
        bits = 0; // one canonical zero: -0 normalizes to +0
    detail::put_le(out_, bits, 8);
    return true;
}

bool Encoder::put_text(const std::string& text, const std::string& path) {
    if (!detail::valid_utf8(text))
        return refuse("invalid_utf8", path);
    if (!put_count(text.size(), path))
        return false;
    for (const char c : text)
        out_.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return true;
}

bool Encoder::put_count(std::size_t count, const std::string& path) {
    if (count > std::numeric_limits<std::uint32_t>::max())
        return refuse("too_long", path);
    detail::put_le(out_, count, 4);
    return true;
}

} // namespace

EncodeResult encode_payload(const EventDesc* schema, const base::Json& payload) {
    EncodeResult result;
    if (schema == nullptr) {
        // Dynamic/unregistered payload encoding (hash 0) is a bus-integration
        // decision — refused here, never guessed (fail closed).
        result.error = detail::codec_error(
            detail::kEncodeCode, detail::kEncodeMessage, base::Name(), "no_schema", {}, {});
        return result;
    }
    Encoder encoder(*schema);
    std::vector<std::byte> bytes;
    if (!encoder.run(payload, bytes)) {
        result.error = encoder.take_error();
        return result;
    }
    // The projection is DECODED from the bytes (never built independently),
    // so encode cannot ship bytes its own decoder would not accept.
    DecodeResult projection = decode_payload(schema, bytes);
    if (projection.error.has_value())
        detail::fatal("payload codec broke its own round-trip on event '" +
                      std::string(schema->name.view()) + "': " + projection.error->code + " (" +
                      projection.error->details.dump() + ")");
    result.payload = CanonicalPayload{std::move(bytes), std::move(projection.payload)};
    return result;
}

} // namespace midday::reflect

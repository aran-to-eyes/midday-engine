// core/reflect/payload_codec.h — the canonical event-payload byte codec
// (M2 node 0B, decision D5): ONE fixed binary spelling of a typed payload,
// schema-driven from the EventDesc registry. The journal will embed these
// bytes as the AUTHORITATIVE payload record and replay will decode them —
// this codec is the M2 determinism risk-#1 mitigation, so the byte layout
// below is a pinned contract (payload_codec_test.cpp golden vector).
//
// v1 wire layout (every multi-byte integer little-endian):
//   u8   codec_version                (kPayloadCodecVersion = 1)
//   u64  payload_compat_hash          (EventDesc::compat_hash — the schema pin)
//   u32  field_count                  (DECLARED schema fields, not present ones)
//   u8[ceil(field_count/8)]           presence bitmap: field i -> byte i/8,
//                                     bit i%8 (LSB first); padding bits zero
//   then, per PRESENT field in schema DECLARATION order (never JSON
//   insertion order):
//     u8 type tag + value bytes
//
// Type tags (wire values, pinned independently of TypeKind's enum order):
//   0x00 null (explicit JSON null — DISTINCT from a cleared presence bit;
//        legal only as a whole field's value, never inside composites)
//   0x01 bool        u8 0/1
//   0x02 int         i64 two's-complement
//   0x03 float       binary64; -0 normalizes to +0; NaN/±inf refuse
//   0x04 string      u32 byte length + UTF-8 bytes (validated)
//   0x05 name        string spelling
//   0x06 vec2        2 x binary64      0x07 vec3   3 x binary64
//   0x08 vec4        4 x binary64      0x09 quat   4 x binary64
//   0x0a color       4 x binary64      (distinct tags: same numbers under a
//                                       different math type are different bytes)
//   0x0b entity_ref  u64 (EntityRef::to_bits(); runtime int form, D-BUILD-046)
//   0x0c asset_ref   string spelling
//   0x0d array       u32 count + per element (u8 tag + value), authored order
//   0x0e map         u32 count + per entry (u32 key length + key UTF-8 bytes +
//                    u8 tag + value), entries sorted by key UTF-8 byte order
//
// Canonical means CANONICAL: encode refuses anything outside the schema
// (unknown fields, wrong shapes, non-finite floats — fail-closed, the
// bus.payload_invalid ethos), and decode refuses every non-canonical byte
// spelling (wrong version/schema hash, tag drift, -0/NaN/inf bit patterns,
// unsorted map keys, invalid UTF-8, bitmap padding, trailing bytes). No two
// distinct byte strings decode to the same value; encode∘decode and
// decode∘encode are both identities on their accepted domains.
//
// Errors are structured base::Error values (never thrown):
//   "reflect.payload_invalid" (encode)  details {event, reason[, field][, expected]}
//   "reflect.payload_decode"  (decode)  details {event, reason[, field][, expected][, found]}
// with `field` the exact path of the offending value ("position[1]",
// "meta.speed", "xs[2][0]"). The later bus::trigger integration wraps these
// into its own refusal codes; this module stays bus-free.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/reflect/registry.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace midday::reflect {

inline constexpr std::uint8_t kPayloadCodecVersion = 1;

// The journal spelling of this codec (event.trigger's `payload_codec`
// field, M2 0B D5 integration): version-suffixed so a future v2 record
// names itself. ONE spelling tree-wide — writers and readers both pin it.
inline constexpr std::string_view kPayloadCodecName = "midday.payload/1";

// The two faces of one accepted payload: the authoritative canonical bytes
// and their decoded normalized projection (journal readability). The
// projection is produced by DECODING the bytes — never built independently —
// so it cannot drift from what the bytes say (D5).
struct CanonicalPayload {
    std::vector<std::byte> bytes;
    base::Json projection;
};

struct EncodeResult {
    CanonicalPayload payload;         // meaningful only when no error
    std::optional<base::Error> error; // reflect.payload_invalid

    explicit operator bool() const { return !error.has_value(); }
};

struct DecodeResult {
    base::Json payload;               // the normalized projection (schema order)
    std::optional<base::Error> error; // reflect.payload_decode

    explicit operator bool() const { return !error.has_value(); }
};

// Encode `payload` (a JSON object in the runtime field forms bus dispatch
// uses) against the event's schema. Absent fields clear their presence bit;
// explicit JSON null encodes the null tag — the two are distinct bytes.
// nullptr schema refuses (reason "no_schema"): dynamic/unregistered payload
// encoding is a later integration decision, not silently guessed here.
EncodeResult encode_payload(const EventDesc* schema, const base::Json& payload);

// Strict inverse: bytes must be a canonical v1 encoding OF THIS schema
// (compat-hash pinned — event A's bytes refuse under event B's schema).
DecodeResult decode_payload(const EventDesc* schema, std::span<const std::byte> bytes);

} // namespace midday::reflect

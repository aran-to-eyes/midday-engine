// core/reflect/payload_codec_test_support.h — shared assertions for the
// payload-codec test TUs (payload_codec_test.cpp core rules,
// payload_codec_strict_test.cpp wire strictness). Test-only: includes
// doctest, never compiled into libraries.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/reflect/payload_codec.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"

#include <cstdlib>
#include <initializer_list>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string>
#include <string_view>
#include <utility>

namespace midday::reflect::testsupport {

inline base::Json vec(std::initializer_list<double> parts) {
    base::Json out = base::Json::array();
    for (const double part : parts)
        out.push(base::Json(part));
    return out;
}

// Encode, then prove the three-way identity: an independent decode of the
// bytes equals the projection, and re-encoding the projection reproduces
// the bytes (the codec's fixed point).
inline CanonicalPayload expect_round_trip(const EventDesc* desc, const base::Json& payload) {
    EncodeResult encoded = encode_payload(desc, payload);
    if (encoded.error.has_value())
        FAIL(encoded.error->code << ": " << encoded.error->details.dump());
    DecodeResult decoded = decode_payload(desc, encoded.payload.bytes);
    if (decoded.error.has_value())
        FAIL(decoded.error->code << ": " << decoded.error->details.dump());
    CHECK(decoded.payload.dump() == encoded.payload.projection.dump());
    EncodeResult again = encode_payload(desc, encoded.payload.projection);
    REQUIRE(!again.error.has_value());
    CHECK(again.payload.bytes == encoded.payload.bytes);
    return std::move(encoded.payload);
}

inline base::Error expect_encode_refusal(const EventDesc* desc, const base::Json& payload) {
    EncodeResult encoded = encode_payload(desc, payload);
    REQUIRE(encoded.error.has_value());
    if (!encoded.error.has_value())
        std::abort(); // unreachable: REQUIRE threw
    CHECK(encoded.error->code == "reflect.payload_invalid");
    return *encoded.error;
}

inline std::string detail_text(const base::Error& error, std::string_view key) {
    const base::Json* value = error.details.find(key);
    REQUIRE_MESSAGE(value != nullptr, key);
    return value->as_string();
}

} // namespace midday::reflect::testsupport

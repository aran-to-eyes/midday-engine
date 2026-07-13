// reflect.payload_codec.* — canonical payload codec, core rules
// (payload_codec.h; M2 node 0B decision D5, the risk-#1 mitigation).
//
// Under test: round-trip identity across the WHOLE builtin vocabulary,
// byte determinism (independent encodes, key-order-scrambled input),
// -0 -> +0 normalization, non-finite refusal with exact field paths,
// absent-vs-null distinctness, fail-closed refusals, wrong-schema decode,
// and THE pinned v1 golden byte vector — the byte-layout pin future nodes
// falsify against. Wire-level strictness (tag pins, tamper suite,
// composites) lives in payload_codec_strict_test.cpp.

#include "core/base/hex.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/payload_codec.h"
#include "core/reflect/payload_codec_test_support.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string>
#include <string_view>

namespace base = midday::base;
using midday::base::bytes_to_hex;
using midday::base::hex64;
using midday::base::hex_to_bytes;
using midday::base::Json;
using midday::base::Name;
using namespace midday::reflect;
using namespace midday::reflect::testsupport;

namespace {

const EventDesc* builtin(const Registry& registry, std::string_view event) {
    const auto* entry = registry.find_event(Name(event));
    REQUIRE_MESSAGE(entry != nullptr, event);
    return &entry->desc;
}

// The golden payload (D6's corpus values): asymmetric refs, -0 components.
Json contact_began_payload() {
    Json payload = Json::object();
    payload.set("self", Json(1));
    payload.set("other", Json(2));
    payload.set("position", vec({7.0, -0.0, 3.0}));
    payload.set("normal", vec({0.0, 1.0, 0.0}));
    payload.set("impulse", Json(-0.0));
    return payload;
}

// A representative runtime payload per builtin event. FAILs on vocabulary
// this table does not know, so a tenth builtin cannot dodge the round-trip.
Json representative_payload(std::string_view event) {
    Json payload = Json::object();
    if (event == "trigger.entered" || event == "trigger.exited") {
        payload.set("trigger", Json(5));
        payload.set("other", Json(9));
    } else if (event == "contact.began") {
        payload = contact_began_payload();
    } else if (event == "contact.ended") {
        payload.set("self", Json(3));
        payload.set("other", Json(4));
    } else if (event == "state.finished") {
        payload.set("entity", Json(6));
        payload.set("region", Json("life"));
        payload.set("state", Json("Alive"));
    } else if (event == "entity.spawned") {
        payload.set("entity", Json(10));
        payload.set("parent", Json(0));
    } else if (event == "entity.despawned") {
        payload.set("entity", Json(11));
    } else if (event == "action.pressed") {
        payload.set("action", Json("jump"));
        payload.set("strength", Json(0.5));
        payload.set("device", Json(0));
    } else if (event == "action.released") {
        payload.set("action", Json("jump"));
        payload.set("device", Json(1));
    } else {
        FAIL("no representative payload for " << event << " — extend this test");
    }
    return payload;
}

} // namespace

TEST_CASE("reflect.payload_codec: every builtin event payload round-trips") {
    Registry registry;
    register_builtin_events(registry);
    CHECK(registry.events().size() == 9);
    for (const auto* entry : registry.events()) {
        const Json payload = representative_payload(entry->desc.name.view());
        const CanonicalPayload canonical = expect_round_trip(&entry->desc, payload);
        // Every declared field made it into the projection (all present).
        for (const EventFieldDesc& field : entry->desc.payload)
            CHECK_MESSAGE(canonical.projection.find(field.name.view()) != nullptr,
                          entry->desc.name.view() << "." << field.name.view());
    }
}

TEST_CASE("reflect.payload_codec: encoding is deterministic and key-order-blind") {
    Registry registry;
    register_builtin_events(registry);
    const EventDesc* desc = builtin(registry, "contact.began");

    // Two INDEPENDENT encodes of independently built payloads: byte-equal.
    const EncodeResult first = encode_payload(desc, contact_began_payload());
    const EncodeResult second = encode_payload(desc, contact_began_payload());
    REQUIRE(!first.error.has_value());
    REQUIRE(!second.error.has_value());
    CHECK(first.payload.bytes == second.payload.bytes);

    // Scrambled JSON insertion order: schema declaration order wins.
    Json scrambled = Json::object();
    scrambled.set("impulse", Json(-0.0));
    scrambled.set("normal", vec({0.0, 1.0, 0.0}));
    scrambled.set("other", Json(2));
    scrambled.set("position", vec({7.0, -0.0, 3.0}));
    scrambled.set("self", Json(1));
    const EncodeResult third = encode_payload(desc, scrambled);
    REQUIRE(!third.error.has_value());
    CHECK(third.payload.bytes == first.payload.bytes);
    CHECK(third.payload.projection.dump() == first.payload.projection.dump());
}

TEST_CASE("reflect.payload_codec: -0 normalizes to +0 in bytes and projection") {
    Registry registry;
    register_builtin_events(registry);
    const EventDesc* desc = builtin(registry, "contact.began");

    Json positive = Json::object();
    positive.set("self", Json(1));
    positive.set("other", Json(2));
    positive.set("position", vec({7.0, 0.0, 3.0}));
    positive.set("normal", vec({0.0, 1.0, 0.0}));
    positive.set("impulse", Json(0.0));

    const EncodeResult negative_zero = encode_payload(desc, contact_began_payload());
    const EncodeResult positive_zero = encode_payload(desc, positive);
    REQUIRE(!negative_zero.error.has_value());
    REQUIRE(!positive_zero.error.has_value());
    CHECK(negative_zero.payload.bytes == positive_zero.payload.bytes);

    const Json* impulse = negative_zero.payload.projection.find("impulse");
    REQUIRE(impulse != nullptr);
    CHECK(impulse->as_double() == 0.0);
    CHECK(!std::signbit(impulse->as_double()));
    CHECK(negative_zero.payload.projection.dump().find("-0") == std::string::npos);
}

TEST_CASE("reflect.payload_codec: an int inhabiting a float field encodes as binary64") {
    Registry registry;
    register_builtin_events(registry);
    const EventDesc* desc = builtin(registry, "contact.began");
    Json as_int = contact_began_payload();
    as_int.set("impulse", Json(3));
    Json as_double = contact_began_payload();
    as_double.set("impulse", Json(3.0));
    const EncodeResult first = encode_payload(desc, as_int);
    const EncodeResult second = encode_payload(desc, as_double);
    REQUIRE(!first.error.has_value());
    REQUIRE(!second.error.has_value());
    CHECK(first.payload.bytes == second.payload.bytes);
}

TEST_CASE("reflect.payload_codec: NaN and ±inf refuse with the exact field path") {
    Registry registry;
    register_builtin_events(registry);
    const EventDesc* desc = builtin(registry, "contact.began");

    Json with_nan = contact_began_payload();
    with_nan.set("position", vec({7.0, std::numeric_limits<double>::quiet_NaN(), 3.0}));
    const base::Error nan_error = expect_encode_refusal(desc, with_nan);
    CHECK(detail_text(nan_error, "reason") == "non_finite");
    CHECK(detail_text(nan_error, "field") == "position[1]");

    Json with_pos_inf = contact_began_payload();
    with_pos_inf.set("impulse", Json(std::numeric_limits<double>::infinity()));
    const base::Error pos_inf_error = expect_encode_refusal(desc, with_pos_inf);
    CHECK(detail_text(pos_inf_error, "reason") == "non_finite");
    CHECK(detail_text(pos_inf_error, "field") == "impulse");

    Json with_neg_inf = contact_began_payload();
    with_neg_inf.set("normal", vec({0.0, 1.0, -std::numeric_limits<double>::infinity()}));
    const base::Error neg_inf_error = expect_encode_refusal(desc, with_neg_inf);
    CHECK(detail_text(neg_inf_error, "reason") == "non_finite");
    CHECK(detail_text(neg_inf_error, "field") == "normal[2]");
}

TEST_CASE("reflect.payload_codec: absent field and explicit null are distinct bytes") {
    Registry registry;
    register_builtin_events(registry);
    const EventDesc* desc = builtin(registry, "contact.began");

    // Rebuild without impulse (Json has no erase — absence is authored).
    const Json full = contact_began_payload();
    Json trimmed = Json::object();
    for (const auto& [key, value] : full.items())
        if (key != "impulse")
            trimmed.set(key, value);
    Json with_null = contact_began_payload();
    with_null.set("impulse", Json(nullptr));

    const CanonicalPayload absent_payload = expect_round_trip(desc, trimmed);
    const CanonicalPayload null_payload = expect_round_trip(desc, with_null);
    CHECK(absent_payload.bytes != null_payload.bytes);

    // Presence bitmap (offset 13): impulse is field 4 — absent clears bit 4,
    // null keeps it set and spends the null tag instead.
    constexpr std::size_t kBitmapAt = 13;
    CHECK(std::to_integer<unsigned>(absent_payload.bytes[kBitmapAt]) == 0x0f);
    CHECK(std::to_integer<unsigned>(null_payload.bytes[kBitmapAt]) == 0x1f);
    CHECK(std::to_integer<unsigned>(null_payload.bytes.back()) == 0x00); // the null tag

    CHECK(absent_payload.projection.find("impulse") == nullptr);
    const Json* impulse = null_payload.projection.find("impulse");
    REQUIRE(impulse != nullptr);
    CHECK(impulse->is_null());
}

TEST_CASE("reflect.payload_codec: fail-closed encode refusals") {
    Registry registry;
    register_builtin_events(registry);
    const EventDesc* desc = builtin(registry, "contact.began");

    SUBCASE("unknown extra field") {
        Json payload = contact_began_payload();
        payload.set("extra", Json(1));
        const base::Error error = expect_encode_refusal(desc, payload);
        CHECK(detail_text(error, "reason") == "unknown_field");
        CHECK(detail_text(error, "field") == "extra");
    }
    SUBCASE("payload is not an object") {
        const base::Error error = expect_encode_refusal(desc, Json(3));
        CHECK(detail_text(error, "reason") == "not_object");
    }
    SUBCASE("mistyped scalar") {
        Json payload = contact_began_payload();
        payload.set("impulse", Json("hard"));
        const base::Error error = expect_encode_refusal(desc, payload);
        CHECK(detail_text(error, "reason") == "field_type");
        CHECK(detail_text(error, "field") == "impulse");
        CHECK(detail_text(error, "expected") == "float");
    }
    SUBCASE("wrong vec arity") {
        Json payload = contact_began_payload();
        payload.set("position", vec({7.0, 3.0}));
        const base::Error error = expect_encode_refusal(desc, payload);
        CHECK(detail_text(error, "reason") == "field_type");
        CHECK(detail_text(error, "field") == "position");
        CHECK(detail_text(error, "expected") == "vec3");
    }
    SUBCASE("negative entity ref") {
        Json payload = contact_began_payload();
        payload.set("self", Json(-1));
        const base::Error error = expect_encode_refusal(desc, payload);
        CHECK(detail_text(error, "reason") == "field_type");
        CHECK(detail_text(error, "field") == "self");
        CHECK(detail_text(error, "expected") == "entity_ref");
    }
    SUBCASE("no schema") {
        EncodeResult refused = encode_payload(nullptr, contact_began_payload());
        REQUIRE(refused.error.has_value());
        if (!refused.error.has_value())
            std::abort(); // unreachable: REQUIRE threw
        CHECK(detail_text(*refused.error, "reason") == "no_schema");
    }
}

TEST_CASE("reflect.payload_codec: event A's bytes refuse under event B's schema") {
    Registry registry;
    register_builtin_events(registry);
    // contact.ended {self, other} and trigger.exited {trigger, other} have
    // IDENTICAL field types — only the compat-hash pin can tell them apart.
    const EventDesc* ended = builtin(registry, "contact.ended");
    const EventDesc* exited = builtin(registry, "trigger.exited");
    Json payload = Json::object();
    payload.set("self", Json(3));
    payload.set("other", Json(4));

    const EncodeResult encoded = encode_payload(ended, payload);
    REQUIRE(!encoded.error.has_value());
    DecodeResult crossed = decode_payload(exited, encoded.payload.bytes);
    REQUIRE(crossed.error.has_value());
    if (!crossed.error.has_value())
        std::abort(); // unreachable: REQUIRE threw
    CHECK(crossed.error->code == "reflect.payload_decode");
    CHECK(detail_text(*crossed.error, "reason") == "schema_mismatch");
    CHECK(detail_text(*crossed.error, "expected") == hex64(exited->compat_hash));
    CHECK(detail_text(*crossed.error, "found") == hex64(ended->compat_hash));

    // The right schema still accepts the same bytes.
    CHECK(!decode_payload(ended, encoded.payload.bytes).error.has_value());
}

TEST_CASE("reflect.payload_codec: GOLDEN v1 byte vector (contact.began)") {
    // THE byte-layout pin (M2 0B D5): if this hex moves, the wire format
    // changed — a deliberate, versioned format break, never an accident.
    // Payload: asymmetric refs, position [7,-0,3], impulse -0 (D6 values).
    constexpr std::string_view kGoldenHex =
        "0156635c241685d608050000001f"                       // v1, schema pin LE, 5 fields, all set
        "0b0100000000000000"                                 // self: entity_ref 1
        "0b0200000000000000"                                 // other: entity_ref 2
        "070000000000001c4000000000000000000000000000000840" // position: vec3 [7, -0→0, 3]
        "070000000000000000000000000000f03f0000000000000000" // normal: vec3 [0, 1, 0]
        "030000000000000000";                                // impulse: float -0→0
    constexpr std::string_view kGoldenProjection =
        R"({"self":1,"other":2,"position":[7,0,3],"normal":[0,1,0],"impulse":0})";

    Registry registry;
    register_builtin_events(registry);
    const EventDesc* desc = builtin(registry, "contact.began");

    const EncodeResult encoded = encode_payload(desc, contact_began_payload());
    REQUIRE(!encoded.error.has_value());
    CHECK(encoded.payload.bytes.size() == 91);
    CHECK(bytes_to_hex(encoded.payload.bytes) == kGoldenHex);
    CHECK(encoded.payload.projection.dump() == kGoldenProjection);

    // The header carries the schema pin: hex chars [2, 18) are the
    // little-endian spelling of contact.began's compat hash.
    const std::string hash_hex = hex64(desc->compat_hash);
    std::string hash_le;
    for (std::size_t i = 16; i >= 2; i -= 2)
        hash_le += hash_hex.substr(i - 2, 2);
    CHECK(kGoldenHex.substr(2, 16) == hash_le);

    // Pure decode path: the pinned hex alone reproduces the projection.
    auto bytes = hex_to_bytes(kGoldenHex);
    const DecodeResult decoded = decode_payload(desc, midday::testkit::unwrap(bytes));
    REQUIRE(!decoded.error.has_value());
    CHECK(decoded.payload.dump() == kGoldenProjection);
}

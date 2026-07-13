// reflect.payload_codec.* — wire-level strictness (payload_codec.h).
//
// Under test: the pinned per-TypeKind tag table (EVERY kind, distinct wire
// values), tag-not-number distinguishes math types with equal components,
// composite fields (array order, canonical map key sorting, nested), null
// refusing inside composites, and the decode tamper suite — every
// non-canonical byte spelling refuses with a structured reason. Core rules
// (vocabulary round-trips, golden vector) live in payload_codec_test.cpp.

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/reflect/payload_codec.h"
#include "core/reflect/payload_codec_test_support.h"
#include "core/reflect/registry.h"
#include "core/reflect/type_model.h"
#include "testkit/doctest.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace base = midday::base;
using midday::base::Json;
using midday::base::Name;
using namespace midday::reflect;
using namespace midday::reflect::testsupport;

namespace {

// One event, one field "v" of the given canonical type spelling.
const EventDesc&
single_field_event(Registry& registry, std::string_view event, std::string_view type) {
    const auto parsed = TypeDesc::parse(type);
    REQUIRE_MESSAGE(parsed.has_value(), type);
    if (!parsed.has_value())
        std::abort(); // unreachable: REQUIRE threw
    EventDesc desc;
    desc.name = Name(event);
    desc.doc = "codec strictness fixture";
    desc.payload.push_back(EventFieldDesc{Name("v"), *parsed, "the field under test"});
    return registry.add_event(std::move(desc)).desc;
}

std::vector<std::byte> encode_ok(const EventDesc& desc, const Json& value) {
    Json payload = Json::object();
    payload.set("v", value);
    EncodeResult encoded = encode_payload(&desc, payload);
    if (encoded.error.has_value())
        FAIL(encoded.error->code << ": " << encoded.error->details.dump());
    return std::move(encoded.payload.bytes);
}

base::Error expect_decode_refusal(const EventDesc& desc, std::span<const std::byte> bytes) {
    DecodeResult decoded = decode_payload(&desc, bytes);
    REQUIRE(decoded.error.has_value());
    if (!decoded.error.has_value())
        std::abort(); // unreachable: REQUIRE threw
    CHECK(decoded.error->code == "reflect.payload_decode");
    return *decoded.error;
}

std::vector<std::byte> with_byte(std::vector<std::byte> bytes, std::size_t at, unsigned value) {
    REQUIRE(at < bytes.size());
    bytes[at] = static_cast<std::byte>(value);
    return bytes;
}

// v1 offsets for a single-field event: u8 version (0), u64 hash (1),
// u32 field count (9), 1-byte bitmap (13), field tag (14), value (15).
constexpr std::size_t kCountAt = 9;
constexpr std::size_t kBitmapAt = 13;
constexpr std::size_t kTagAt = 14;
constexpr std::size_t kValueAt = 15;

} // namespace

TEST_CASE("reflect.payload_codec: every TypeKind carries its pinned distinct wire tag") {
    // The tag table IS the wire contract: pinned per kind, decoupled from
    // TypeKind's enum order. A moved value here is a format break.
    Registry registry;

    struct Pin {
        std::string_view type;
        unsigned tag;
        Json value;
    };

    Json array_value = Json::array();
    array_value.push(Json(1));
    Json map_value = Json::object();
    map_value.set("a", Json(1));
    const Pin pins[] = {
        {"bool", 0x01, Json(true)},
        {"int", 0x02, Json(7)},
        {"float", 0x03, Json(1.5)},
        {"string", 0x04, Json("s")},
        {"name", 0x05, Json("n")},
        {"vec2", 0x06, vec({1.0, 2.0})},
        {"vec3", 0x07, vec({1.0, 2.0, 3.0})},
        {"vec4", 0x08, vec({1.0, 2.0, 3.0, 4.0})},
        {"quat", 0x09, vec({1.0, 2.0, 3.0, 4.0})},
        {"color", 0x0a, vec({1.0, 2.0, 3.0, 4.0})},
        {"entity_ref", 0x0b, Json(1)},
        {"asset_ref", 0x0c, Json("art/a.png")},
        {"array<int>", 0x0d, array_value},
        {"map<int>", 0x0e, map_value},
    };
    std::vector<unsigned> seen;
    for (const Pin& pin : pins) {
        const EventDesc& desc =
            single_field_event(registry, std::string("pin.") + std::string(pin.type), pin.type);
        const std::vector<std::byte> bytes = encode_ok(desc, pin.value);
        CHECK_MESSAGE(std::to_integer<unsigned>(bytes[kTagAt]) == pin.tag, pin.type);
        seen.push_back(pin.tag);
    }
    std::ranges::sort(seen);
    CHECK(std::ranges::adjacent_find(seen) == seen.end()); // pairwise distinct
}

TEST_CASE("reflect.payload_codec: equal components under different math types differ by tag") {
    Registry registry;
    const EventDesc& vec4_event = single_field_event(registry, "tag.vec4", "vec4");
    const EventDesc& quat_event = single_field_event(registry, "tag.quat", "quat");
    const EventDesc& color_event = single_field_event(registry, "tag.color", "color");
    const EventDesc& vec3_event = single_field_event(registry, "tag.vec3", "vec3");

    const Json four = vec({1.0, 2.0, 3.0, 4.0});
    const std::vector<std::byte> vec4_bytes = encode_ok(vec4_event, four);
    const std::vector<std::byte> quat_bytes = encode_ok(quat_event, four);
    const std::vector<std::byte> color_bytes = encode_ok(color_event, four);
    CHECK(vec4_bytes != quat_bytes);
    CHECK(quat_bytes != color_bytes);

    // Isolate the discriminator: the field tag differs, the 32 component
    // bytes are identical — the TYPE rides the tag, never the numbers.
    CHECK(vec4_bytes[kTagAt] != quat_bytes[kTagAt]);
    CHECK(quat_bytes[kTagAt] != color_bytes[kTagAt]);
    const auto components = [](const std::vector<std::byte>& bytes) {
        return std::vector<std::byte>(bytes.begin() + kValueAt, bytes.end());
    };
    CHECK(components(vec4_bytes) == components(quat_bytes));
    CHECK(components(quat_bytes) == components(color_bytes));

    // Vec3 vs quat with the same numeric prefix: distinct tags, and the
    // shared 1,2,3 prefix bytes are equal — only tag and arity differ.
    const std::vector<std::byte> vec3_bytes = encode_ok(vec3_event, vec({1.0, 2.0, 3.0}));
    CHECK(vec3_bytes[kTagAt] != quat_bytes[kTagAt]);
    CHECK(std::equal(vec3_bytes.begin() + kValueAt,
                     vec3_bytes.end(),
                     quat_bytes.begin() + kValueAt,
                     quat_bytes.begin() + kValueAt + 24));
}

TEST_CASE("reflect.payload_codec: composites round-trip; map keys sort canonically") {
    Registry registry;

    SUBCASE("array keeps authored order and normalizes -0 elements") {
        const EventDesc& desc = single_field_event(registry, "cfg.arr", "array<float>");
        Json payload = Json::object();
        payload.set("v", vec({1.5, -0.0, 2.5}));
        const CanonicalPayload canonical = expect_round_trip(&desc, payload);
        CHECK(canonical.projection.dump() == R"({"v":[1.5,0,2.5]})");
    }
    SUBCASE("map entries encode key-sorted whatever the authoring order") {
        const EventDesc& desc = single_field_event(registry, "cfg.map", "map<int>");
        Json sorted_input = Json::object();
        Json sorted_map = Json::object();
        sorted_map.set("a", Json(2));
        sorted_map.set("b", Json(1));
        sorted_map.set("c", Json(3));
        sorted_input.set("v", sorted_map);
        Json scrambled_input = Json::object();
        Json scrambled_map = Json::object();
        scrambled_map.set("b", Json(1));
        scrambled_map.set("c", Json(3));
        scrambled_map.set("a", Json(2));
        scrambled_input.set("v", scrambled_map);
        const CanonicalPayload sorted = expect_round_trip(&desc, sorted_input);
        const CanonicalPayload scrambled = expect_round_trip(&desc, scrambled_input);
        CHECK(sorted.bytes == scrambled.bytes);
        CHECK(scrambled.projection.dump() == R"({"v":{"a":2,"b":1,"c":3}})");
    }
    SUBCASE("nested map<array<int>> sorts by UTF-8 byte order") {
        const EventDesc& desc = single_field_event(registry, "cfg.nested", "map<array<int>>");
        Json inner_z = Json::array();
        inner_z.push(Json(1));
        Json inner_a = Json::array();
        inner_a.push(Json(2));
        inner_a.push(Json(3));
        Json nested = Json::object();
        nested.set("z", inner_z);
        nested.set("a", inner_a);
        Json payload = Json::object();
        payload.set("v", nested);
        const CanonicalPayload canonical = expect_round_trip(&desc, payload);
        CHECK(canonical.projection.dump() == R"({"v":{"a":[2,3],"z":[1]}})");
    }
    SUBCASE("null refuses inside composites — no null tag below field level") {
        const EventDesc& arr = single_field_event(registry, "cfg.null_arr", "array<float>");
        Json arr_payload = Json::object();
        Json arr_value = Json::array();
        arr_value.push(Json(1.0));
        arr_value.push(Json(nullptr));
        arr_payload.set("v", arr_value);
        const base::Error arr_error = expect_encode_refusal(&arr, arr_payload);
        CHECK(detail_text(arr_error, "reason") == "field_type");
        CHECK(detail_text(arr_error, "field") == "v[1]");

        const EventDesc& map = single_field_event(registry, "cfg.null_map", "map<int>");
        Json map_payload = Json::object();
        Json map_value = Json::object();
        map_value.set("k", Json(nullptr));
        map_payload.set("v", map_value);
        const base::Error map_error = expect_encode_refusal(&map, map_payload);
        CHECK(detail_text(map_error, "reason") == "field_type");
        CHECK(detail_text(map_error, "field") == "v.k");
    }
    SUBCASE("non-finite refuses with the full nested path") {
        const EventDesc& desc = single_field_event(registry, "cfg.deep", "array<vec3>");
        Json payload = Json::object();
        Json value = Json::array();
        value.push(vec({1.0, 2.0, 3.0}));
        value.push(vec({4.0, std::numeric_limits<double>::quiet_NaN(), 6.0}));
        payload.set("v", value);
        const base::Error error = expect_encode_refusal(&desc, payload);
        CHECK(detail_text(error, "reason") == "non_finite");
        CHECK(detail_text(error, "field") == "v[1][1]");
    }
}

TEST_CASE("reflect.payload_codec: decode refuses every non-canonical byte spelling") {
    Registry registry;

    SUBCASE("empty, version, truncation, trailing, count, padding") {
        const EventDesc& desc = single_field_event(registry, "wire.float", "float");
        const std::vector<std::byte> bytes = encode_ok(desc, Json(1.5));

        CHECK(detail_text(expect_decode_refusal(desc, {}), "reason") == "truncated");
        CHECK(detail_text(expect_decode_refusal(desc, with_byte(bytes, 0, 2)), "reason") ==
              "version");
        const std::vector<std::byte> chopped(bytes.begin(), bytes.end() - 1);
        CHECK(detail_text(expect_decode_refusal(desc, chopped), "reason") == "truncated");
        std::vector<std::byte> trailing = bytes;
        trailing.push_back(std::byte{0});
        CHECK(detail_text(expect_decode_refusal(desc, trailing), "reason") == "trailing_bytes");
        CHECK(detail_text(expect_decode_refusal(desc, with_byte(bytes, kCountAt, 2)), "reason") ==
              "field_count");
        CHECK(detail_text(expect_decode_refusal(desc, with_byte(bytes, kBitmapAt, 0x03)),
                          "reason") == "bitmap_padding");

        // Tag drift: a float field spelled with the int tag refuses.
        const base::Error tag_error = expect_decode_refusal(desc, with_byte(bytes, kTagAt, 0x02));
        CHECK(detail_text(tag_error, "reason") == "tag_mismatch");
        CHECK(detail_text(tag_error, "field") == "v");
        CHECK(detail_text(tag_error, "expected") == "float");
    }
    SUBCASE("float bit patterns: -0, NaN, +inf are never canonical bytes") {
        const EventDesc& desc = single_field_event(registry, "wire.zero", "float");
        const std::vector<std::byte> bytes = encode_ok(desc, Json(0.0));
        constexpr std::size_t kHighByte = kValueAt + 7;
        const base::Error minus_zero =
            expect_decode_refusal(desc, with_byte(bytes, kHighByte, 0x80));
        CHECK(detail_text(minus_zero, "reason") == "non_canonical");
        CHECK(detail_text(minus_zero, "field") == "v");
        const auto nan_bytes = with_byte(with_byte(bytes, kHighByte, 0x7f), kHighByte - 1, 0xf8);
        CHECK(detail_text(expect_decode_refusal(desc, nan_bytes), "reason") == "non_canonical");
        const auto inf_bytes = with_byte(with_byte(bytes, kHighByte, 0x7f), kHighByte - 1, 0xf0);
        CHECK(detail_text(expect_decode_refusal(desc, inf_bytes), "reason") == "non_canonical");
    }
    SUBCASE("bool bytes beyond 0/1 refuse") {
        const EventDesc& desc = single_field_event(registry, "wire.bool", "bool");
        const std::vector<std::byte> bytes = encode_ok(desc, Json(true));
        CHECK(detail_text(expect_decode_refusal(desc, with_byte(bytes, kValueAt, 2)), "reason") ==
              "non_canonical");
    }
    SUBCASE("entity_ref with the high bit set refuses") {
        const EventDesc& desc = single_field_event(registry, "wire.ref", "entity_ref");
        const std::vector<std::byte> bytes = encode_ok(desc, Json(1));
        CHECK(detail_text(expect_decode_refusal(desc, with_byte(bytes, kValueAt + 7, 0x80)),
                          "reason") == "non_canonical");
    }
    SUBCASE("strings: invalid UTF-8 and lying lengths refuse") {
        const EventDesc& desc = single_field_event(registry, "wire.str", "string");
        const std::vector<std::byte> bytes = encode_ok(desc, Json("ab"));
        constexpr std::size_t kTextAt = kValueAt + 4; // after the u32 length
        CHECK(detail_text(expect_decode_refusal(desc, with_byte(bytes, kTextAt, 0xff)), "reason") ==
              "invalid_utf8");
        CHECK(detail_text(expect_decode_refusal(desc, with_byte(bytes, kValueAt, 5)), "reason") ==
              "truncated");
    }
    SUBCASE("map entries out of key order refuse") {
        const EventDesc& desc = single_field_event(registry, "wire.map", "map<int>");
        Json value = Json::object();
        value.set("a", Json(1));
        value.set("b", Json(2));
        const std::vector<std::byte> bytes = encode_ok(desc, value);
        // Entries start after tag + u32 count; each is u32 keylen + 1-byte
        // key + u8 tag + i64 = 14 bytes. Swapping the blocks breaks the
        // strictly-ascending key contract.
        constexpr std::size_t kEntriesAt = kValueAt + 4;
        constexpr std::size_t kEntrySize = 14;
        REQUIRE(bytes.size() == kEntriesAt + 2 * kEntrySize);
        std::vector<std::byte> swapped = bytes;
        std::copy(
            bytes.begin() + kEntriesAt + kEntrySize, bytes.end(), swapped.begin() + kEntriesAt);
        std::copy(bytes.begin() + kEntriesAt,
                  bytes.begin() + kEntriesAt + kEntrySize,
                  swapped.begin() + kEntriesAt + kEntrySize);
        const base::Error error = expect_decode_refusal(desc, swapped);
        CHECK(detail_text(error, "reason") == "map_key_order");
        CHECK(detail_text(error, "field") == "v.a");
    }
    SUBCASE("no schema refuses decode too") {
        const EventDesc& desc = single_field_event(registry, "wire.none", "int");
        const std::vector<std::byte> bytes = encode_ok(desc, Json(1));
        DecodeResult refused = decode_payload(nullptr, bytes);
        REQUIRE(refused.error.has_value());
        if (!refused.error.has_value())
            std::abort(); // unreachable: REQUIRE threw
        CHECK(detail_text(*refused.error, "reason") == "no_schema");
    }
}

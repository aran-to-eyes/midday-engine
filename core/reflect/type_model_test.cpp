// reflect.type.* — the data-driven type model (core/reflect/type_model.h).
//
// Under test: the canonical spelling round-trips through parse(), strictly;
// accepts() decides whether JSON literals inhabit a type — the registration-
// time guard on property defaults and the seed of validate-before-write.

#include "core/base/json.h"
#include "core/reflect/type_model.h"
#include "testkit/doctest.h"

#include <optional>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string_view>

using midday::base::Json;
using midday::reflect::TypeDesc;
using midday::reflect::TypeKind;

namespace {

Json parsed(std::string_view text) {
    auto result = Json::parse(text);
    REQUIRE(!result.error.has_value());
    return result.value;
}

} // namespace

TEST_CASE("reflect.type: canonical spellings cover every kind") {
    CHECK(TypeDesc::scalar(TypeKind::kBool).canonical() == "bool");
    CHECK(TypeDesc::scalar(TypeKind::kInt).canonical() == "int");
    CHECK(TypeDesc::scalar(TypeKind::kFloat).canonical() == "float");
    CHECK(TypeDesc::scalar(TypeKind::kString).canonical() == "string");
    CHECK(TypeDesc::scalar(TypeKind::kName).canonical() == "name");
    CHECK(TypeDesc::scalar(TypeKind::kVec2).canonical() == "vec2");
    CHECK(TypeDesc::scalar(TypeKind::kVec3).canonical() == "vec3");
    CHECK(TypeDesc::scalar(TypeKind::kVec4).canonical() == "vec4");
    CHECK(TypeDesc::scalar(TypeKind::kQuat).canonical() == "quat");
    CHECK(TypeDesc::scalar(TypeKind::kColor).canonical() == "color");
    CHECK(TypeDesc::scalar(TypeKind::kEntityRef).canonical() == "entity_ref");
    CHECK(TypeDesc::scalar(TypeKind::kAssetRef).canonical() == "asset_ref");
    CHECK(TypeDesc::array_of(TypeDesc::scalar(TypeKind::kVec3)).canonical() == "array<vec3>");
    CHECK(TypeDesc::map_of(TypeDesc::scalar(TypeKind::kFloat)).canonical() == "map<float>");
    CHECK(TypeDesc::array_of(TypeDesc::map_of(TypeDesc::scalar(TypeKind::kInt))).canonical() ==
          "array<map<int>>");
}

TEST_CASE("reflect.type: parse is the strict inverse of canonical") {
    for (std::string_view text : {"bool",
                                  "int",
                                  "float",
                                  "string",
                                  "name",
                                  "vec2",
                                  "vec3",
                                  "vec4",
                                  "quat",
                                  "color",
                                  "entity_ref",
                                  "asset_ref",
                                  "array<vec3>",
                                  "map<string>",
                                  "map<array<entity_ref>>"}) {
        auto type = TypeDesc::parse(text);
        REQUIRE_MESSAGE(type.has_value(), text);
        if (type.has_value()) // re-proven for the analyzer's dataflow model
            CHECK(type->canonical() == text);
    }
    // Composite structure survives the round trip as a value, not just text.
    auto nested = TypeDesc::parse("array<map<int>>");
    REQUIRE(nested.has_value());
    if (nested.has_value()) { // re-proven for the analyzer's dataflow model
        CHECK(*nested == TypeDesc::array_of(TypeDesc::map_of(TypeDesc::scalar(TypeKind::kInt))));
        CHECK(nested->element().element().kind() == TypeKind::kInt);
    }
}

TEST_CASE("reflect.type: parse rejects everything that is not canonical") {
    for (std::string_view text : {"",
                                  "array",
                                  "map",
                                  "array<>",
                                  "array<bogus>",
                                  "array<vec3",
                                  "array<vec3>>",
                                  "map<float> ",
                                  " float",
                                  "Float",
                                  "vec5",
                                  "array<map<int>",
                                  "int32",
                                  "entity ref"}) {
        CHECK_MESSAGE(!TypeDesc::parse(text).has_value(), text);
    }
}

TEST_CASE("reflect.type: accepts() decides literal inhabitance per kind") {
    CHECK(TypeDesc::scalar(TypeKind::kBool).accepts(parsed("true")));
    CHECK_FALSE(TypeDesc::scalar(TypeKind::kBool).accepts(parsed("1")));

    CHECK(TypeDesc::scalar(TypeKind::kInt).accepts(parsed("42")));
    CHECK_FALSE(TypeDesc::scalar(TypeKind::kInt).accepts(parsed("42.5")));

    // float accepts any number: authors write `100` for 100.0.
    CHECK(TypeDesc::scalar(TypeKind::kFloat).accepts(parsed("42.5")));
    CHECK(TypeDesc::scalar(TypeKind::kFloat).accepts(parsed("100")));
    CHECK_FALSE(TypeDesc::scalar(TypeKind::kFloat).accepts(parsed("\"100\"")));

    // string-shaped kinds: string, name, asset_ref (path), entity_ref
    // (symbolic key: self/root/global/<group> — spec section 4.2).
    for (TypeKind kind :
         {TypeKind::kString, TypeKind::kName, TypeKind::kAssetRef, TypeKind::kEntityRef}) {
        CHECK(TypeDesc::scalar(kind).accepts(parsed("\"self\"")));
        CHECK_FALSE(TypeDesc::scalar(kind).accepts(parsed("7")));
    }

    // fixed-width numeric vectors
    CHECK(TypeDesc::scalar(TypeKind::kVec2).accepts(parsed("[0,1.5]")));
    CHECK(TypeDesc::scalar(TypeKind::kVec3).accepts(parsed("[0,0,0]")));
    CHECK(TypeDesc::scalar(TypeKind::kVec4).accepts(parsed("[1,2,3,4]")));
    CHECK(TypeDesc::scalar(TypeKind::kQuat).accepts(parsed("[0,0,0,1]")));
    CHECK(TypeDesc::scalar(TypeKind::kColor).accepts(parsed("[1,1,1,1]")));
    CHECK_FALSE(TypeDesc::scalar(TypeKind::kVec3).accepts(parsed("[0,0]")));
    CHECK_FALSE(TypeDesc::scalar(TypeKind::kVec3).accepts(parsed("[0,0,\"z\"]")));
    CHECK_FALSE(TypeDesc::scalar(TypeKind::kQuat).accepts(parsed("[0,0,0]")));

    // composites recurse
    auto floats = TypeDesc::array_of(TypeDesc::scalar(TypeKind::kFloat));
    CHECK(floats.accepts(parsed("[1,2.5,3]")));
    CHECK(floats.accepts(parsed("[]")));
    CHECK_FALSE(floats.accepts(parsed("[1,\"two\"]")));
    auto by_name = TypeDesc::map_of(TypeDesc::scalar(TypeKind::kInt));
    CHECK(by_name.accepts(parsed("{\"a\":1,\"b\":2}")));
    CHECK_FALSE(by_name.accepts(parsed("{\"a\":1.5}")));
    CHECK_FALSE(by_name.accepts(parsed("[1]")));

    // null never inhabits: null means "absent", not a value of any type.
    CHECK_FALSE(TypeDesc::scalar(TypeKind::kString).accepts(parsed("null")));
    CHECK_FALSE(floats.accepts(parsed("null")));
}

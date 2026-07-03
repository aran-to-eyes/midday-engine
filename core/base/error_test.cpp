// core.error.* — tests for the engine-wide structured Error envelope
// (core/base/error.h). cli/envelope.h wraps this type; there is exactly one
// definition of code/message/details in the tree.

#include "core/base/error.h"
#include "core/base/json.h"
#include "doctest/doctest.h"

using midday::base::Error;
using midday::base::Json;

TEST_CASE("core.error: serializes as {code,message[,details]} in that order") {
    Error plain{.code = "loader.missing_ref", .message = "state 'Dead' not found"};
    CHECK(plain.to_json().dump() ==
          R"({"code":"loader.missing_ref","message":"state 'Dead' not found"})");

    Error detailed = plain;
    detailed.details.set("file", "boss.machine.yaml");
    detailed.details.set("line", 12);
    CHECK(detailed.to_json().dump() ==
          R"({"code":"loader.missing_ref","message":"state 'Dead' not found",)"
          R"("details":{"file":"boss.machine.yaml","line":12}})");
}

TEST_CASE("core.error: JSON round trip is lossless and byte-stable") {
    Error original{.code = "selftest.failed", .message = "2 of 40 test cases failed"};
    original.details.set("cases_failed", 2);
    original.details.set("cases", 40);

    const std::string wire = original.to_json().dump();
    Json::ParseResult parsed = Json::parse(wire);
    REQUIRE(parsed);
    std::optional<Error> parsed_error = Error::from_json(parsed.value);
    REQUIRE(parsed_error.has_value()); // REQUIRE aborts the case; fallback never reports
    const Error restored = parsed_error.value_or(Error{});
    CHECK(restored.code == original.code);
    CHECK(restored.message == original.message);
    CHECK(restored.details.dump() == original.details.dump());
    // Two independent trips produce identical bytes (determinism idiom).
    CHECK(restored.to_json().dump() == wire);
}

TEST_CASE("core.error: from_json is strict about the envelope shape") {
    auto reject = [](std::string_view text) {
        Json::ParseResult parsed = Json::parse(text);
        REQUIRE(parsed);
        return !Error::from_json(parsed.value).has_value();
    };
    CHECK(reject(R"({"message":"no code"})"));
    CHECK(reject(R"({"code":"","message":"empty code"})"));
    CHECK(reject(R"({"code":"x"})"));                            // missing message
    CHECK(reject(R"({"code":"x","message":1})"));                // wrong type
    CHECK(reject(R"({"code":"x","message":"m","details":[]})")); // details not object
    CHECK(reject(R"({"code":"x","message":"m","extra":1})"));    // unknown key
    CHECK(reject(R"(["code","message"])"));                      // not an object

    Json::ParseResult ok = Json::parse(R"({"code":"x","message":"m"})");
    REQUIRE(ok);
    CHECK(Error::from_json(ok.value).has_value());
}

TEST_CASE("core.error: JSON parse failures convert to structured errors") {
    Json::ParseResult bad = Json::parse("{\"a\":1,\n\"b\" 2}", "scene.json");
    REQUIRE_FALSE(bad);
    Error error = midday::base::to_error(bad.error.value_or(midday::base::JsonParseError{}));
    CHECK(error.code == "json.parse");
    CHECK(error.message.starts_with("scene.json:2:5: "));
    REQUIRE(error.details.find("file") != nullptr);
    CHECK(error.details.find("file")->as_string() == "scene.json");
    CHECK(error.details.find("line")->as_int() == 2);
    CHECK(error.details.find("col")->as_int() == 5);
    CHECK(error.details.find("offset")->as_int() == 12);
}

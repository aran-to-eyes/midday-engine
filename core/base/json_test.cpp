// core.json.* — tests for THE tree-wide JSON implementation (core/base/json.*).
// Written first; the implementation exists to make these pass. The write-side
// cases migrated here from cli/verbs/envelope_test.cpp when cli/json.* folded
// into core/base (D-BUILD-003 planned migration).

#include "core/base/json.h"
#include "doctest/doctest.h"

#include <cstdint>
#include <string>

using midday::base::Json;
using midday::base::JsonParseError;

namespace {

JsonParseError expect_error(std::string_view text) {
    Json::ParseResult result = Json::parse(text, "t.json");
    REQUIRE_FALSE(result); // REQUIRE aborts the case, so the fallback never reports
    return result.error.value_or(JsonParseError{});
}

} // namespace

TEST_CASE("core.json: scalars serialize deterministically") {
    CHECK(Json(nullptr).dump() == "null");
    CHECK(Json(true).dump() == "true");
    CHECK(Json(false).dump() == "false");
    CHECK(Json(0).dump() == "0");
    CHECK(Json(-42).dump() == "-42");
    CHECK(Json(std::int64_t{9007199254740993}).dump() == "9007199254740993");
    CHECK(Json(1.5).dump() == "1.5");
    CHECK(Json(0.1).dump() == "0.1"); // shortest round-trip, not 0.1000000000000000055
}

TEST_CASE("core.json: non-finite doubles serialize as null, never invalid JSON") {
    // JSON has no NaN/Inf. Sim code must never produce them (spec section 4.3);
    // if one leaks into diagnostics the writer still emits valid JSON.
    CHECK(Json(std::numeric_limits<double>::quiet_NaN()).dump() == "null");
    CHECK(Json(std::numeric_limits<double>::infinity()).dump() == "null");
    CHECK(Json(-std::numeric_limits<double>::infinity()).dump() == "null");
}

TEST_CASE("core.json: strings escape control, quote, backslash; UTF-8 passes through") {
    CHECK(Json("plain").dump() == "\"plain\"");
    CHECK(Json("a\"b\\c").dump() == "\"a\\\"b\\\\c\"");
    CHECK(Json("line\nbreak\ttab\rret").dump() == "\"line\\nbreak\\ttab\\rret\"");
    CHECK(Json(std::string("\x01\x1f")).dump() == "\"\\u0001\\u001f\"");
    CHECK(Json("smörgås — 日本").dump() == "\"smörgås — 日本\"");
}

TEST_CASE("core.json: objects preserve insertion order and round out arrays") {
    Json obj = Json::object();
    obj.set("zeta", 1);
    obj.set("alpha", 2);
    obj.set("zeta", 3); // replace in place, order kept
    CHECK(obj.dump() == "{\"zeta\":3,\"alpha\":2}");

    Json arr = Json::array();
    arr.push(1);
    arr.push("two");
    arr.push(Json::object());
    CHECK(arr.dump() == "[1,\"two\",{}]");

    // Determinism: dumping twice is byte-identical.
    CHECK(obj.dump() == obj.dump());
}

TEST_CASE("core.json: typed accessors expose parsed values") {
    Json::ParseResult result =
        Json::parse(R"({"b":true,"i":-7,"d":2.5,"s":"x","a":[1,2],"o":{"k":null}})");
    REQUIRE(result);
    const Json& v = result.value;
    REQUIRE(v.is_object());
    CHECK(v.find("b")->as_bool() == true);
    CHECK(v.find("i")->as_int() == -7);
    CHECK(v.find("d")->as_double() == 2.5);
    CHECK(v.find("i")->as_double() == -7.0); // int coerces to double
    CHECK(v.find("s")->as_string() == "x");
    REQUIRE(v.find("a")->is_array());
    CHECK(v.find("a")->elements().size() == 2);
    CHECK(v.find("a")->elements()[1].as_int() == 2);
    REQUIRE(v.find("o")->is_object());
    CHECK(v.find("o")->find("k")->is_null());
    CHECK(v.find("missing") == nullptr);
}

TEST_CASE("core.json: parse handles scalars, nesting, and whitespace") {
    CHECK(Json::parse("null").value.is_null());
    CHECK(Json::parse("true").value.as_bool());
    CHECK(Json::parse(" \t\r\n 42 \n").value.as_int() == 42);
    CHECK(Json::parse("-9223372036854775808").value.as_int() == INT64_MIN);
    CHECK(Json::parse("9223372036854775807").value.as_int() == INT64_MAX);
    // Integers beyond int64 degrade to double (standard JSON interop).
    CHECK(Json::parse("9223372036854775808").value.is_double());
    CHECK(Json::parse("1e+30").value.is_double());
    CHECK(Json::parse("[ [ ] , [ 1 ] ]").value.dump() == "[[],[1]]");
}

TEST_CASE("core.json: negative zero stays a double so dump-parse is a fixed point") {
    Json::ParseResult minus_zero = Json::parse("-0");
    REQUIRE(minus_zero);
    CHECK(minus_zero.value.is_double());
    CHECK(minus_zero.value.dump() == "-0");
    CHECK(Json::parse("0").value.is_int()); // plain zero stays an int
}

TEST_CASE("core.json: string escapes decode, including surrogate pairs") {
    CHECK(Json::parse(R"("a\"b\\c\/d")").value.as_string() == "a\"b\\c/d");
    CHECK(Json::parse(R"("Aé日")").value.as_string() == "Aé日");
    CHECK(Json::parse(R"("😀")").value.as_string() == "😀"); // surrogate pair
    CHECK(Json::parse(R"("\n\t\r\b\f")").value.as_string() == "\n\t\r\b\f");
}

TEST_CASE("core.json: canonical documents round-trip byte-identically") {
    const std::string doc = R"({"ok":true,"verb":"run","exit_code":0,"ticks":100,)"
                            R"("ratio":0.1,"tags":["a","b"],"error":null})";
    Json::ParseResult first = Json::parse(doc);
    REQUIRE(first);
    CHECK(first.value.dump() == doc);

    // dump∘parse is a fixed point even for non-canonical input.
    Json::ParseResult loose = Json::parse(" { \"a\" : [ 1 , -0.0 , 2e1 ] } ");
    REQUIRE(loose);
    const std::string once = loose.value.dump();
    Json::ParseResult again = Json::parse(once);
    REQUIRE(again);
    CHECK(again.value.dump() == once);
}

TEST_CASE("core.json: parse errors carry origin, 1-based line and column") {
    JsonParseError e = expect_error("{\"a\":1,\n\"b\" 2}");
    CHECK(e.origin == "t.json");
    CHECK(e.line == 2);
    CHECK(e.col == 5);
    CHECK(e.to_string().starts_with("t.json:2:5: "));

    CHECK(expect_error("").line == 1);
    CHECK(expect_error("").col == 1);
    CHECK(expect_error("[1,2,]").col == 6); // trailing comma
    CHECK(expect_error("\"unterminated").col == 1);
    CHECK(expect_error("{\"a\":1} trailing").col == 9); // junk after top-level value
}

TEST_CASE("core.json: strict mode rejects what agents must never write") {
    CHECK_FALSE(Json::parse("{\"a\":1,\"a\":2}")); // duplicate keys
    CHECK_FALSE(Json::parse("[1,2,]"));            // trailing comma
    CHECK_FALSE(Json::parse("// comment\n1"));     // comments
    CHECK_FALSE(Json::parse("'single'"));          // single quotes
    CHECK_FALSE(Json::parse("NaN"));
    CHECK_FALSE(Json::parse("Infinity"));
    CHECK_FALSE(Json::parse("01")); // leading zero
    CHECK_FALSE(Json::parse("+1"));
    CHECK_FALSE(Json::parse("1.")); // bare decimal point
    CHECK_FALSE(Json::parse(".5"));
    CHECK_FALSE(Json::parse("-"));
    CHECK_FALSE(Json::parse("1e"));    // empty exponent
    CHECK_FALSE(Json::parse("1e999")); // double overflow
    CHECK_FALSE(Json::parse("tru"));
    CHECK_FALSE(Json::parse("\"raw\ncontrol\"")); // unescaped control char
    CHECK_FALSE(Json::parse("\"\\ud83d\""));      // lone high surrogate
    CHECK_FALSE(Json::parse("\"\\udc00\""));      // lone low surrogate
    CHECK_FALSE(Json::parse("\"\\q\""));          // unknown escape
    CHECK_FALSE(Json::parse("\"\xc3\x28\""));     // invalid UTF-8 continuation
    CHECK_FALSE(Json::parse("\"\xed\xa0\x80\"")); // UTF-8-encoded surrogate
    CHECK_FALSE(Json::parse("\xff"));             // not JSON at all
    CHECK_FALSE(Json::parse("1 2"));              // two top-level values
    CHECK_FALSE(Json::parse("{\"a\" 1}"));        // missing colon
}

TEST_CASE("core.json: nesting depth is capped deterministically") {
    std::string deep;
    for (int i = 0; i < 200; ++i)
        deep += '[';
    JsonParseError e = expect_error(deep);
    CHECK(e.message.find("depth") != std::string::npos);

    std::string fits(100, '[');
    fits += std::string(100, ']');
    CHECK(Json::parse(fits));
}

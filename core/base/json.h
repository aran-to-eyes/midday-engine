// core/base/json.h — THE JSON implementation for the whole tree. The CLI's
// write-only cli/json.* migrated here at m0-core-primitives (D-BUILD-003);
// there is exactly one JSON reader/writer in first-party code.
//
// Contract:
//   * Values: null, bool, int64, double, string (UTF-8), array, object.
//   * Objects are insertion-ordered pair vectors: serialization order is
//     authoring order, deterministically — no hash-map iteration anywhere.
//   * dump() is deterministic (byte-identical across runs and platforms).
//     Numbers use shortest-round-trip formatting (std::to_chars). Non-finite
//     doubles serialize as null: JSON has no NaN/Inf, and sim code must never
//     produce them (determinism contract, spec section 4.3).
//   * parse() is strict RFC 8259 plus: duplicate object keys rejected,
//     invalid UTF-8 rejected, unescaped control characters rejected, nesting
//     depth capped. Errors are structured returns with origin:line:col —
//     no exceptions escape. `-0` parses as double -0.0 so dump∘parse is a
//     fixed point; integers beyond int64 degrade to double.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace midday::base {

// A strict-parse diagnostic: where and what. line/col are 1-based; col counts
// bytes since the last newline. Convert to the engine Error envelope with
// to_error() (core/base/error.h).
struct JsonParseError {
    std::string message;
    std::string origin; // file path or source label, "<input>" when unnamed
    int line = 1;
    int col = 1;
    std::size_t offset = 0; // byte offset of the offending position

    std::string to_string() const; // "origin:line:col: message"
};

struct JsonParseResult; // Json + optional JsonParseError, defined below

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    // Parse depth cap: deterministic structured error instead of stack overflow.
    static constexpr int kMaxParseDepth = 128;

    Json() : value_(nullptr) {}

    Json(std::nullptr_t) : value_(nullptr) {}

    Json(bool b) : value_(b) {}

    Json(int n) : value_(static_cast<std::int64_t>(n)) {}

    Json(std::int64_t n) : value_(n) {}

    Json(std::uint32_t n) : value_(static_cast<std::int64_t>(n)) {}

    Json(double d) : value_(d) {}

    Json(const char* s) : value_(std::string(s)) {}

    Json(std::string_view s) : value_(std::string(s)) {}

    Json(std::string s) : value_(std::move(s)) {}

    static Json object() { return Json(Object{}); }

    static Json array() { return Json(Array{}); }

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }

    bool is_bool() const { return std::holds_alternative<bool>(value_); }

    bool is_int() const { return std::holds_alternative<std::int64_t>(value_); }

    bool is_double() const { return std::holds_alternative<double>(value_); }

    bool is_number() const { return is_int() || is_double(); }

    bool is_string() const { return std::holds_alternative<std::string>(value_); }

    bool is_array() const { return std::holds_alternative<Array>(value_); }

    bool is_object() const { return std::holds_alternative<Object>(value_); }

    // Typed accessors. Pre: the value holds the requested type (as_double
    // also accepts int); misuse is a programming error, asserted in debug.
    bool as_bool() const;
    std::int64_t as_int() const;
    double as_double() const;
    const std::string& as_string() const;

    // Object: insert or replace in place; insertion order is serialization order.
    Json& set(std::string_view key, Json value);
    // Object: pointer to the value for `key`; nullptr if absent or not an object.
    const Json* find(std::string_view key) const;
    // Array: append.
    Json& push(Json value);

    const Object& items() const { return std::get<Object>(value_); }

    const Array& elements() const { return std::get<Array>(value_); }

    // Compact, deterministic serialization (byte-identical across runs).
    std::string dump() const;

    using ParseResult = JsonParseResult;

    // Strict parse of exactly one JSON document. Never throws; failures come
    // back as a structured JsonParseError with origin:line:col.
    static ParseResult parse(std::string_view text, std::string_view origin = "<input>");

private:
    explicit Json(Array a) : value_(std::move(a)) {}

    explicit Json(Object o) : value_(std::move(o)) {}

    void write(std::string& out) const;

    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> value_;
};

struct JsonParseResult {
    Json value;
    std::optional<JsonParseError> error;

    explicit operator bool() const { return !error.has_value(); }
};

} // namespace midday::base

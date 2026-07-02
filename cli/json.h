// Minimal ordered JSON value + deterministic writer for CLI envelope emission.
//
// Deliberately CLI-local: core JSON IO (with parsing, streaming, and the
// structured Error envelope) arrives at m0-core-primitives in core/base and
// this type migrates onto it. Kept small on purpose — write-only, insertion-
// ordered objects, shortest-round-trip number formatting, no exceptions in
// the serialization path.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace midday::cli {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

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

    bool is_object() const { return std::holds_alternative<Object>(value_); }

    bool is_array() const { return std::holds_alternative<Array>(value_); }

    // Object: insert or replace in place; insertion order is serialization order.
    Json& set(std::string_view key, Json value);
    // Object: pointer to the value for `key`, or nullptr.
    const Json* find(std::string_view key) const;
    // Array: append.
    Json& push(Json value);

    const Object& items() const { return std::get<Object>(value_); }

    // Compact, deterministic serialization (byte-identical across runs).
    std::string dump() const;

private:
    explicit Json(Array a) : value_(std::move(a)) {}

    explicit Json(Object o) : value_(std::move(o)) {}

    void write(std::string& out) const;

    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> value_;
};

} // namespace midday::cli

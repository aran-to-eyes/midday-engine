// Deterministic JSON serialization (see contract in json.h). Migrated from
// cli/json.cpp at m0-core-primitives — the one writer in the tree.
//
// Doubles are emitted via core/base/float_format.h's append_shortest_double
// (vendored dragonbox, shortest round-trip digits, std::to_chars(general)
// style) — byte-identical across every platform/toolchain regardless of the
// standard library's FP support (D-BUILD-015; the macos-14 libc++ has no FP
// to_chars/from_chars at all, and iOS < 16.3 marks the float overload
// unavailable). That formatter is a shared base:: primitive (float_format.h)
// so the float32 path — m1-scene-format's machine emitter — inherits the same
// portability and the same std::to_chars-equivalence proof. Validated against
// std::to_chars over a 64.6M-double cross-check; the core.json byte-pin
// corpus guards it from here on. Integers stay on std::to_chars — integer
// support is universal and locale-free.

#include "core/base/float_format.h"
#include "core/base/json.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace midday::base {
namespace {

void write_escaped(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
            } else {
                out += static_cast<char>(c); // UTF-8 passes through untouched
            }
        }
    }
    out += '"';
}

void write_int(std::string& out, std::int64_t value) {
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
    assert(ec == std::errc{});
    out.append(buf, end);
}

} // namespace

bool Json::as_bool() const {
    assert(is_bool());
    return std::get<bool>(value_);
}

std::int64_t Json::as_int() const {
    assert(is_int());
    return std::get<std::int64_t>(value_);
}

double Json::as_double() const {
    assert(is_number());
    if (const auto* i = std::get_if<std::int64_t>(&value_))
        return static_cast<double>(*i);
    return std::get<double>(value_);
}

const std::string& Json::as_string() const {
    assert(is_string());
    return std::get<std::string>(value_);
}

Json& Json::set(std::string_view key, Json value) {
    assert(is_object());
    auto& object = std::get<Object>(value_);
    for (auto& [k, v] : object) {
        if (k == key) {
            v = std::move(value);
            return *this;
        }
    }
    object.emplace_back(std::string(key), std::move(value));
    return *this;
}

const Json* Json::find(std::string_view key) const {
    if (!is_object())
        return nullptr;
    for (const auto& [k, v] : std::get<Object>(value_)) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

Json& Json::push(Json value) {
    assert(is_array());
    std::get<Array>(value_).emplace_back(std::move(value));
    return *this;
}

std::string Json::dump() const {
    std::string out;
    write(out);
    return out;
}

void Json::write(std::string& out) const {
    struct Writer {
        std::string& out;

        void operator()(std::nullptr_t) const { out += "null"; }

        void operator()(bool b) const { out += b ? "true" : "false"; }

        void operator()(std::int64_t n) const { write_int(out, n); }

        void operator()(double d) const {
            if (std::isfinite(d)) {
                append_shortest_double(out, d);
            } else {
                out += "null"; // JSON has no NaN/Inf (contract in json.h)
            }
        }

        void operator()(const std::string& s) const { write_escaped(out, s); }

        void operator()(const Array& a) const {
            out += '[';
            for (const Json& v : a) {
                if (out.back() != '[')
                    out += ',';
                v.write(out);
            }
            out += ']';
        }

        void operator()(const Object& o) const {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : o) {
                if (!first)
                    out += ',';
                first = false;
                write_escaped(out, k);
                out += ':';
                v.write(out);
            }
            out += '}';
        }
    };

    std::visit(Writer{out}, value_);
}

} // namespace midday::base

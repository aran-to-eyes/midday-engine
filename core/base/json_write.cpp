// Deterministic JSON serialization (see contract in json.h). Migrated from
// cli/json.cpp at m0-core-primitives — the one writer in the tree.

#include "core/base/json.h"

#include <cassert>
#include <charconv>
#include <cmath>

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

template <typename T> void write_number(std::string& out, T value) {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
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

        void operator()(std::int64_t n) const { write_number(out, n); }

        void operator()(double d) const {
            if (std::isfinite(d)) {
                write_number(out, d);
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

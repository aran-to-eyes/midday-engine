#include "cli/json.h"

#include <cassert>
#include <charconv>

namespace midday::cli {
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

        void operator()(double d) const { write_number(out, d); }

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

} // namespace midday::cli

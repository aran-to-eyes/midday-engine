// Strict JSON parser (contract in json.h): RFC 8259 plus duplicate-key,
// UTF-8, and depth strictness. Recursive descent, fail-fast, no exceptions —
// errors come back as structured JsonParseError values with byte-exact
// positions. Determinism: output is a pure function of the input bytes.

#include "core/base/json.h"

#include <charconv>
#include <cstdint>

namespace midday::base {
namespace {

constexpr bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

std::string describe_unexpected(unsigned char c) {
    if (c >= 0x20 && c < 0x7f)
        return std::string("unexpected character '") + static_cast<char>(c) + "'";
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "unexpected byte 0x";
    out += hex[c >> 4];
    out += hex[c & 0xf];
    return out;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct Parser {
    std::string_view text;
    std::size_t pos = 0;
    int depth = 0;
    JsonParseError error;
    bool failed = false;

    Json fail(std::size_t at, std::string message) {
        if (!failed) { // first error wins; everything after is fallout
            failed = true;
            error.message = std::move(message);
            error.offset = at;
        }
        return {};
    }

    [[nodiscard]] bool at_end() const { return pos >= text.size(); }

    [[nodiscard]] char peek() const { return text[pos]; }

    void skip_ws() {
        while (!at_end()) {
            const char c = text[pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++pos;
        }
    }

    Json parse_value() {
        skip_ws();
        if (at_end())
            return fail(pos, "expected a JSON value, got end of input");
        const char c = peek();
        switch (c) {
        case '{':
            return parse_object();
        case '[':
            return parse_array();
        case '"':
            return parse_string_value();
        case 't':
            return parse_literal("true", Json(true));
        case 'f':
            return parse_literal("false", Json(false));
        case 'n':
            return parse_literal("null", Json(nullptr));
        default:
            if (c == '-' || is_digit(c))
                return parse_number();
            return fail(pos, describe_unexpected(static_cast<unsigned char>(c)));
        }
    }

    Json parse_literal(std::string_view word, Json value) {
        if (text.substr(pos, word.size()) != word)
            return fail(pos, "invalid literal, expected '" + std::string(word) + "'");
        pos += word.size();
        return value;
    }

    Json parse_object() {
        if (++depth > Json::kMaxParseDepth)
            return fail(pos, "nesting depth exceeds the maximum of 128");
        ++pos; // '{'
        Json object = Json::object();
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos;
            --depth;
            return object;
        }
        while (true) {
            skip_ws();
            if (at_end() || peek() != '"')
                return fail(pos, "expected an object key string");
            const std::size_t key_at = pos;
            std::string key;
            if (!parse_string_into(key))
                return {};
            if (object.find(key) != nullptr)
                return fail(key_at, "duplicate object key \"" + key + "\"");
            skip_ws();
            if (at_end() || peek() != ':')
                return fail(pos, "expected ':' after object key");
            ++pos;
            Json value = parse_value();
            if (failed)
                return {};
            object.set(key, std::move(value));
            skip_ws();
            if (at_end())
                return fail(pos, "unterminated object, expected ',' or '}'");
            if (peek() == ',') {
                ++pos;
                continue;
            }
            if (peek() == '}') {
                ++pos;
                --depth;
                return object;
            }
            return fail(pos, "expected ',' or '}' in object");
        }
    }

    Json parse_array() {
        if (++depth > Json::kMaxParseDepth)
            return fail(pos, "nesting depth exceeds the maximum of 128");
        ++pos; // '['
        Json array = Json::array();
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos;
            --depth;
            return array;
        }
        while (true) {
            Json value = parse_value();
            if (failed)
                return {};
            array.push(std::move(value));
            skip_ws();
            if (at_end())
                return fail(pos, "unterminated array, expected ',' or ']'");
            if (peek() == ',') {
                ++pos;
                continue;
            }
            if (peek() == ']') {
                ++pos;
                --depth;
                return array;
            }
            return fail(pos, "expected ',' or ']' in array");
        }
    }

    Json parse_string_value() {
        std::string value;
        if (!parse_string_into(value))
            return {};
        return {std::move(value)};
    }

    // Pre: pos is at the opening quote. Post (success): pos is past the
    // closing quote and `out` holds the decoded UTF-8 text.
    bool parse_string_into(std::string& out) {
        const std::size_t start = pos;
        ++pos;
        while (true) {
            if (at_end()) {
                fail(start, "unterminated string");
                return false;
            }
            const auto c = static_cast<unsigned char>(text[pos]);
            if (c == '"') {
                ++pos;
                return true;
            }
            if (c == '\\') {
                if (!parse_escape(out))
                    return false;
                continue;
            }
            if (c < 0x20) {
                fail(pos, "unescaped control character in string");
                return false;
            }
            if (c < 0x80) {
                out += static_cast<char>(c);
                ++pos;
                continue;
            }
            const int len = utf8_sequence_length();
            if (len == 0) {
                fail(pos, "invalid UTF-8 in string");
                return false;
            }
            out.append(text.substr(pos, static_cast<std::size_t>(len)));
            pos += static_cast<std::size_t>(len);
        }
    }

    bool parse_escape(std::string& out) {
        const std::size_t at = pos;
        ++pos; // '\'
        if (at_end()) {
            fail(at, "unterminated escape sequence");
            return false;
        }
        const char c = text[pos++];
        switch (c) {
        case '"':
        case '\\':
        case '/':
            out += c;
            return true;
        case 'n':
            out += '\n';
            return true;
        case 't':
            out += '\t';
            return true;
        case 'r':
            out += '\r';
            return true;
        case 'b':
            out += '\b';
            return true;
        case 'f':
            out += '\f';
            return true;
        case 'u': {
            std::uint32_t hi = 0;
            if (!parse_hex4(at, hi))
                return false;
            if (hi >= 0xDC00 && hi <= 0xDFFF) {
                fail(at, "lone low surrogate escape");
                return false;
            }
            std::uint32_t cp = hi;
            if (hi >= 0xD800 && hi <= 0xDBFF) { // needs a low surrogate partner
                if (pos + 2 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') {
                    fail(at, "unpaired high surrogate escape");
                    return false;
                }
                pos += 2;
                std::uint32_t lo = 0;
                if (!parse_hex4(at, lo))
                    return false;
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    fail(at, "invalid low surrogate in pair");
                    return false;
                }
                cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
            }
            append_utf8(out, cp);
            return true;
        }
        default:
            fail(at, "unknown escape sequence");
            return false;
        }
    }

    bool parse_hex4(std::size_t escape_at, std::uint32_t& value) {
        if (pos + 4 > text.size()) {
            fail(escape_at, "truncated \\u escape");
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[pos + static_cast<std::size_t>(i)];
            std::uint32_t digit = 0;
            if (is_digit(c)) {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a') + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A') + 10;
            } else {
                fail(escape_at, "invalid \\u escape digits");
                return false;
            }
            value = (value << 4) | digit;
        }
        pos += 4;
        return true;
    }

    // Length of the valid UTF-8 sequence at pos (lead byte >= 0x80), or 0.
    // Rejects overlongs, UTF-8-encoded surrogates, and code points > U+10FFFF.
    [[nodiscard]] int utf8_sequence_length() const {
        const auto b0 = static_cast<unsigned char>(text[pos]);
        const auto cont = [&](std::size_t i, unsigned char lo, unsigned char hi) {
            if (pos + i >= text.size())
                return false;
            const auto b = static_cast<unsigned char>(text[pos + i]);
            return b >= lo && b <= hi;
        };
        if (b0 >= 0xC2 && b0 <= 0xDF)
            return cont(1, 0x80, 0xBF) ? 2 : 0;
        if (b0 == 0xE0)
            return cont(1, 0xA0, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
        if ((b0 >= 0xE1 && b0 <= 0xEC) || b0 == 0xEE || b0 == 0xEF)
            return cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
        if (b0 == 0xED) // exclude surrogates U+D800..U+DFFF
            return cont(1, 0x80, 0x9F) && cont(2, 0x80, 0xBF) ? 3 : 0;
        if (b0 == 0xF0)
            return cont(1, 0x90, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        if (b0 >= 0xF1 && b0 <= 0xF3)
            return cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        if (b0 == 0xF4) // cap at U+10FFFF
            return cont(1, 0x80, 0x8F) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        return 0;
    }

    Json parse_number() {
        const std::size_t start = pos;
        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++pos;
        }
        if (at_end() || !is_digit(peek()))
            return fail(start, "invalid number");
        if (peek() == '0') {
            ++pos;
            if (!at_end() && is_digit(peek()))
                return fail(start, "leading zeros are not allowed");
        } else {
            while (!at_end() && is_digit(peek()))
                ++pos;
        }
        bool integral = true;
        if (!at_end() && peek() == '.') {
            integral = false;
            ++pos;
            if (at_end() || !is_digit(peek()))
                return fail(start, "expected digits after decimal point");
            while (!at_end() && is_digit(peek()))
                ++pos;
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            integral = false;
            ++pos;
            if (!at_end() && (peek() == '+' || peek() == '-'))
                ++pos;
            if (at_end() || !is_digit(peek()))
                return fail(start, "expected digits in exponent");
            while (!at_end() && is_digit(peek()))
                ++pos;
        }
        const char* first = text.data() + start;
        const char* last = text.data() + pos;
        if (integral) {
            std::int64_t number = 0;
            const auto [end, ec] = std::from_chars(first, last, number);
            if (ec == std::errc{} && end == last) {
                if (negative && number == 0)
                    return {-0.0}; // keep the sign: dump∘parse stays a fixed point
                return {number};
            }
            // Beyond int64: degrade to double (standard JSON interop).
        }
        double number = 0;
        const auto [end, ec] = std::from_chars(first, last, number);
        if (ec != std::errc{} || end != last)
            return fail(start, "number out of range");
        return {number};
    }
};

} // namespace

std::string JsonParseError::to_string() const {
    return origin + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " + message;
}

Json::ParseResult Json::parse(std::string_view text, std::string_view origin) {
    Parser parser;
    parser.text = text;
    Json value = parser.parse_value();
    if (!parser.failed) {
        parser.skip_ws();
        if (!parser.at_end())
            parser.fail(parser.pos, "unexpected trailing content after JSON document");
    }

    ParseResult result;
    if (parser.failed) {
        parser.error.origin = std::string(origin);
        int line = 1;
        std::size_t line_start = 0;
        const std::size_t stop =
            parser.error.offset < text.size() ? parser.error.offset : text.size();
        for (std::size_t i = 0; i < stop; ++i) {
            if (text[i] == '\n') {
                ++line;
                line_start = i + 1;
            }
        }
        parser.error.line = line;
        parser.error.col = static_cast<int>(parser.error.offset - line_start) + 1;
        result.error = std::move(parser.error);
    } else {
        result.value = std::move(value);
    }
    return result;
}

} // namespace midday::base

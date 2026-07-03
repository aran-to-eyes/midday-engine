#include "core/expr/lexer.h"

#include "fast_float.h"

#include <charconv>
#include <cmath>
#include <system_error>

namespace midday::expr {
namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_char(char c) {
    return is_ident_start(c) || is_digit(c);
}

// The lexer: one pass, tracks line/col per the base::Json parse conventions
// (1-based; col counts bytes since the last newline).
class Lexer {
public:
    Lexer(std::string_view source, std::string_view origin) : src_(source), origin_(origin) {}

    std::optional<Diag> run(std::vector<Token>& out) {
        while (true) {
            skip_whitespace();
            if (diag_)
                return diag_;
            mark();
            if (pos_ >= src_.size()) {
                out.push_back(make(TokenKind::kEnd, 0));
                return std::nullopt;
            }
            Token token = next();
            if (diag_)
                return diag_;
            out.push_back(std::move(token));
        }
    }

private:
    void skip_whitespace() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\n') {
                ++pos_;
                ++line_;
                line_start_ = pos_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++pos_;
            } else {
                return;
            }
        }
    }

    // Remembers the current position as the start of the next token.
    void mark() {
        tok_line_ = line_;
        tok_col_ = static_cast<int>(pos_ - line_start_) + 1;
        tok_offset_ = pos_;
    }

    [[nodiscard]] Token make(TokenKind kind, std::size_t length) const {
        Token token;
        token.kind = kind;
        token.text = src_.substr(tok_offset_, length);
        token.line = tok_line_;
        token.col = tok_col_;
        token.offset = tok_offset_;
        return token;
    }

    Token fail(std::string code, std::string message) {
        diag_ = Diag{.code = std::move(code),
                     .message = std::move(message),
                     .origin = std::string(origin_),
                     .line = tok_line_,
                     .col = tok_col_,
                     .offset = tok_offset_};
        return Token{};
    }

    Token reject_side_effect(std::string message) {
        return fail("expr.side_effect",
                    std::move(message) + " — expressions are side-effect-free by construction");
    }

    [[nodiscard]] char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }

    Token next() {
        const char c = peek();
        if (is_digit(c))
            return number();
        if (is_ident_start(c))
            return identifier();
        if (c == '\'' || c == '"')
            return string_literal(c);
        return punctuation();
    }

    Token punctuation() {
        const char c = peek();
        const char c1 = peek(1);
        switch (c) {
        case '(':
            ++pos_;
            return make(TokenKind::kLParen, 1);
        case ')':
            ++pos_;
            return make(TokenKind::kRParen, 1);
        case ',':
            ++pos_;
            return make(TokenKind::kComma, 1);
        case '.':
            ++pos_;
            return make(TokenKind::kDot, 1);
        case '?':
            ++pos_;
            return make(TokenKind::kQuestion, 1);
        case ':':
            ++pos_;
            return make(TokenKind::kColon, 1);
        case '+':
            if (c1 == '=')
                return reject_side_effect("compound assignment '+=' is not allowed");
            if (c1 == '+')
                return reject_side_effect("increment '++' is not allowed");
            ++pos_;
            return make(TokenKind::kPlus, 1);
        case '-':
            if (c1 == '=')
                return reject_side_effect("compound assignment '-=' is not allowed");
            if (c1 == '-')
                return reject_side_effect("decrement '--' is not allowed");
            ++pos_;
            return make(TokenKind::kMinus, 1);
        case '*':
            if (c1 == '=')
                return reject_side_effect("compound assignment '*=' is not allowed");
            ++pos_;
            return make(TokenKind::kStar, 1);
        case '/':
            if (c1 == '=')
                return reject_side_effect("compound assignment '/=' is not allowed");
            ++pos_;
            return make(TokenKind::kSlash, 1);
        case '%':
            if (c1 == '=')
                return reject_side_effect("compound assignment '%=' is not allowed");
            ++pos_;
            return make(TokenKind::kPercent, 1);
        case ';':
            return reject_side_effect(
                "statement separator ';' is not allowed: one expression, no statements");
        case '=':
            if (c1 == '=') {
                pos_ += 2;
                return make(TokenKind::kEqEq, 2);
            }
            return reject_side_effect("assignment '=' is not allowed (use '==' to compare)");
        case '!':
            if (c1 == '=') {
                pos_ += 2;
                return make(TokenKind::kNe, 2);
            }
            ++pos_;
            return make(TokenKind::kNot, 1);
        case '<':
            if (c1 == '=') {
                pos_ += 2;
                return make(TokenKind::kLe, 2);
            }
            ++pos_;
            return make(TokenKind::kLt, 1);
        case '>':
            if (c1 == '=') {
                pos_ += 2;
                return make(TokenKind::kGe, 2);
            }
            ++pos_;
            return make(TokenKind::kGt, 1);
        case '&':
            if (c1 == '&') {
                pos_ += 2;
                return make(TokenKind::kAnd, 2);
            }
            return fail("expr.parse", "single '&' is not an operator (use '&&' or 'and')");
        case '|':
            if (c1 == '|') {
                pos_ += 2;
                return make(TokenKind::kOr, 2);
            }
            return fail("expr.parse", "single '|' is not an operator (use '||' or 'or')");
        default:
            return fail("expr.parse",
                        "unexpected character '" + std::string(1, c) + "' (byte " +
                            std::to_string(static_cast<unsigned char>(c)) + ")");
        }
    }

    Token identifier() {
        std::size_t end = pos_;
        while (end < src_.size() && is_ident_char(src_[end]))
            ++end;
        const std::string_view word = src_.substr(pos_, end - pos_);
        pos_ = end;
        if (word == "true")
            return make(TokenKind::kTrue, word.size());
        if (word == "false")
            return make(TokenKind::kFalse, word.size());
        if (word == "and")
            return make(TokenKind::kAnd, word.size());
        if (word == "or")
            return make(TokenKind::kOr, word.size());
        if (word == "not")
            return make(TokenKind::kNot, word.size());
        if (word == "if" || word == "else")
            return reject_side_effect("reserved word '" + std::string(word) +
                                      "': the conditional is written 'cond ? a : b'");
        if (word == "while" || word == "for")
            return reject_side_effect("reserved word '" + std::string(word) +
                                      "': the expression language has no loops");
        if (word == "let" || word == "var" || word == "fn" || word == "function" ||
            word == "return")
            return reject_side_effect("reserved word '" + std::string(word) +
                                      "': the expression language has no statements");
        return make(TokenKind::kIdent, word.size());
    }

    Token number() {
        std::size_t end = pos_;
        while (end < src_.size() && is_digit(src_[end]))
            ++end;
        if (src_[pos_] == '0' && end - pos_ > 1)
            return fail("expr.parse", "leading zeros are not allowed in numeric literals");

        bool is_float = false;
        if (end < src_.size() && src_[end] == '.' && end + 1 < src_.size() &&
            is_digit(src_[end + 1])) {
            is_float = true;
            ++end;
            while (end < src_.size() && is_digit(src_[end]))
                ++end;
        }
        if (end < src_.size() && (src_[end] == 'e' || src_[end] == 'E')) {
            std::size_t exp = end + 1;
            if (exp < src_.size() && (src_[exp] == '+' || src_[exp] == '-'))
                ++exp;
            if (exp >= src_.size() || !is_digit(src_[exp]))
                return fail("expr.parse", "exponent requires at least one digit");
            is_float = true;
            end = exp;
            while (end < src_.size() && is_digit(src_[end]))
                ++end;
        }

        const std::string_view text = src_.substr(pos_, end - pos_);
        pos_ = end;
        if (is_float) {
            // Vendored fast_float: correctly rounded float32, locale-free —
            // literal bits are identical on every platform (D-BUILD-015).
            float parsed = 0.0f;
            const auto result =
                fast_float::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
                !std::isfinite(parsed))
                return fail("expr.parse",
                            "float literal '" + std::string(text) + "' is out of range");
            Token token = make(TokenKind::kFloatLit, text.size());
            token.float_value = parsed;
            return token;
        }
        std::int64_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size())
            return fail("expr.parse",
                        "integer literal '" + std::string(text) + "' does not fit int64");
        Token token = make(TokenKind::kIntLit, text.size());
        token.int_value = parsed;
        return token;
    }

    Token string_literal(char quote) {
        std::string decoded;
        std::size_t at = pos_ + 1;
        while (true) {
            if (at >= src_.size())
                return fail("expr.parse", "unterminated string literal");
            const char c = src_[at];
            if (c == quote) {
                ++at;
                break;
            }
            if (c == '\\') {
                if (at + 1 >= src_.size())
                    return fail("expr.parse", "unterminated string literal");
                const char esc = src_[at + 1];
                if (esc == '\\' || esc == '\'' || esc == '"')
                    decoded.push_back(esc);
                else if (esc == 'n')
                    decoded.push_back('\n');
                else if (esc == 't')
                    decoded.push_back('\t');
                else
                    return fail("expr.parse",
                                std::string("unknown escape '\\") + esc +
                                    R"(' (supported: \\ \' \" \n \t))");
                at += 2;
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("expr.parse",
                            "raw control byte in string literal (escape it: \\n, \\t)");
            decoded.push_back(c);
            ++at;
        }
        const std::size_t length = at - pos_;
        pos_ = at;
        Token token = make(TokenKind::kStringLit, length);
        token.string_value = std::move(decoded);
        return token;
    }

    std::string_view src_;
    std::string_view origin_;
    std::size_t pos_ = 0;
    int line_ = 1;
    std::size_t line_start_ = 0;
    int tok_line_ = 1;
    int tok_col_ = 1;
    std::size_t tok_offset_ = 0;
    std::optional<Diag> diag_;
};

} // namespace

std::optional<Diag> lex(std::string_view source, std::string_view origin, std::vector<Token>& out) {
    return Lexer(source, origin).run(out);
}

} // namespace midday::expr

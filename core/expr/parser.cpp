#include "core/expr/parser.h"

#include "core/expr/lexer.h"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace midday::expr {
namespace {

std::string_view describe(const Token& token) {
    return token.kind == TokenKind::kEnd ? std::string_view("end of expression") : token.text;
}

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string_view origin)
        : tokens_(std::move(tokens)), origin_(origin) {}

    ParseResult run() {
        AstPtr root = parse_expr();
        if (!diag_ && peek().kind != TokenKind::kEnd)
            fail(peek(),
                 "expr.parse",
                 "unexpected '" + std::string(describe(peek())) + "' after the expression");
        if (diag_)
            return ParseResult{nullptr, diag_};
        return ParseResult{std::move(root), std::nullopt};
    }

private:
    [[nodiscard]] const Token& peek(std::size_t ahead = 0) const {
        const std::size_t at = pos_ + ahead;
        return at < tokens_.size() ? tokens_[at] : tokens_.back(); // back() is kEnd
    }

    const Token& advance() { return tokens_[pos_ < tokens_.size() - 1 ? pos_++ : pos_]; }

    bool match(TokenKind kind) {
        if (peek().kind != kind)
            return false;
        advance();
        return true;
    }

    void fail(const Token& at, std::string code, std::string message) {
        if (diag_)
            return;
        diag_ = Diag{.code = std::move(code),
                     .message = std::move(message),
                     .origin = std::string(origin_),
                     .line = at.line,
                     .col = at.col,
                     .offset = at.offset};
    }

    AstPtr node(AstKind kind, const Token& at) {
        auto n = std::make_unique<AstNode>();
        n->kind = kind;
        n->line = at.line;
        n->col = at.col;
        n->offset = at.offset;
        return n;
    }

    // RAII-free depth guard: every recursive production calls enter()/leave().
    bool enter(const Token& at) {
        if (++depth_ > kMaxParseDepth) {
            fail(at,
                 "expr.too_complex",
                 "expression nests deeper than " + std::to_string(kMaxParseDepth) + " levels");
            return false;
        }
        return true;
    }

    void leave() { --depth_; }

    AstPtr parse_expr() { return parse_ternary(); }

    AstPtr parse_ternary() {
        if (!enter(peek()))
            return nullptr;
        AstPtr cond = parse_or();
        if (!diag_ && peek().kind == TokenKind::kQuestion) {
            const Token& question = advance();
            AstPtr then_branch = parse_expr();
            if (!diag_ && !match(TokenKind::kColon))
                fail(peek(), "expr.parse", "expected ':' in conditional 'cond ? a : b'");
            AstPtr else_branch = diag_ ? nullptr : parse_ternary();
            if (!diag_) {
                AstPtr n = node(AstKind::kTernary, question);
                n->a = std::move(cond);
                n->b = std::move(then_branch);
                n->c = std::move(else_branch);
                cond = std::move(n);
            }
        }
        leave();
        return diag_ ? nullptr : std::move(cond);
    }

    using Production = AstPtr (Parser::*)();

    // Shared left-associative binary-chain builder.
    AstPtr parse_left(std::initializer_list<TokenKind> ops, Production next) {
        if (!enter(peek()))
            return nullptr;
        AstPtr left = (this->*next)();
        while (!diag_) {
            bool matched = false;
            for (const TokenKind op : ops) {
                if (peek().kind != op)
                    continue;
                const Token& token = advance();
                AstPtr right = (this->*next)();
                if (diag_)
                    break;
                AstPtr n = node(AstKind::kBinary, token);
                n->op = op;
                n->a = std::move(left);
                n->b = std::move(right);
                left = std::move(n);
                matched = true;
                break;
            }
            if (!matched)
                break;
        }
        leave();
        return diag_ ? nullptr : std::move(left);
    }

    AstPtr parse_or() { return parse_left({TokenKind::kOr}, &Parser::parse_and); }

    AstPtr parse_and() { return parse_left({TokenKind::kAnd}, &Parser::parse_equality); }

    AstPtr parse_equality() {
        return parse_left({TokenKind::kEqEq, TokenKind::kNe}, &Parser::parse_relational);
    }

    AstPtr parse_relational() {
        return parse_left({TokenKind::kLt, TokenKind::kLe, TokenKind::kGt, TokenKind::kGe},
                          &Parser::parse_additive);
    }

    AstPtr parse_additive() {
        return parse_left({TokenKind::kPlus, TokenKind::kMinus}, &Parser::parse_multiplicative);
    }

    AstPtr parse_multiplicative() {
        return parse_left({TokenKind::kStar, TokenKind::kSlash, TokenKind::kPercent},
                          &Parser::parse_unary);
    }

    AstPtr parse_unary() {
        const TokenKind kind = peek().kind;
        if (kind == TokenKind::kMinus || kind == TokenKind::kNot) {
            if (!enter(peek()))
                return nullptr;
            const Token& token = advance();
            AstPtr operand = parse_unary();
            leave();
            if (diag_)
                return nullptr;
            AstPtr n = node(AstKind::kUnary, token);
            n->op = kind;
            n->a = std::move(operand);
            return n;
        }
        return parse_postfix();
    }

    AstPtr parse_postfix() {
        AstPtr base = parse_primary();
        while (!diag_ && peek().kind == TokenKind::kDot) {
            const Token& dot = advance();
            if (peek().kind != TokenKind::kIdent) {
                fail(peek(), "expr.parse", "expected a component name after '.'");
                return nullptr;
            }
            const Token& ident = advance();
            AstPtr n = node(AstKind::kMember, dot);
            n->a = std::move(base);
            n->member = std::string(ident.text);
            base = std::move(n);
        }
        return diag_ ? nullptr : std::move(base);
    }

    AstPtr parse_primary() {
        const Token& token = peek();
        switch (token.kind) {
        case TokenKind::kIntLit: {
            advance();
            AstPtr n = node(AstKind::kLiteral, token);
            n->literal = Value::of_int(token.int_value);
            return n;
        }
        case TokenKind::kFloatLit: {
            advance();
            AstPtr n = node(AstKind::kLiteral, token);
            n->literal = Value::of_float(token.float_value);
            return n;
        }
        case TokenKind::kStringLit: {
            advance();
            AstPtr n = node(AstKind::kLiteral, token);
            n->string_storage = token.string_value;
            n->literal = Value::of_string(n->string_storage);
            return n;
        }
        case TokenKind::kTrue:
        case TokenKind::kFalse: {
            advance();
            AstPtr n = node(AstKind::kLiteral, token);
            n->literal = Value::of_bool(token.kind == TokenKind::kTrue);
            return n;
        }
        case TokenKind::kLParen: {
            if (!enter(token))
                return nullptr;
            advance();
            AstPtr inner = parse_expr();
            if (!diag_ && !match(TokenKind::kRParen))
                fail(peek(), "expr.parse", "expected ')'");
            leave();
            return diag_ ? nullptr : std::move(inner);
        }
        case TokenKind::kIdent:
            return parse_path_or_call();
        default:
            fail(token,
                 "expr.parse",
                 "expected an expression, found '" + std::string(describe(token)) + "'");
            return nullptr;
        }
    }

    // path := ident ('.' ident)*; a following '(' makes it a call. The '.'
    // is consumed here only while it continues a pure identifier path —
    // member access on call results is parse_postfix's job.
    AstPtr parse_path_or_call() {
        const Token& first = advance();
        std::vector<std::string> segments;
        segments.emplace_back(first.text);
        while (peek().kind == TokenKind::kDot && peek(1).kind == TokenKind::kIdent) {
            advance(); // '.'
            segments.emplace_back(advance().text);
        }
        if (peek().kind != TokenKind::kLParen) {
            AstPtr n = node(AstKind::kPath, first);
            n->segments = std::move(segments);
            return n;
        }
        if (!enter(peek()))
            return nullptr;
        advance(); // '('
        AstPtr n = node(AstKind::kCall, first);
        n->segments = std::move(segments);
        if (!match(TokenKind::kRParen)) {
            while (true) {
                AstPtr arg = parse_expr();
                if (diag_)
                    break;
                n->args.push_back(std::move(arg));
                if (match(TokenKind::kComma))
                    continue;
                if (!match(TokenKind::kRParen))
                    fail(peek(), "expr.parse", "expected ',' or ')' in the argument list");
                break;
            }
        }
        leave();
        return diag_ ? nullptr : std::move(n);
    }

    std::vector<Token> tokens_;
    std::string_view origin_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    std::optional<Diag> diag_;
};

} // namespace

ParseResult parse(std::string_view source, std::string_view origin) {
    std::vector<Token> tokens;
    if (auto diag = lex(source, origin, tokens))
        return ParseResult{nullptr, std::move(diag)};
    return Parser(std::move(tokens), origin).run();
}

} // namespace midday::expr

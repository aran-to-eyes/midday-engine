// core/expr/ast.h — the parse tree, annotated in place by the typechecker.
//
// The AST exists only during compilation (parse -> typecheck -> codegen);
// nothing at eval time touches it. One node struct with a kind switch keeps
// the tree walkable in a single visitor per pass.
//
// There is deliberately NO assignment, statement, loop, lambda, or
// user-function node kind: side-effecting shapes are unrepresentable — the
// lexer/parser rejected them with structured diagnostics before a tree exists.

#pragma once

#include "core/expr/lexer.h"
#include "core/expr/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace midday::expr {

struct AstNode;
using AstPtr = std::unique_ptr<AstNode>;

enum class AstKind : std::uint8_t {
    kLiteral, // int/float/bool/string literal (string may fold to name)
    kPath,    // identifier path: `speed`, `health.current`, `pos.x`
    kUnary,   // - ! not
    kBinary,  // arithmetic, comparison, logic
    kTernary, // cond ? a : b
    kCall,    // function(args...)
    kMember,  // lane access on a non-path expression: normalize(v).x
};

struct AstNode {
    AstKind kind;
    // Location of the node's primary token (diagnostics).
    int line = 1;
    int col = 1;
    std::size_t offset = 0;

    // kLiteral. Strings: `literal` views `string_storage` (decoded escapes).
    Value literal;
    std::string string_storage;

    // kPath / kCall (callee): identifier segments, e.g. {"health","current"}.
    std::vector<std::string> segments;

    // kUnary / kBinary operator.
    TokenKind op = TokenKind::kEnd;

    // Children: a (unary/binary/ternary/member base), b (binary/ternary), c (ternary).
    AstPtr a;
    AstPtr b;
    AstPtr c;
    std::vector<AstPtr> args; // kCall

    // kMember: the accessed component name ("x".."w").
    std::string member;

    // ---- typecheck annotations (set by typecheck.cpp, read by codegen) ----
    ValueType type = ValueType::kBool; // the node's static type
    bool coerce_to_float = false;      // emit int->float right after this node
    std::int16_t slot = -1;            // kPath: variable slot in the EnvSpec
    std::int8_t lane = -1;             // kPath / kMember: component index
    std::int16_t builtin = -1;         // kCall: index into the builtin table
};

// The type the node leaves on the eval stack (after any int->float coercion).
inline ValueType effective_type(const AstNode& node) {
    return node.coerce_to_float ? ValueType::kFloat : node.type;
}

} // namespace midday::expr

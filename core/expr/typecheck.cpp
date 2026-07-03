#include "core/expr/typecheck.h"

#include "core/base/name.h"
#include "core/expr/functions.h"

#include <string>

namespace midday::expr {
namespace {

bool is_numeric(ValueType type) {
    return type == ValueType::kInt || type == ValueType::kFloat;
}

bool is_vector(ValueType type) {
    return type == ValueType::kVec2 || type == ValueType::kVec3 || type == ValueType::kVec4;
}

int lane_index(std::string_view member) {
    if (member.size() != 1)
        return -1;
    switch (member[0]) {
    case 'x':
        return 0;
    case 'y':
        return 1;
    case 'z':
        return 2;
    case 'w':
        return 3;
    default:
        return -1;
    }
}

std::string join(const std::vector<std::string>& segments, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0)
            out += '.';
        out += segments[i];
    }
    return out;
}

std::string spell(std::string_view op_text) {
    return "operator '" + std::string(op_text) + "'";
}

std::string_view op_text(TokenKind op) {
    switch (op) {
    case TokenKind::kPlus:
        return "+";
    case TokenKind::kMinus:
        return "-";
    case TokenKind::kStar:
        return "*";
    case TokenKind::kSlash:
        return "/";
    case TokenKind::kPercent:
        return "%";
    case TokenKind::kLt:
        return "<";
    case TokenKind::kLe:
        return "<=";
    case TokenKind::kGt:
        return ">";
    case TokenKind::kGe:
        return ">=";
    case TokenKind::kEqEq:
        return "==";
    case TokenKind::kNe:
        return "!=";
    case TokenKind::kAnd:
        return "and";
    case TokenKind::kOr:
        return "or";
    case TokenKind::kNot:
        return "not";
    default:
        return "?";
    }
}

class Checker {
public:
    Checker(const EnvSpec& env, std::string_view origin) : env_(env), origin_(origin) {}

    std::optional<Diag> run(AstNode& root) {
        check(root);
        return diag_;
    }

private:
    bool fail(const AstNode& at, std::string code, std::string message) {
        if (!diag_)
            diag_ = Diag{.code = std::move(code),
                         .message = std::move(message),
                         .origin = std::string(origin_),
                         .line = at.line,
                         .col = at.col,
                         .offset = at.offset};
        return false;
    }

    bool type_error(const AstNode& at, std::string message) {
        return fail(at, "expr.type", std::move(message));
    }

    // Applies the single implicit coercion (int -> float) to a child node.
    static void coerce_to_float(AstNode& node) {
        if (effective_type(node) == ValueType::kInt)
            node.coerce_to_float = true;
    }

    // Unifies two numeric children (int/float) to a common type, inserting
    // the int->float coercion where needed. Pre: both are numeric.
    static ValueType unify_numeric(AstNode& a, AstNode& b) {
        if (effective_type(a) == ValueType::kInt && effective_type(b) == ValueType::kInt)
            return ValueType::kInt;
        coerce_to_float(a);
        coerce_to_float(b);
        return ValueType::kFloat;
    }

    // Folds a string LITERAL into a name constant (compile-time interning).
    static bool fold_string_literal_to_name(AstNode& node) {
        if (node.kind != AstKind::kLiteral || node.type != ValueType::kString)
            return false;
        node.literal = Value::of_name(base::Name(node.string_storage));
        node.type = ValueType::kName;
        return true;
    }

    bool check(AstNode& node) {
        switch (node.kind) {
        case AstKind::kLiteral:
            node.type = node.literal.type;
            return true;
        case AstKind::kPath:
            return check_path(node);
        case AstKind::kMember:
            return check_member(node);
        case AstKind::kUnary:
            return check_unary(node);
        case AstKind::kBinary:
            return check_binary(node);
        case AstKind::kTernary:
            return check_ternary(node);
        case AstKind::kCall:
            return check_call(node);
        }
        return type_error(node, "internal: unknown AST node kind");
    }

    bool check_lane(AstNode& node, ValueType base_type, const std::string& member) {
        if (!is_vector(base_type) && base_type != ValueType::kQuat)
            return type_error(node,
                              "type '" + std::string(to_string(base_type)) +
                                  "' has no components (accessed '." + member + "')");
        const int lane = lane_index(member);
        if (lane < 0 || lane >= lane_count(base_type))
            return type_error(node,
                              "type '" + std::string(to_string(base_type)) +
                                  "' has no component '" + member + "'");
        node.lane = static_cast<std::int8_t>(lane);
        node.type = ValueType::kFloat;
        return true;
    }

    bool check_path(AstNode& node) {
        // Longest declared prefix wins; remaining segments are components.
        for (std::size_t count = node.segments.size(); count > 0; --count) {
            const std::string candidate = join(node.segments, count);
            const int slot = env_.find(candidate);
            if (slot < 0)
                continue;
            node.slot = static_cast<std::int16_t>(slot);
            const ValueType var_type = env_.vars()[static_cast<std::size_t>(slot)].type;
            const std::size_t remaining = node.segments.size() - count;
            if (remaining == 0) {
                node.type = var_type;
                return true;
            }
            if (remaining > 1)
                return type_error(node,
                                  "'" + join(node.segments, count + 1) +
                                      "' is a scalar component — nothing further to access");
            return check_lane(node, var_type, node.segments[count]);
        }
        const std::string path = join(node.segments, node.segments.size());
        std::string message = "unknown variable '" + path + "'";
        for (const EnvSpec::Var& var : env_.vars()) {
            if (var.name.size() > path.size() && var.name.starts_with(path) &&
                var.name[path.size()] == '.') {
                message += " (did you mean '" + var.name + "'?)";
                break;
            }
        }
        return fail(node, "expr.unknown_variable", std::move(message));
    }

    bool check_member(AstNode& node) {
        if (!check(*node.a))
            return false;
        return check_lane(node, effective_type(*node.a), node.member);
    }

    bool check_unary(AstNode& node) {
        if (!check(*node.a))
            return false;
        const ValueType operand = effective_type(*node.a);
        if (node.op == TokenKind::kNot) {
            if (operand != ValueType::kBool)
                return type_error(
                    node, "'not' expects bool, got '" + std::string(to_string(operand)) + "'");
            node.type = ValueType::kBool;
            return true;
        }
        // Unary minus.
        if (is_numeric(operand) || is_vector(operand)) {
            node.type = operand;
            return true;
        }
        return type_error(node,
                          "unary '-' is not defined for '" + std::string(to_string(operand)) + "'");
    }

    bool check_binary(AstNode& node) {
        if (!check(*node.a) || !check(*node.b))
            return false;
        AstNode& a = *node.a;
        AstNode& b = *node.b;
        const ValueType ta = effective_type(a);
        const ValueType tb = effective_type(b);
        const auto mismatch = [&]() {
            return type_error(node,
                              spell(op_text(node.op)) + " is not defined for '" +
                                  std::string(to_string(ta)) + "' and '" +
                                  std::string(to_string(tb)) + "'");
        };
        switch (node.op) {
        case TokenKind::kAnd:
        case TokenKind::kOr:
            if (ta != ValueType::kBool || tb != ValueType::kBool)
                return mismatch();
            node.type = ValueType::kBool;
            return true;
        case TokenKind::kPlus:
        case TokenKind::kMinus:
            if (is_numeric(ta) && is_numeric(tb)) {
                node.type = unify_numeric(a, b);
                return true;
            }
            if (is_vector(ta) && ta == tb) {
                node.type = ta;
                return true;
            }
            return mismatch();
        case TokenKind::kStar:
            if (is_numeric(ta) && is_numeric(tb)) {
                node.type = unify_numeric(a, b);
                return true;
            }
            if (is_vector(ta) && ta == tb) {
                node.type = ta; // component-wise (Hadamard), matching core/math
                return true;
            }
            if (is_vector(ta) && is_numeric(tb)) {
                coerce_to_float(b);
                node.type = ta;
                return true;
            }
            if (is_numeric(ta) && is_vector(tb)) {
                coerce_to_float(a);
                node.type = tb;
                return true;
            }
            if (ta == ValueType::kQuat && tb == ValueType::kQuat) {
                node.type = ValueType::kQuat;
                return true;
            }
            return mismatch();
        case TokenKind::kSlash:
            if (is_numeric(ta) && is_numeric(tb)) {
                node.type = unify_numeric(a, b);
                return true;
            }
            if (is_vector(ta) && is_numeric(tb)) {
                coerce_to_float(b);
                node.type = ta;
                return true;
            }
            return mismatch();
        case TokenKind::kPercent:
            if (ta == ValueType::kInt && tb == ValueType::kInt) {
                node.type = ValueType::kInt;
                return true;
            }
            return type_error(node, "'%' is integer-only (int % int); floats use fract()/floor()");
        case TokenKind::kLt:
        case TokenKind::kLe:
        case TokenKind::kGt:
        case TokenKind::kGe:
            if (!is_numeric(ta) || !is_numeric(tb))
                return mismatch();
            unify_numeric(a, b);
            node.type = ValueType::kBool;
            return true;
        case TokenKind::kEqEq:
        case TokenKind::kNe:
            return check_equality(node, a, b);
        default:
            return type_error(node, "internal: unknown binary operator");
        }
    }

    bool check_equality(AstNode& node, AstNode& a, AstNode& b) {
        node.type = ValueType::kBool;
        if (is_numeric(effective_type(a)) && is_numeric(effective_type(b))) {
            unify_numeric(a, b);
            return true;
        }
        // A string literal against a name folds to a name constant at
        // compile time (interning now, so eval never allocates).
        if (effective_type(a) == ValueType::kName && effective_type(b) == ValueType::kString &&
            fold_string_literal_to_name(b))
            return true;
        if (effective_type(b) == ValueType::kName && effective_type(a) == ValueType::kString &&
            fold_string_literal_to_name(a))
            return true;
        if (effective_type(a) == effective_type(b))
            return true;
        if (effective_type(a) == ValueType::kName || effective_type(b) == ValueType::kName)
            return type_error(node,
                              spell(op_text(node.op)) +
                                  ": a name compares against a name or a string LITERAL "
                                  "(string variables would intern at eval time)");
        return type_error(node,
                          spell(op_text(node.op)) + " is not defined for '" +
                              std::string(to_string(effective_type(a))) + "' and '" +
                              std::string(to_string(effective_type(b))) + "'");
    }

    bool check_ternary(AstNode& node) {
        if (!check(*node.a) || !check(*node.b) || !check(*node.c))
            return false;
        if (effective_type(*node.a) != ValueType::kBool)
            return type_error(*node.a,
                              "the condition of '?:' expects bool, got '" +
                                  std::string(to_string(effective_type(*node.a))) + "'");
        AstNode& b = *node.b;
        AstNode& c = *node.c;
        if (is_numeric(effective_type(b)) && is_numeric(effective_type(c))) {
            node.type = unify_numeric(b, c);
            return true;
        }
        if (effective_type(b) == effective_type(c)) {
            node.type = effective_type(b);
            return true;
        }
        return type_error(node,
                          "'?:' branches must have one type, got '" +
                              std::string(to_string(effective_type(b))) + "' and '" +
                              std::string(to_string(effective_type(c))) + "'");
    }

    bool check_call(AstNode& node) {
        const std::string callee = join(node.segments, node.segments.size());
        if (node.segments.size() > 1)
            return fail(node,
                        "expr.unknown_function",
                        "unknown function '" + callee + "' (function names are not dotted)");
        const int index = find_builtin(callee);
        if (index < 0)
            return fail(node, "expr.unknown_function", "unknown function '" + callee + "'");
        const Builtin& builtin = builtins()[static_cast<std::size_t>(index)];
        if (node.args.size() != builtin.arity)
            return fail(node,
                        "expr.arity",
                        "'" + callee + "' expects " + std::to_string(builtin.arity) +
                            " argument(s), got " + std::to_string(node.args.size()));
        for (std::size_t i = 0; i < node.args.size(); ++i) {
            AstNode& arg = *node.args[i];
            if (!check(arg))
                return false;
            const ValueType expected = builtin.params[i];
            if (effective_type(arg) == expected)
                continue;
            if (expected == ValueType::kFloat && effective_type(arg) == ValueType::kInt) {
                coerce_to_float(arg);
                continue;
            }
            if (expected == ValueType::kName && effective_type(arg) == ValueType::kString &&
                fold_string_literal_to_name(arg))
                continue;
            return type_error(arg,
                              "argument " + std::to_string(i + 1) + " ('" +
                                  std::string(builtin.param_names[i]) + "') of '" + callee +
                                  "' expects '" + std::string(to_string(expected)) + "', got '" +
                                  std::string(to_string(effective_type(arg))) + "'");
        }
        node.builtin = static_cast<std::int16_t>(index);
        node.type = builtin.returns;
        return true;
    }

    const EnvSpec& env_;
    std::string_view origin_;
    std::optional<Diag> diag_;
};

} // namespace

std::optional<Diag> typecheck(AstNode& root, const EnvSpec& env, std::string_view origin) {
    return Checker(env, origin).run(root);
}

} // namespace midday::expr

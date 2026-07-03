#include "core/expr/codegen.h"

#include "core/expr/functions.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace midday::expr {
namespace {

class Codegen {
public:
    Codegen(const EnvSpec& env, std::string_view origin) : env_(env), origin_(origin) {}

    CodegenResult run(const AstNode& root) {
        emit_node(root);
        if (static_cast<int>(parts_.code.size()) > kMaxInstructions)
            return too_complex(root,
                               "compiles to more than " + std::to_string(kMaxInstructions) +
                                   " instructions");
        if (max_depth_ > kMaxEvalStack)
            return too_complex(root,
                               "needs more than " + std::to_string(kMaxEvalStack) +
                                   " evaluation stack slots");
        parts_.result_type = effective_type(root);
        parts_.max_stack = max_depth_;
        parts_.var_count = static_cast<int>(env_.vars().size());
        return CodegenResult{Program(std::move(parts_)), std::nullopt};
    }

private:
    CodegenResult too_complex(const AstNode& root, std::string what) {
        return CodegenResult{std::nullopt,
                             Diag{.code = "expr.too_complex",
                                  .message = "expression " + std::move(what),
                                  .origin = std::string(origin_),
                                  .line = root.line,
                                  .col = root.col,
                                  .offset = root.offset}};
    }

    // ---- emission helpers -------------------------------------------------

    std::size_t emit(Op op, int lanes = 0, int imm = 0) {
        parts_.code.push_back(
            Instr{op, static_cast<std::uint8_t>(lanes), static_cast<std::int16_t>(imm)});
        return parts_.code.size() - 1;
    }

    void patch(std::size_t jump_at) {
        parts_.code[jump_at].imm = static_cast<std::int16_t>(parts_.code.size() - jump_at - 1);
    }

    void push(int n = 1) {
        depth_ += n;
        if (depth_ > max_depth_)
            max_depth_ = depth_;
    }

    void pop(int n = 1) { depth_ -= n; }

    int constant(Value value) {
        parts_.constants.push_back(value);
        return static_cast<int>(parts_.constants.size() - 1);
    }

    // String constants copy their bytes into program-owned storage
    // (unique_ptr: addresses survive Program moves).
    int string_constant(std::string_view text) {
        parts_.strings.push_back(std::make_unique<std::string>(text));
        return constant(Value::of_string(*parts_.strings.back()));
    }

    // ---- the walk ----------------------------------------------------------

    void emit_node(const AstNode& node) {
        switch (node.kind) {
        case AstKind::kLiteral:
            if (node.coerce_to_float && node.literal.type == ValueType::kInt) {
                // Fold the coercion into the constant: same IEEE
                // round-to-nearest conversion, done once at compile time.
                emit(Op::kPushConst,
                     0,
                     constant(Value::of_float(static_cast<float>(node.literal.u.i))));
                push();
                return;
            }
            emit(Op::kPushConst,
                 0,
                 node.literal.type == ValueType::kString
                     ? string_constant(node.literal.as_string_view())
                     : constant(node.literal));
            push();
            break;
        case AstKind::kPath:
            emit(Op::kPushVar, 0, node.slot);
            push();
            if (node.lane >= 0)
                emit(Op::kLane, 0, node.lane);
            break;
        case AstKind::kMember:
            emit_node(*node.a);
            emit(Op::kLane, 0, node.lane);
            break;
        case AstKind::kUnary:
            emit_unary(node);
            break;
        case AstKind::kBinary:
            emit_binary(node);
            break;
        case AstKind::kTernary:
            emit_ternary(node);
            break;
        case AstKind::kCall:
            emit_call(node);
            break;
        }
        if (node.coerce_to_float)
            emit(Op::kIntToFloat);
    }

    void emit_unary(const AstNode& node) {
        emit_node(*node.a);
        const ValueType operand = effective_type(*node.a);
        if (node.op == TokenKind::kNot)
            emit(Op::kNotB);
        else if (operand == ValueType::kInt)
            emit(Op::kNegI);
        else if (operand == ValueType::kFloat)
            emit(Op::kNegF);
        else
            emit(Op::kNegV, lane_count(operand));
    }

    void emit_binary(const AstNode& node) {
        if (node.op == TokenKind::kAnd || node.op == TokenKind::kOr) {
            // Short-circuit: the right side is skipped (not just ignored) —
            // it can neither cost time nor fault.
            emit_node(*node.a);
            const std::size_t jump_at =
                emit(node.op == TokenKind::kAnd ? Op::kJumpIfFalseKeep : Op::kJumpIfTrueKeep);
            emit(Op::kPop);
            pop();
            emit_node(*node.b);
            patch(jump_at);
            return;
        }
        emit_node(*node.a);
        emit_node(*node.b);
        emit_binary_op(node);
        pop(); // two operands became one result
    }

    void emit_binary_op(const AstNode& node) {
        const ValueType ta = effective_type(*node.a);
        const ValueType tb = effective_type(*node.b);
        const int lanes = lane_count(ta) != 0 ? lane_count(ta) : lane_count(tb);
        const bool ints = ta == ValueType::kInt; // unified: tb matches for scalars
        switch (node.op) {
        case TokenKind::kPlus:
            emit(lanes != 0 ? Op::kAddV : (ints ? Op::kAddI : Op::kAddF), lanes);
            return;
        case TokenKind::kMinus:
            emit(lanes != 0 ? Op::kSubV : (ints ? Op::kSubI : Op::kSubF), lanes);
            return;
        case TokenKind::kStar:
            emit_multiply(node, ta, tb);
            return;
        case TokenKind::kSlash:
            if (lane_count(ta) != 0)
                emit(Op::kDivVS, lane_count(ta));
            else
                emit(ints ? Op::kDivI : Op::kDivF);
            return;
        case TokenKind::kPercent:
            emit(Op::kModI);
            return;
        case TokenKind::kLt:
            emit(ints ? Op::kLtI : Op::kLtF);
            return;
        case TokenKind::kLe:
            emit(ints ? Op::kLeI : Op::kLeF);
            return;
        case TokenKind::kGt:
            emit(ints ? Op::kGtI : Op::kGtF);
            return;
        case TokenKind::kGe:
            emit(ints ? Op::kGeI : Op::kGeF);
            return;
        case TokenKind::kEqEq:
        case TokenKind::kNe:
            emit_equality(node, ta);
            return;
        default:
            return; // unreachable: checker admitted only the cases above
        }
    }

    void emit_multiply(const AstNode& node, ValueType ta, ValueType tb) {
        if (ta == ValueType::kQuat) {
            emit(Op::kMulQ);
            return;
        }
        if (lane_count(ta) != 0 && lane_count(tb) != 0) {
            emit(Op::kMulVV, lane_count(ta));
            return;
        }
        if (lane_count(ta) != 0) {
            emit(Op::kMulVS, lane_count(ta));
            return;
        }
        if (lane_count(tb) != 0) {
            emit(Op::kMulSV, lane_count(tb));
            return;
        }
        emit(effective_type(*node.a) == ValueType::kInt ? Op::kMulI : Op::kMulF);
    }

    void emit_equality(const AstNode& node, ValueType operand_type) {
        const bool eq = node.op == TokenKind::kEqEq;
        switch (operand_type) {
        case ValueType::kBool:
            emit(eq ? Op::kEqB : Op::kNeB);
            return;
        case ValueType::kInt:
            emit(eq ? Op::kEqI : Op::kNeI);
            return;
        case ValueType::kFloat:
            emit(eq ? Op::kEqF : Op::kNeF);
            return;
        case ValueType::kString:
            emit(eq ? Op::kEqS : Op::kNeS);
            return;
        case ValueType::kName:
            emit(eq ? Op::kEqN : Op::kNeN);
            return;
        default:
            emit(eq ? Op::kEqV : Op::kNeV, lane_count(operand_type));
            return;
        }
    }

    void emit_ternary(const AstNode& node) {
        emit_node(*node.a);
        const std::size_t to_else = emit(Op::kJumpIfFalsePop);
        pop(); // the condition
        const int depth_at_branch = depth_;
        emit_node(*node.b);
        const std::size_t to_end = emit(Op::kJump);
        patch(to_else);
        depth_ = depth_at_branch; // the else arm starts where the then arm did
        emit_node(*node.c);
        patch(to_end);
    }

    void emit_call(const AstNode& node) {
        for (const AstPtr& arg : node.args)
            emit_node(*arg);
        const Builtin& builtin = builtins()[static_cast<std::size_t>(node.builtin)];
        if (builtin.fn == nullptr) {
            // Conversion intrinsics compile to (checked) opcodes.
            emit(builtin.returns == ValueType::kInt ? Op::kFloatToInt : Op::kIntToFloat);
        } else {
            emit(Op::kCallNative, 0, node.builtin);
        }
        pop(builtin.arity - 1); // arity operands became one result
    }

    const EnvSpec& env_;
    std::string_view origin_;
    detail::ProgramParts parts_;
    int depth_ = 0;
    int max_depth_ = 0;
};

} // namespace

CodegenResult codegen(const AstNode& root, const EnvSpec& env, std::string_view origin) {
    return Codegen(env, origin).run(root);
}

} // namespace midday::expr

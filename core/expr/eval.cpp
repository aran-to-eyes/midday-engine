// core/expr/eval.cpp — the evaluator: one linear dispatch loop over the
// compiled program. noexcept, zero heap allocation, no recursion; the value
// stack is a fixed array on the C++ stack (cost model in program.h).
//
// Jumps are forward-only by construction (no loop opcodes exist), so the
// loop terminates in at most code.size() steps — no fuel counter needed.

#include "core/expr/functions.h"
#include "core/expr/program.h"

#include <cstring>

namespace midday::expr {
namespace {

// Two's-complement wrapping arithmetic: well-defined via uint64 (C++20).
std::int64_t wrap_add(std::int64_t a, std::int64_t b) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

std::int64_t wrap_sub(std::int64_t a, std::int64_t b) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}

std::int64_t wrap_mul(std::int64_t a, std::int64_t b) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

std::int64_t wrap_neg(std::int64_t a) noexcept {
    return static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(a));
}

bool str_eq(const Value& a, const Value& b) noexcept {
    return a.u.s.size == b.u.s.size &&
           (a.u.s.size == 0 ||
            std::memcmp(a.u.s.data, b.u.s.data, static_cast<std::size_t>(a.u.s.size)) == 0);
}

bool lanes_eq(const Value& a, const Value& b, int lanes) noexcept {
    for (int i = 0; i < lanes; ++i)
        if (a.u.lanes[i] != b.u.lanes[i]) // IEEE: -0 == 0, NaN != NaN
            return false;
    return true;
}

// int64 range bounds, exact as doubles: -2^63 and 2^63.
constexpr double kInt64Lo = -9223372036854775808.0;
constexpr double kInt64Hi = 9223372036854775808.0;

} // namespace

base::Error to_error(EvalStatus status) {
    switch (status) {
    case EvalStatus::kDivZero:
        return base::Error{.code = "expr.div_zero",
                           .message = "integer division or modulo by zero"};
    case EvalStatus::kIntRange:
        return base::Error{.code = "expr.int_range",
                           .message = "int() input is NaN or outside int64 range"};
    case EvalStatus::kOk:
        break;
    }
    return base::Error{.code = "expr.ok", .message = "no error"};
}

EvalResult Program::eval(std::span<const Value> vars) const noexcept {
    Value stack[kMaxEvalStack];
    int sp = 0;
    const std::span<const Instr> code(parts_.code);
    const std::span<const Value> consts(parts_.constants);

    std::size_t ip = 0;
    while (ip < code.size()) {
        const Instr in = code[ip++];
        // Operand indices; only dereferenced by opcodes that have operands.
        const int t = sp - 1; // top
        const int u = sp - 2; // under top
        switch (in.op) {
        case Op::kPushConst:
            stack[sp++] = consts[static_cast<std::size_t>(in.imm)];
            break;
        case Op::kPushVar:
            stack[sp++] = vars[static_cast<std::size_t>(in.imm)];
            break;
        case Op::kPop:
            --sp;
            break;
        case Op::kJump:
            ip += static_cast<std::size_t>(in.imm);
            break;
        case Op::kJumpIfFalsePop:
            if (!stack[t].u.b)
                ip += static_cast<std::size_t>(in.imm);
            --sp;
            break;
        case Op::kJumpIfFalseKeep:
            if (!stack[t].u.b)
                ip += static_cast<std::size_t>(in.imm);
            break;
        case Op::kJumpIfTrueKeep:
            if (stack[t].u.b)
                ip += static_cast<std::size_t>(in.imm);
            break;
        case Op::kNegI:
            stack[t].u.i = wrap_neg(stack[t].u.i);
            break;
        case Op::kNegF:
            stack[t].u.f = -stack[t].u.f;
            break;
        case Op::kNegV:
            for (int i = 0; i < in.lanes; ++i)
                stack[t].u.lanes[i] = -stack[t].u.lanes[i];
            break;
        case Op::kNotB:
            stack[t].u.b = !stack[t].u.b;
            break;
        case Op::kAddI:
            stack[u].u.i = wrap_add(stack[u].u.i, stack[t].u.i);
            --sp;
            break;
        case Op::kAddF:
            stack[u].u.f = stack[u].u.f + stack[t].u.f;
            --sp;
            break;
        case Op::kAddV:
            for (int i = 0; i < in.lanes; ++i)
                stack[u].u.lanes[i] = stack[u].u.lanes[i] + stack[t].u.lanes[i];
            --sp;
            break;
        case Op::kSubI:
            stack[u].u.i = wrap_sub(stack[u].u.i, stack[t].u.i);
            --sp;
            break;
        case Op::kSubF:
            stack[u].u.f = stack[u].u.f - stack[t].u.f;
            --sp;
            break;
        case Op::kSubV:
            for (int i = 0; i < in.lanes; ++i)
                stack[u].u.lanes[i] = stack[u].u.lanes[i] - stack[t].u.lanes[i];
            --sp;
            break;
        case Op::kMulI:
            stack[u].u.i = wrap_mul(stack[u].u.i, stack[t].u.i);
            --sp;
            break;
        case Op::kMulF:
            stack[u].u.f = stack[u].u.f * stack[t].u.f;
            --sp;
            break;
        case Op::kMulVV:
            for (int i = 0; i < in.lanes; ++i)
                stack[u].u.lanes[i] = stack[u].u.lanes[i] * stack[t].u.lanes[i];
            --sp;
            break;
        case Op::kMulVS:
            for (int i = 0; i < in.lanes; ++i)
                stack[u].u.lanes[i] = stack[u].u.lanes[i] * stack[t].u.f;
            --sp;
            break;
        case Op::kMulSV: {
            const float s = stack[u].u.f;
            stack[u] = stack[t];
            for (int i = 0; i < in.lanes; ++i)
                stack[u].u.lanes[i] = s * stack[u].u.lanes[i];
            --sp;
            break;
        }
        case Op::kMulQ:
            stack[u] = Value::of_quat(stack[u].as_quat() * stack[t].as_quat());
            --sp;
            break;
        case Op::kDivI:
            if (stack[t].u.i == 0)
                return EvalResult{Value{}, EvalStatus::kDivZero};
            stack[u].u.i =
                stack[t].u.i == -1 ? wrap_neg(stack[u].u.i) : stack[u].u.i / stack[t].u.i;
            --sp;
            break;
        case Op::kDivF:
            stack[u].u.f = stack[u].u.f / stack[t].u.f; // IEEE inf/NaN semantics
            --sp;
            break;
        case Op::kDivVS:
            for (int i = 0; i < in.lanes; ++i)
                stack[u].u.lanes[i] = stack[u].u.lanes[i] / stack[t].u.f;
            --sp;
            break;
        case Op::kModI:
            if (stack[t].u.i == 0)
                return EvalResult{Value{}, EvalStatus::kDivZero};
            stack[u].u.i = stack[t].u.i == -1 ? 0 : stack[u].u.i % stack[t].u.i;
            --sp;
            break;
        case Op::kLtI:
            stack[u] = Value::of_bool(stack[u].u.i < stack[t].u.i);
            --sp;
            break;
        case Op::kLtF:
            stack[u] = Value::of_bool(stack[u].u.f < stack[t].u.f);
            --sp;
            break;
        case Op::kLeI:
            stack[u] = Value::of_bool(stack[u].u.i <= stack[t].u.i);
            --sp;
            break;
        case Op::kLeF:
            stack[u] = Value::of_bool(stack[u].u.f <= stack[t].u.f);
            --sp;
            break;
        case Op::kGtI:
            stack[u] = Value::of_bool(stack[u].u.i > stack[t].u.i);
            --sp;
            break;
        case Op::kGtF:
            stack[u] = Value::of_bool(stack[u].u.f > stack[t].u.f);
            --sp;
            break;
        case Op::kGeI:
            stack[u] = Value::of_bool(stack[u].u.i >= stack[t].u.i);
            --sp;
            break;
        case Op::kGeF:
            stack[u] = Value::of_bool(stack[u].u.f >= stack[t].u.f);
            --sp;
            break;
        case Op::kEqB:
            stack[u] = Value::of_bool(stack[u].u.b == stack[t].u.b);
            --sp;
            break;
        case Op::kEqI:
            stack[u] = Value::of_bool(stack[u].u.i == stack[t].u.i);
            --sp;
            break;
        case Op::kEqF:
            stack[u] = Value::of_bool(stack[u].u.f == stack[t].u.f);
            --sp;
            break;
        case Op::kEqS:
            stack[u] = Value::of_bool(str_eq(stack[u], stack[t]));
            --sp;
            break;
        case Op::kEqN:
            stack[u] = Value::of_bool(stack[u].u.name_id == stack[t].u.name_id);
            --sp;
            break;
        case Op::kEqV:
            stack[u] = Value::of_bool(lanes_eq(stack[u], stack[t], in.lanes));
            --sp;
            break;
        case Op::kNeB:
            stack[u] = Value::of_bool(stack[u].u.b != stack[t].u.b);
            --sp;
            break;
        case Op::kNeI:
            stack[u] = Value::of_bool(stack[u].u.i != stack[t].u.i);
            --sp;
            break;
        case Op::kNeF:
            stack[u] = Value::of_bool(stack[u].u.f != stack[t].u.f);
            --sp;
            break;
        case Op::kNeS:
            stack[u] = Value::of_bool(!str_eq(stack[u], stack[t]));
            --sp;
            break;
        case Op::kNeN:
            stack[u] = Value::of_bool(stack[u].u.name_id != stack[t].u.name_id);
            --sp;
            break;
        case Op::kNeV:
            stack[u] = Value::of_bool(!lanes_eq(stack[u], stack[t], in.lanes));
            --sp;
            break;
        case Op::kIntToFloat:
            stack[t] = Value::of_float(static_cast<float>(stack[t].u.i));
            break;
        case Op::kFloatToInt: {
            const auto wide = static_cast<double>(stack[t].u.f);
            if (!(wide >= kInt64Lo && wide < kInt64Hi)) // NaN fails too
                return EvalResult{Value{}, EvalStatus::kIntRange};
            stack[t] = Value::of_int(static_cast<std::int64_t>(wide));
            break;
        }
        case Op::kLane:
            stack[t] = Value::of_float(stack[t].u.lanes[in.imm]);
            break;
        case Op::kCallNative: {
            const Builtin& builtin = builtins()[static_cast<std::size_t>(in.imm)];
            sp -= builtin.arity - 1;
            stack[sp - 1] = builtin.fn(&stack[sp - 1]);
            break;
        }
        }
    }
    return EvalResult{stack[0], EvalStatus::kOk};
}

} // namespace midday::expr

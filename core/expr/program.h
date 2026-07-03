// core/expr/program.h — the compiled expression: a compact, typed stack
// program designed for the statechart hot path (one eval per candidate
// transition per event).
//
// EVAL COST MODEL (the contract callers size against):
//   * time  — O(instruction count); every opcode is O(1) work (vector ops
//     touch at most 4 lanes; native calls are one indirect call). Programs
//     are branch-forward only: `&&`, `||`, `?:` compile to FORWARD jumps —
//     no loops exist in the instruction set, so eval always terminates in
//     at most `instruction_count()` steps.
//   * space — a fixed Value array on the C++ stack (kMaxEvalStack slots,
//     ~3 KiB). ZERO heap allocation, ZERO re-parsing, no recursion, no
//     exceptions (eval is noexcept). Repeated eval touches only the program
//     (read-only, shareable) and the caller's variable span.
//   * types were fully resolved at compile time: opcodes are pre-selected
//     per type (kAddI vs kAddF vs kAddV) — eval never inspects type tags.
//
// Runtime statuses (the ONLY partial operations in the language):
//   * kDivZero  — integer '/' or '%' with a zero right side.
//   * kIntRange — int(x) where x is NaN or outside int64 range.
// These are FAILURE-class (CLI exit 1): a filter faulting at runtime is a
// failed run, not an invalid file (compile diagnostics are the validation
// class, diag.h). Float division by zero is NOT an error: IEEE inf/NaN
// semantics apply, deterministically (core/math README non-finite policy).
//
// Integer semantics (documented, deterministic on every platform):
//   * + - * and unary - wrap two's-complement (computed in uint64).
//   * / % truncate toward zero; INT64_MIN / -1 wraps to INT64_MIN, and
//     INT64_MIN % -1 is 0 (guarded — never UB).

#pragma once

#include "core/base/error.h"
#include "core/expr/value.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace midday::expr {

enum class Op : std::uint8_t {
    kPushConst, // imm = constant index
    kPushVar,   // imm = variable slot
    kPop,
    kJump,            // imm = forward offset from the next instruction
    kJumpIfFalsePop,  // pop cond; jump when false
    kJumpIfFalseKeep, // keep cond on the stack; jump when false  (&&)
    kJumpIfTrueKeep,  // keep cond on the stack; jump when true   (||)
    kNegI,
    kNegF,
    kNegV, // lanes
    kNotB,
    kAddI,
    kAddF,
    kAddV, // lanes
    kSubI,
    kSubF,
    kSubV, // lanes
    kMulI,
    kMulF,
    kMulVV, // lanes; component-wise (Hadamard)
    kMulVS, // lanes; stack: vec, scalar
    kMulSV, // lanes; stack: scalar, vec
    kMulQ,  // quaternion composition
    kDivI,  // checked: kDivZero
    kDivF,  // IEEE
    kDivVS, // lanes; stack: vec, scalar
    kModI,  // checked: kDivZero
    kLtI,
    kLtF,
    kLeI,
    kLeF,
    kGtI,
    kGtF,
    kGeI,
    kGeF,
    kEqB,
    kEqI,
    kEqF, // IEEE (-0 == 0; NaN != NaN)
    kEqS, // byte equality of views
    kEqN, // Name id equality
    kEqV, // lanes; all-lanes IEEE equality
    kNeB,
    kNeI,
    kNeF,
    kNeS,
    kNeN,
    kNeV,        // lanes
    kIntToFloat, // IEEE round-to-nearest
    kFloatToInt, // checked: kIntRange; truncates toward zero
    kLane,       // imm = lane index; vec/quat -> float component
    kCallNative, // imm = builtin index; pops arity args, pushes the result
};

struct Instr {
    Op op;
    std::uint8_t lanes = 0; // vector opcodes: 2, 3, or 4
    std::int16_t imm = 0;   // constant/slot/builtin/lane index or jump offset
};

// Compiled-form limits; exceeding them is a compile diagnostic
// (expr.too_complex), never an eval-time surprise.
inline constexpr int kMaxEvalStack = 128;
inline constexpr int kMaxInstructions = 4096;

enum class EvalStatus : std::uint8_t {
    kOk = 0,
    kDivZero,  // integer '/' or '%' by zero
    kIntRange, // int(x) of NaN or out-of-int64-range input
};

struct EvalResult {
    Value value; // meaningful iff status == kOk
    EvalStatus status = EvalStatus::kOk;
};

// Lift a non-ok eval status into the engine Error envelope (cold path):
// codes "expr.div_zero" / "expr.int_range" — FAILURE-class (exit 1).
base::Error to_error(EvalStatus status);

namespace detail {

// Everything codegen produces; Program is an immutable wrapper around it.
struct ProgramParts {
    std::vector<Instr> code;
    std::vector<Value> constants;
    // Owned bytes for string constants (constants' Values point into these;
    // unique_ptr keeps the addresses stable across Program moves).
    std::vector<std::unique_ptr<std::string>> strings;
    ValueType result_type = ValueType::kBool;
    int max_stack = 0;
    int var_count = 0;
};

} // namespace detail

class Program {
public:
    explicit Program(detail::ProgramParts parts) : parts_(std::move(parts)) {}

    Program(Program&&) = default;
    Program& operator=(Program&&) = default;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    ~Program() = default;

    // Evaluates against `vars`, bound in EnvSpec slot order with the declared
    // types (env.h contract). Deterministic: same program + same variable
    // bits => same result bits, on every supported platform (BIT-PORTABLE
    // operation set only — D-BUILD-019).
    [[nodiscard]] EvalResult eval(std::span<const Value> vars) const noexcept;

    [[nodiscard]] ValueType result_type() const { return parts_.result_type; }

    [[nodiscard]] int instruction_count() const { return static_cast<int>(parts_.code.size()); }

    [[nodiscard]] int max_stack() const { return parts_.max_stack; }

    // Number of EnvSpec slots the eval span must provide.
    [[nodiscard]] int var_count() const { return parts_.var_count; }

private:
    detail::ProgramParts parts_;
};

} // namespace midday::expr

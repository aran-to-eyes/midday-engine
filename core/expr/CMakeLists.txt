# core/expr — THE expression language (m0-expr-lang): one deterministic,
# side-effect-free, statically typed language for transition `if` filters,
# `when` watchers, and model params. Compile once (lex/parse/typecheck/
# codegen), evaluate per event with zero allocation. Function inventory
# registers through core/reflect. Language reference: core/expr/README.md.

add_library(midday_expr STATIC
    codegen.cpp
    diag.cpp
    env.cpp
    eval.cpp
    expr.cpp
    functions.cpp
    lexer.cpp
    parser.cpp
    typecheck.cpp
    value.cpp
)
add_library(midday::expr ALIAS midday_expr)

target_link_libraries(midday_expr
    PUBLIC midday::options midday::base midday::math midday::reflect
    PRIVATE fast_float::fast_float)

# Expression evaluation runs INSIDE the sim (spec section 4.3): the
# deterministic-FP contract applies — results are bit-compared in CI.
midday_apply_deterministic_fp(midday_expr)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (same pattern as midday_base_tests, D-BUILD-014).
add_library(midday_expr_tests OBJECT
    determinism_test.cpp
    eval_test.cpp
    functions_test.cpp
    parse_test.cpp
    typecheck_test.cpp
)
target_link_libraries(midday_expr_tests PRIVATE midday::expr doctest::doctest xxhash::xxhash)
midday_apply_deterministic_fp(midday_expr_tests)

# core/expr — THE expression language (m0-expr-lang)

One deterministic, side-effect-free, statically typed expression language, shared by
transition `if:` filters, `when:` watchers, and model `params` expressions (spec §4.2:
"no per-context mini-DSLs"; §10 model op-lists use the same language). Expressions appear
in YAML as strings (§8) and evaluate INSIDE the sim — bit-deterministic, side-effect-free
**by construction** (§4.3): there is no assignment, no statement, no loop, no user
function, and no syntax that could express one.

Facade: `expr.h` — `compile(source, EnvSpec, origin) -> CompileResult`, then
`Program::eval(span<const Value>)`. Pipeline: `lexer` → `parser` (AST) → `typecheck`
(annotates) → `codegen` (typed stack program) → `eval` (dispatch loop).

## Grammar (conventional infix)

```
expr        := or_expr ('?' expr ':' expr)?            # ternary, right-assoc
or_expr     := and_expr (('or'  | '||') and_expr)*     # short-circuit
and_expr    := equality (('and' | '&&') equality)*     # short-circuit
equality    := relational (('==' | '!=') relational)*
relational  := additive (('<' | '<=' | '>' | '>=') additive)*
additive    := multiplicative (('+' | '-') multiplicative)*
multiplicative := unary (('*' | '/' | '%') unary)*
unary       := ('-' | '!' | 'not') unary | postfix
postfix     := primary ('.' component)*                # x y z w
primary     := int | float | string | 'true' | 'false'
             | '(' expr ')' | path | function '(' args ')'
path        := ident ('.' ident)*
```

**Rejected at parse, with structured diagnostics** (`expr.side_effect`, validation-class
→ CLI exit 3): `=`, `+= -= *= /= %=`, `++`/`--`, `;`, and the reserved statement words
`if else while for let var fn function return` (each rejection says what to write
instead). Nesting is capped at 64 levels (`expr.too_complex`), like `Json::parse`'s
depth cap. Strings use `'...'` or `"..."` (single quotes exist because expressions live
inside double-quoted YAML strings); escapes: `\\ \' \" \n \t`.

## Value & numeric model

Types: `bool · int (int64) · float (float32, THE sim scalar) · string · name · vec2 ·
vec3 · vec4 · quat` — exactly the reflect `TypeDesc` kinds that make sense in an
expression. **Excluded** (documented in `value.h`): `color`, `entity_ref`, `asset_ref`,
`array`, `map`.

- **Exactly one implicit coercion: `int -> float`** (mixed operands, float parameters,
  unified `?:` branches). Never `float -> int` — that is the explicit, range-checked
  `int()`. Coerced *literals* fold to float constants at compile time.
- **int**: `+ - *` and unary `-` wrap two's-complement (deterministic, documented);
  `/ %` truncate toward zero; `/ 0` and `% 0` are the runtime status `expr.div_zero`;
  `INT64_MIN / -1` wraps, `INT64_MIN % -1 == 0` (guarded, never UB).
- **float**: IEEE 754 float32 under the pinned deterministic-FP flags. Division by zero
  is IEEE (`inf`/NaN), NOT an error; NaN compares false; `==` is IEEE (`-0 == 0`,
  `NaN != NaN`).
- **string**: literals and bound variables only — comparisons (`==`/`!=`) exist,
  construction/concatenation does not (that is what keeps eval allocation-free).
- **name**: compares by interned id. A string LITERAL folds to a name constant at
  compile time where a name is expected; a string VARIABLE does not (it would intern at
  eval).
- **vectors/quat**: `+ - *(Hadamard) * scalar / scalar` on vec2/3/4; `quat * quat`
  composition; `.x .y .z .w` component access (width-checked); `==`/`!=` lane-wise.
- Variables resolve by **longest declared prefix**: the binder declares dotted names
  (`health.current`, `hull.bbox.max`) and remaining segments are component access.

## Compile → evaluate split & eval cost model

`compile()` produces an immutable `Program`: a typed stack machine (opcodes selected
from static types — eval never reads a type tag), a constant pool, and forward-only
jumps implementing real short-circuit for `&&`/`||`/`?:` (the untaken side is skipped:
it can neither cost time nor fault — `d != 0 && 1/d > k` is safe).

Per eval (`Program::eval`, `noexcept`): **O(instruction count)** time, every opcode O(1)
(≤ 4 lanes / one indirect call); **zero heap allocation, zero re-parsing, no recursion,
no exceptions**; the value stack is a fixed array on the C++ stack (`kMaxEvalStack` =
128 slots, ≈3 KiB). No loop opcodes exist, so eval terminates in ≤ `instruction_count()`
steps. Binding a variable is one array write (`EnvSpec` slot order). Programs exceeding
the limits are refused at compile (`expr.too_complex`) — never an eval-time surprise.

The only runtime statuses are `expr.div_zero` and `expr.int_range` (from `int()`),
FAILURE-class (exit 1); all compile diagnostics are validation-class (exit 3, `diag.h`).

## Function inventory (registered through core/reflect)

`functions.cpp` holds ONE table driving the typechecker, the evaluator, and
`register_expr_functions(Registry&)` — every entry a `MethodDesc` free function with
param/return types and a compat hash, shipped in `engine_api.json` (m0-api-json).
**One name = one signature = one registry entry — no overloads** (the language stays
1:1 with what agents read; an overload-set need is a registry-model decision first).

- conversions: `int(float)`, `float(int)`
- scalar float: `abs sign floor ceil round trunc fract sqrt min max clamp saturate lerp`
- constructors: `vec2 vec3 vec4 quat` (quat is raw components, not normalized)
- vec3 geometry: `dot cross length length_squared normalize distance distance_squared`,
  `rotate(quat, vec3)`

Every function is **total** and **BIT-PORTABLE** (D-BUILD-019): IEEE `+ - * /`, `sqrt`,
and exact integral roundings only. **Excluded**: `sin cos tan asin acos atan atan2 exp
log pow` — LIBM-BOUND; when filters need one it lands as a `det_*` controlled
polynomial in core/math first (the `det_log` recipe), never raw libm. vec2/vec4
*functions* are also excluded for now — operators, constructors, components, and
equality cover them; the inventory grows additively when a consumer arrives.

Tests: `expr.*` doctest cases beside the code (`midday selftest --filter 'expr.*'`),
including the determinism corpus (dual run + shuffled compile order + pinned
known-answer float bits).

# core/math — the deterministic math stdlib (m0-math-stdlib)

One header per domain; everything in `namespace midday::math`; the sim scalar is
**float32** (double only where documented: RNG distribution internals, arc-length
accumulation).

- `vec.h` — Vec2/3/4, dot/cross/lerp/normalize (zero -> zero policy), scalar helpers.
- `mat.h` — Mat3/Mat4. **THE column convention** (documented once there, asserted in
  math.mat tests): column-major storage, column vectors (`v' = M * v`), translation in
  column 3, right-handed.
- `quat.h` — quaternions with an **explicit normalize policy**: rotation ops require
  unit input and never renormalize silently; `normalized()`/`is_normalized()` are the
  caller's tools. `nlerp` is the bit-portable blend; `slerp`/`from_axis_angle` are
  libm-bound (below).
- `xform.h` — TRS transform; `to_mat4` = T·R·S; deterministic decompose (scale from
  column lengths, mirror folds into scale.x, Shepperd quat extraction). Hierarchy
  composition happens in matrix space (TRS is not closed under non-uniform scale).
- `intersect.h` — Aabb (min/max), Plane (`n·x = d`), Ray, Sphere; slab ray-AABB
  (Williams et al.), stable-quadratic ray-sphere (RTCD), Möller–Trumbore ray-triangle
  (the Godot shape), closed-interval AABB overlap, center-extent plane classify.
- `spline.h` — Bézier (de Casteljau) / uniform Catmull-Rom (τ=1/2) / uniform cubic
  B-spline segment evaluators + derivatives; `ArcLengthTable` (fixed-count sampled
  bake + piecewise-linear inverse, the Godot Curve3D approach — chosen because
  adaptive quadrature would make bits depend on tolerances). Scene-primitive
  integration is node m4-splines.
- `ease.h` — the full Penner family (exact reference formulas, easings.net constants),
  free functions + `Ease` enum dispatch.
- `rng.h` — **Philox4x32-10** counter-based streams (D-BUILD-017): key = seed,
  counter = (block, stream); order-independent child streams via XXH3 over
  (domain tag, parent stream, Name-id/index); **no global RNG state anywhere**.
  Distributions: Lemire bounded ints, bitmask-rejection u64, 24/53-bit uniforms,
  inverse-CDF normal (Acklam) over `det_log`, rejection disk/sphere sampling
  (Marsaglia). Pinned against the official Random123 known-answer vectors.
- `noise.h` — value + Perlin (improved, 2002) gradient noise, fBm; integer-only
  lattice hashing (SplitMix64 finalizer `mix64`), seedable (no permutation table).
  Known-answer bit pins in math.noise tests.
- `fixture.h` — the 1,000,000-op RNG/transform determinism fixture; digest pinned in
  math.determinism and published as `math_fixture_hash` in `midday selftest --json`
  for CI's determinism lane.

Tests: `math.*` doctest cases beside the code, linked in as `midday_math_tests`
OBJECT lib. Run: `midday selftest --filter 'math.*'`.

Future work (recorded, not speculatively built): SIMD kernels for Vec/Mat/Quat hot
paths — the types are plain aggregates precisely so a later node can add SIMD
implementations behind the same API and prove bit-equality per lane against these
scalar kernels.

## Deterministic floating-point policy

The FP half of the determinism contract (spec §4.3/§15 constraint 5). Enforced by
`cmake/DeterministicFP.cmake` at configure/target level and re-checked falsifiably by
`scripts/check_fp_flags.py` against the emitted `compile_commands.json` in CI.

### What the build enforces, per compiler

| Toolchain | Flags applied to every sim target | Effect |
|---|---|---|
| Clang / AppleClang | `-fno-fast-math -ffp-contract=off` | no fast-math value changes; **no FMA contraction** — `a*b+c` is two IEEE-rounded ops on every lane, at every optimization level |
| GCC | `-fno-fast-math -ffp-contract=off` | same; GCC would otherwise contract by default at `-O2` |
| MSVC | `/fp:precise` | no reassociation, no contraction into FMA by default |

Configure-time guard: any global opt-in to `-ffast-math`, `-Ofast`,
`-funsafe-math-optimizations`, `-ffp-contract=fast`, or `/fp:fast` hard-fails the
configure. `check_fp_flags.py` additionally bans the sub-flags
(`-fassociative-math`, `-freciprocal-math`, `-ffinite-math-only`,
`-menable-unsafe-fp-math`) per translation unit.

### x87 ban (and why)

32-bit x86 x87 evaluates in 80-bit extended precision with double rounding on spill —
results depend on register allocation, which depends on the optimizer's mood. That can
never be bit-stable. `DeterministicFP.cmake` hard-fails on 32-bit x86 targets; all five
supported platforms (Linux/Windows x86-64 → SSE2 scalar math; macOS/iOS/Android
arm64 → NEON scalar) evaluate float/double at declared precision
(`FLT_EVAL_METHOD == 0`).

### FMA contraction policy

Pinned **off** (`-ffp-contract=off` / `/fp:precise`). A fused multiply-add rounds once
where separate ops round twice; whether a compiler contracts varies by version,
optimization level, and even statement shape — the single biggest cause of
same-source cross-lane FP drift. With contraction off, `+ - * /` and `sqrt` are IEEE
754 correctly-rounded operations: **same inputs, same bits, on every supported
toolchain**. Hardware FMA intrinsics may only ever appear later as an explicit opt-in
API (`fma()`-as-an-operation), never as a compiler rewrite.

### What is and is not bit-portable (honest table)

| Class | Operations | Guarantee |
|---|---|---|
| BIT-PORTABLE | `+ - * /`, `sqrt`, comparisons, `floor` on all types; every integer op | identical bits across runs, hosts, platforms, compilers (given the flags above) |
| BIT-PORTABLE (by construction) | Philox RNG, all distributions incl. `normal()` (via `det_log`), value/Perlin/fBm noise, splines, polynomial easings, vec/mat/xform/intersect kernels, the 1M-op fixture | pinned by known-answer bit tests; a lane mismatch is a build-config bug, not "FP noise" |
| LIBM-BOUND | `std::sin/cos/acos/exp2/pow/log` — used by `slerp`, `from_axis_angle`, sine/expo/elastic easings | deterministic **within one build** (same binary + libm ⇒ same bits, which is what the M0 determinism contract gates); NOT bit-identical across platforms/libcs |

Float **transcendentals are not portably correctly-rounded** — libm results differ
between glibc, Apple libm, and MSVCRT (and can differ across glibc versions). We do
not pretend otherwise. Where cross-PLATFORM bit-exactness is required the engine uses
integer-based or polynomial implementations **we control**:

- `det_log(double)` (rng.h): exponent/mantissa bit split + fixed-order atanh-series
  Horner polynomial; only IEEE `+ - * /`. |rel err| < 4e-15 (tested against libm),
  bit-identical everywhere. It exists so `normal()`'s tail is bit-portable.
- Noise lattice hashing is pure integer (`mix64`); gradients/fades are polynomial.
- The determinism fixture and every known-answer pin avoid libm entirely.

When a future node needs cross-platform `sin/cos` (e.g. TS/C++ parity for the procgen
stdlib, spec §13), the same recipe applies: a controlled polynomial implementation in
this module, KAT-pinned — never "hope libm agrees". Cross-platform determinism beyond
the pinned Linux-x86_64 lane remains a stretch goal at the CONTRACT level (spec §15),
but every operation in the BIT-PORTABLE classes above already meets it.

### Non-finite policy

Sim code never launders NaN/Inf into state: `normalized()` returns zero (vectors) or
identity (quats) on degenerate input; decompose falls back to canonical axes;
intersection kernels rely on well-defined IEEE inf semantics (slab method) and are
KAT-tested. Division by a caller-supplied zero (e.g. `inverse()` of a singular
matrix) yields inf/NaN deterministically — guard with `determinant()` where input
isn't known-invertible.

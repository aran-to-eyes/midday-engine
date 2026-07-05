// ts/toolchain/toolchain.h — the TypeScript toolchain: the vendored compiler
// (third_party/typescript/typescript.js) running ON the embedded QuickJS —
// no Node anywhere (Aurora D-11). One class drives transpile + typecheck
// against api/engine.d.ts, the engine lint pack, and the content-hash cache.
//
// Cache contract (byte-stable keys, proven by script.cache doctests):
//   toolchain fingerprint = XXH3-128 over the length-prefixed segments
//     [schema "midday.ts.cache/1" + lint pack version, canonical compiler
//      options JSON, typescript.js bytes, driver.js bytes, engine.d.ts bytes]
//   module key = XXH3-128 over [fingerprint hex, source bytes]
//   cached artifact = <cache_dir>/<key>.js  (the compiled module, verbatim)
// The key is a pure function of content — no paths, no clocks, no locale —
// so the same source under the same toolchain yields byte-identical cache
// entries on every platform. Only clean builds (zero diagnostics) populate
// the cache; a hit therefore skips compile AND check soundly.
//
// Lint pack (kLintPackVersion, mechanism: TypeScript AST walk in driver.js —
// never regex over text): no-wall-clock (Date.now / new Date() / Date() /
// performance.now), no-unseeded-random (Math.random), no-timer (setTimeout /
// setInterval), each reported with file:line:col. The rules run on every
// non-declaration source in the program — imports included — independent of
// whether the names typecheck. Bypass policy: NONE — there is no disable
// comment, no config switch, no severity downgrade; sim code that needs time
// or randomness gets both from the engine bindings.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "ts/runtime/script_runtime.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midday::script {

struct ToolchainConfig {
    // The only two knobs any caller sets. The vendored-compiler paths, the
    // driver, and the engine TS library dir are fixed constants (kTypescriptJs
    // etc. in toolchain.cpp): they are canonical parts of the toolchain
    // fingerprint, so nothing may vary them. Repo-root-relative (M0 verbs run
    // from the project root).
    std::string engine_dts = "api/engine.d.ts";
    std::string cache_dir = ".midday-cache/ts"; // regenerable, gitignored, never drift-gated
};

// One diagnostic, structured: kind "type" (tsc, code "TS<n>") or "lint"
// (engine rule name). line/col are 1-based; 0 = no location (options-level).
struct Diagnostic {
    std::string kind;
    std::string code;
    std::string file;
    std::int64_t line = 0;
    std::int64_t col = 0;
    std::string message;

    [[nodiscard]] base::Json to_json() const;
    [[nodiscard]] std::string to_string() const; // "file:line:col: kind code: message"
};

// check(): ok iff zero diagnostics. `failure` is an infrastructure error
// (missing vendored compiler, unreadable source) — never a script problem.
struct CheckOutcome {
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::optional<base::Error> failure;
};

struct BuildOutcome {
    CheckOutcome check;
    std::string cache_key; // 32 hex chars (XXH3-128)
    std::string js_path;   // cached artifact path (set on success)
    std::string js_source; // the compiled module
    bool cache_hit = false;
};

struct BuildStats {
    std::int64_t transpiled = 0;
    std::int64_t cache_hits = 0;
};

class Toolchain {
public:
    explicit Toolchain(ToolchainConfig config = {});
    ~Toolchain();
    Toolchain(const Toolchain&) = delete;
    Toolchain(Toolchain&&) = delete;
    Toolchain& operator=(const Toolchain&) = delete;
    Toolchain& operator=(Toolchain&&) = delete;

    // Typecheck + lint (no emit, no cache traffic).
    CheckOutcome check(const std::string& path);

    // check + transpile + populate the content-hash cache. A cache hit skips
    // the compiler entirely (the key covers everything that could change the
    // outcome) and counts in stats().cache_hits; a miss that compiles clean
    // counts in stats().transpiled.
    BuildOutcome build(const std::string& path);

    [[nodiscard]] const BuildStats& stats() const;

    // Library seam for `midday run` (m0-yaml-loader-run): build `path`
    // through the cache and evaluate it on `runtime`. Installs a module
    // resolver on `runtime` so relative imports (./a, ../b, extensionless)
    // build through the same cache; bare specifiers stay unresolvable until
    // the batch bindings land. On failure the error carries the structured
    // script shape — {file, line, col, stack} plus the {tick,
    // replay_bookmark} slots the sim caller fills via annotate_sim_context.
    struct LoadOutcome {
        std::string resolved;
        std::optional<base::Error> error;
    };

    LoadOutcome load_module(ScriptRuntime& runtime, const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// XXH3-128 over length-prefixed segments (u64 little-endian byte count before
// each segment's bytes), spelled as 32 lowercase hex chars. Exposed for the
// byte-stability doctests; the cache composition above is built on it.
std::string content_key_hex(std::span<const std::string_view> segments);

} // namespace midday::script

// ts/toolchain/toolchain.cpp — drives the vendored TypeScript compiler on the
// embedded QuickJS (tool profile), owns the content-hash cache, and adapts
// driver.js responses into structured outcomes. File access for the compiler
// goes through ONE host hook (__midday_read_file) with a canonical-path
// allowlist: the vendored lib dir, api/engine.d.ts, and the entry's subtree.

#include "ts/toolchain/toolchain.h"

#include "core/base/file_io.h"
#include "core/base/hex.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace midday::script {
namespace {

constexpr std::string_view kIoCode = "script.io";
constexpr std::string_view kCacheSchema = "midday.ts.cache/1 midday-lint/1";

// Canonical compiler options — part of the cache key, mapped by driver.js via
// ts.convertCompilerOptionsFromJson (tsconfig spellings). Deterministic emit:
// LF newlines, no source maps, no timestamps. skipLibCheck stays OFF on
// purpose: every check also tsc-validates api/engine.d.ts and the vendored
// libs — the toolchain tier of formats/engine_dts.meta.md's contract.
constexpr std::string_view kCompilerOptionsJson =
    R"({"lib":["es2020"],"module":"esnext","moduleResolution":"bundler",)"
    R"("newLine":"lf","strict":true,"target":"es2020","types":[]})";

std::string generic(const std::filesystem::path& path) {
    return path.generic_string();
}

bool within(const std::filesystem::path& inner, const std::filesystem::path& root) {
    const auto [root_end, ignored] = std::ranges::mismatch(root, inner);
    return root_end == root.end();
}

Diagnostic diagnostic_from_json(const base::Json& value) {
    Diagnostic diag;
    const auto text = [&](const char* key) {
        const base::Json* field = value.find(key);
        return field != nullptr && field->is_string() ? field->as_string() : std::string();
    };
    const auto number = [&](const char* key) {
        const base::Json* field = value.find(key);
        return field != nullptr && field->is_int() ? field->as_int() : std::int64_t{0};
    };
    diag.kind = text("kind");
    diag.code = text("code");
    diag.file = text("file");
    diag.line = number("line");
    diag.col = number("col");
    diag.message = text("message");
    return diag;
}

} // namespace

base::Json Diagnostic::to_json() const {
    base::Json out = base::Json::object();
    out.set("kind", kind);
    out.set("code", code);
    out.set("file", file);
    out.set("line", line);
    out.set("col", col);
    out.set("message", message);
    return out;
}

std::string Diagnostic::to_string() const {
    std::string where = file.empty() ? std::string("<options>") : file;
    if (line > 0)
        where += ":" + std::to_string(line) + ":" + std::to_string(col);
    return where + ": " + kind + " " + code + ": " + message;
}

std::string content_key_hex(std::span<const std::string_view> segments) {
    std::size_t total = 0;
    for (const std::string_view segment : segments)
        total += 8 + segment.size();
    std::string stream;
    stream.reserve(total);
    for (const std::string_view segment : segments) {
        std::uint64_t length = segment.size();
        for (int i = 0; i < 8; ++i) { // u64 little-endian length prefix
            stream.push_back(static_cast<char>(length & 0xFF));
            length >>= 8;
        }
        stream.append(segment);
    }
    const XXH128_hash_t hash = XXH3_128bits(stream.data(), stream.size());
    return base::hex64(hash.high64) + base::hex64(hash.low64);
}

struct Toolchain::Impl {
    ToolchainConfig config;
    BuildStats stats;
    std::unique_ptr<ScriptRuntime> runtime; // tool profile, lazily booted
    std::string fingerprint;                // lazily computed (see toolchain.h)
    std::filesystem::path entry_root;       // read-allowlist root for the current entry
    std::optional<base::Error> deferred;    // import-build failure carried out of the resolver

    [[nodiscard]] bool read_allowed(const std::filesystem::path& path) const {
        std::error_code ec;
        const std::filesystem::path canon = std::filesystem::weakly_canonical(path, ec);
        if (ec)
            return false;
        const auto canon_of = [](const std::string& p) {
            std::error_code inner;
            return std::filesystem::weakly_canonical(p, inner);
        };
        return canon == canon_of(config.engine_dts) || within(canon, canon_of(config.lib_dir)) ||
               (!entry_root.empty() && within(canon, entry_root));
    }

    std::optional<base::Error> ensure_runtime() {
        if (runtime != nullptr)
            return std::nullopt;
        // Tool profile: big heap and stack for the compiler (its parser
        // recursion is measured in REAL C-stack bytes, which quadruple in
        // unoptimized builds), wall clock allowed — output stays
        // byte-deterministic (script.cache doctests).
        auto booted = std::make_unique<ScriptRuntime>(RuntimeConfig{
            .memory_limit_bytes = 512u << 20,
            .gas_limit = 0,
            .stack_size_bytes = 6u << 20,
            .deterministic = false,
        });
        booted->register_host_fn("__midday_read_file", [this](const base::Json::Array& args) {
            HostResult result;
            if (args.size() != 1 || !args[0].is_string()) {
                result.error = base::Error{.code = "script.host",
                                           .message = "__midday_read_file expects one path"};
                return result;
            }
            const std::string& path = args[0].as_string();
            if (!read_allowed(path)) {
                result.value = base::Json(); // null: outside the allowlist = not found
                return result;
            }
            base::ReadFileResult file = base::read_file(path, kIoCode);
            result.value = file.error ? base::Json() : base::Json(std::move(file.bytes));
            return result;
        });
        for (const std::string* path : {&config.typescript_js, &config.driver_js}) {
            base::ReadFileResult file = base::read_file(*path, kIoCode);
            if (file.error)
                return std::move(file.error);
            if (auto error = booted->eval_global(file.bytes, *path))
                return error;
        }
        runtime = std::move(booted);
        return std::nullopt;
    }

    std::optional<base::Error> ensure_fingerprint() {
        if (!fingerprint.empty())
            return std::nullopt;
        std::string blobs[3];
        const std::string* files[] = {&config.typescript_js, &config.driver_js, &config.engine_dts};
        for (std::size_t i = 0; i < 3; ++i) {
            base::ReadFileResult file = base::read_file(*files[i], kIoCode);
            if (file.error)
                return std::move(file.error);
            blobs[i] = std::move(file.bytes);
        }
        const std::string_view segments[] = {
            kCacheSchema, kCompilerOptionsJson, blobs[0], blobs[1], blobs[2]};
        fingerprint = content_key_hex(segments);
        return std::nullopt;
    }

    // Run driver.js on the entry; decode into a CheckOutcome (+ emitted js).
    CheckOutcome compile(const std::string& path, bool emit, std::string* js_out) {
        CheckOutcome outcome;
        if (auto error = ensure_runtime()) {
            outcome.failure = std::move(error);
            return outcome;
        }
        std::error_code ec;
        entry_root =
            std::filesystem::weakly_canonical(std::filesystem::path(path).parent_path(), ec);
        base::Json::ParseResult options = base::Json::parse(kCompilerOptionsJson, "<options>");
        base::Json request = base::Json::object();
        request.set("entry", generic(path));
        request.set("engine_dts", generic(config.engine_dts));
        request.set("lib_dir", generic(config.lib_dir));
        request.set("options", std::move(options.value));
        request.set("emit", emit);
        EvalResult result = runtime->call_json("__midday_ts_run", request);
        entry_root.clear();
        if (result.error) {
            outcome.failure = std::move(result.error);
            return outcome;
        }
        const base::Json* failure = result.value.find("failure");
        if (failure != nullptr && failure->is_string()) {
            outcome.failure =
                base::Error{.code = "script.toolchain", .message = failure->as_string()};
            return outcome;
        }
        if (const base::Json* diags = result.value.find("diagnostics");
            diags != nullptr && diags->is_array())
            for (const base::Json& diag : diags->elements())
                outcome.diagnostics.push_back(diagnostic_from_json(diag));
        outcome.ok = outcome.diagnostics.empty();
        if (js_out != nullptr) {
            const base::Json* js = result.value.find("js");
            if (outcome.ok && js != nullptr && js->is_string())
                *js_out = js->as_string();
        }
        return outcome;
    }

    BuildOutcome build(const std::string& path) {
        BuildOutcome outcome;
        if (auto error = ensure_fingerprint()) {
            outcome.check.failure = std::move(error);
            return outcome;
        }
        base::ReadFileResult source = base::read_file(path, kIoCode);
        if (source.error) {
            outcome.check.failure = std::move(source.error);
            return outcome;
        }
        const std::string_view segments[] = {fingerprint, source.bytes};
        outcome.cache_key = content_key_hex(segments);
        const std::filesystem::path cached =
            std::filesystem::path(config.cache_dir) / (outcome.cache_key + ".js");
        if (base::ReadFileResult hit = base::read_file(cached, kIoCode); !hit.error) {
            outcome.check.ok = true;
            outcome.cache_hit = true;
            outcome.js_path = generic(cached);
            outcome.js_source = std::move(hit.bytes);
            ++stats.cache_hits;
            return outcome;
        }
        outcome.check = compile(path, true, &outcome.js_source);
        if (!outcome.check.ok)
            return outcome;
        std::error_code ec;
        std::filesystem::create_directories(config.cache_dir, ec);
        // Write-then-rename: concurrent builders race benignly (same bytes).
        const std::filesystem::path tmp = cached.string() + ".tmp";
        if (auto error = base::write_file(tmp, outcome.js_source, kIoCode)) {
            outcome.check.failure = std::move(error);
            return outcome;
        }
        std::filesystem::rename(tmp, cached, ec);
        if (ec) {
            outcome.check.failure =
                base::Error{.code = std::string(kIoCode),
                            .message = "cannot move cache entry into place: " + generic(cached)};
            return outcome;
        }
        outcome.js_path = generic(cached);
        ++stats.transpiled;
        return outcome;
    }
};

Toolchain::Toolchain(ToolchainConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

Toolchain::~Toolchain() = default;

CheckOutcome Toolchain::check(const std::string& path) {
    return impl_->compile(path, false, nullptr);
}

BuildOutcome Toolchain::build(const std::string& path) {
    return impl_->build(path);
}

const BuildStats& Toolchain::stats() const {
    return impl_->stats;
}

Toolchain::LoadOutcome Toolchain::load_module(ScriptRuntime& runtime, const std::string& path) {
    impl_->deferred.reset();
    runtime.set_module_resolver([this](std::string_view specifier,
                                       std::string_view referrer) -> std::optional<ModuleSource> {
        std::filesystem::path target;
        if (referrer.empty()) {
            target = std::filesystem::path(specifier); // top-level load: a real path
        } else if (specifier.starts_with("./") || specifier.starts_with("../")) {
            target = std::filesystem::path(referrer).parent_path() / specifier;
        } else {
            return std::nullopt; // bare specifiers: batch bindings territory
        }
        if (!target.has_extension())
            target += ".ts";
        const std::string source_path = generic(target.lexically_normal());
        BuildOutcome built = impl_->build(source_path);
        if (built.check.failure) {
            impl_->deferred = std::move(built.check.failure);
            return std::nullopt;
        }
        if (!built.check.ok) {
            base::Error error{.code = "script.type_error",
                              .message = built.check.diagnostics.front().to_string()};
            base::Json list = base::Json::array();
            for (const Diagnostic& diag : built.check.diagnostics)
                list.push(diag.to_json());
            error.details.set("diagnostics", std::move(list));
            impl_->deferred = std::move(error);
            return std::nullopt;
        }
        return ModuleSource{source_path, std::move(built.js_source)};
    });
    ScriptRuntime::LoadedModule loaded = runtime.load_module(path);
    if (impl_->deferred) // an import failed to BUILD: that error outranks "not found"
        return {std::move(loaded.resolved), std::move(impl_->deferred)};
    return {std::move(loaded.resolved), std::move(loaded.error)};
}

} // namespace midday::script

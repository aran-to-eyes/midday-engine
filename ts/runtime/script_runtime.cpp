// ts/runtime/script_runtime.cpp — QuickJS embedding: lifecycle, limits, the
// sim-profile prelude, module loading, host hooks, and the JS-exception ->
// base::Error translation. The ONLY translation unit that speaks QuickJS.

#include "ts/runtime/script_runtime.h"

#include <cstring>
#include <quickjs.h>
#include <string>
#include <utility>
#include <vector>

namespace midday::script {
namespace {

// SIM profile: poison every wall-clock/randomness door at the JS level.
// quickjs-libc is not vendored, so setTimeout/setInterval/os.* never exist.
// There is deliberately no unpoison API (bypass policy: none).
constexpr std::string_view kSimPrelude = R"js("use strict";
(() => {
    const poison = (what, hint) => function () {
        throw new TypeError("script.nondeterminism: " + what + " is disabled in the sim — " + hint);
    };
    const clock = "sim time is the tick, not the wall clock";
    const date = poison("Date", clock);
    date.now = poison("Date.now", clock);
    date.parse = poison("Date.parse", clock);
    date.UTC = poison("Date.UTC", clock);
    globalThis.Date = date;
    globalThis.performance = Object.freeze({
        now: poison("performance.now", clock),
        timeOrigin: NaN,
    });
    Math.random = poison("Math.random", "seeded randomness arrives through the engine bindings");
})();
)js";

// First stack line shaped "    at name (file:line:col)" or "    at file:line:col"
// (quickjs-ng backtrace format). Returns false for frames like "(native)".
bool parse_top_frame(std::string_view stack,
                     std::string& file,
                     std::int64_t& line,
                     std::int64_t& col) {
    const std::size_t at = stack.find("at ");
    if (at == std::string_view::npos)
        return false;
    std::string_view frame = stack.substr(at + 3);
    if (const std::size_t eol = frame.find('\n'); eol != std::string_view::npos)
        frame = frame.substr(0, eol);
    if (const std::size_t open = frame.rfind('('); open != std::string_view::npos) {
        frame = frame.substr(open + 1);
        if (const std::size_t close = frame.rfind(')'); close != std::string_view::npos)
            frame = frame.substr(0, close);
    }
    // frame is now "file:line:col" (both suffixes numeric) or something else.
    const auto number_after = [&](std::size_t colon, std::int64_t& out) {
        const std::string_view digits = frame.substr(colon + 1);
        if (digits.empty())
            return false;
        std::int64_t value = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + (c - '0');
        }
        out = value;
        return true;
    };
    const std::size_t col_colon = frame.rfind(':');
    if (col_colon == std::string_view::npos || !number_after(col_colon, col))
        return false;
    frame = frame.substr(0, col_colon);
    const std::size_t line_colon = frame.rfind(':');
    if (line_colon == std::string_view::npos || !number_after(line_colon, line))
        return false;
    file = std::string(frame.substr(0, line_colon));
    return true;
}

std::string to_std_string(JSContext* ctx, JSValueConst value) {
    std::size_t len = 0;
    const char* cstr = JS_ToCStringLen(ctx, &len, value);
    if (cstr == nullptr)
        return {};
    std::string out(cstr, len);
    JS_FreeCString(ctx, cstr);
    return out;
}

std::string property_string(JSContext* ctx, JSValueConst object, const char* name) {
    const JSValue prop = JS_GetPropertyStr(ctx, object, name);
    std::string out;
    if (!JS_IsUndefined(prop) && !JS_IsException(prop))
        out = to_std_string(ctx, prop);
    JS_FreeValue(ctx, prop);
    return out;
}

// JSON is the value bridge (batch bindings are the fast path, next node).
JSValue json_to_js(JSContext* ctx, const base::Json& value) {
    const std::string text = value.dump();
    return JS_ParseJSON(ctx, text.data(), text.size(), "<midday:json>");
}

base::Json js_to_json(JSContext* ctx, JSValueConst value) {
    if (JS_IsUndefined(value))
        return {};
    const JSValue text = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(text) || JS_IsUndefined(text)) {
        JS_FreeValue(ctx, JS_GetException(ctx)); // cyclic or unserializable: null
        JS_FreeValue(ctx, text);
        return {};
    }
    const std::string dumped = to_std_string(ctx, text);
    JS_FreeValue(ctx, text);
    base::Json::ParseResult parsed = base::Json::parse(dumped, "<script>");
    return parsed.error ? base::Json() : std::move(parsed.value);
}

} // namespace

struct ScriptRuntime::Impl {
    RuntimeConfig config;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    std::uint64_t gas_used = 0;
    bool interrupted = false;
    ModuleResolver resolver;
    std::vector<std::pair<std::string, HostFn>> hooks;
    // Normalize resolves+stashes module sources; the loader compiles them.
    std::vector<std::pair<std::string, std::string>> module_sources;

    [[nodiscard]] const std::string* find_source(std::string_view resolved) const {
        for (const auto& [name, source] : module_sources)
            if (name == resolved)
                return &source;
        return nullptr;
    }

    // The thrown JS value -> structured error. Frees `exception`.
    base::Error error_from_value(JSValue exception) {
        base::Error error;
        if (interrupted) {
            interrupted = false;
            error.code = "script.interrupted";
            error.message = "script exceeded its interrupt budget (" +
                            std::to_string(config.gas_limit) +
                            " gas; deterministic VM-poll units, never wall clock)";
            error.details.set("gas_limit", static_cast<std::int64_t>(config.gas_limit));
            error.details.set("gas_used", static_cast<std::int64_t>(gas_used));
            JS_FreeValue(ctx, exception);
            return error;
        }
        std::string name;
        std::string message;
        std::string stack;
        if (JS_IsError(exception) || JS_IsObject(exception)) {
            name = property_string(ctx, exception, "name");
            message = property_string(ctx, exception, "message");
            stack = property_string(ctx, exception, "stack");
        }
        if (message.empty())
            message = to_std_string(ctx, exception);
        if (message == "out of memory")
            error.code = "script.out_of_memory";
        else if (name == "SyntaxError")
            error.code = "script.syntax";
        else
            error.code = "script.exception";
        std::string file;
        std::int64_t line = 0;
        std::int64_t col = 0;
        const bool located = parse_top_frame(stack, file, line, col);
        const std::string label = name.empty() ? std::string("Error") : name;
        error.message = located ? file + ":" + std::to_string(line) + ":" + std::to_string(col) +
                                      ": " + label + ": " + message
                                : label + ": " + message;
        if (located) {
            error.details.set("file", file);
            error.details.set("line", line);
            error.details.set("col", col);
        }
        if (!stack.empty())
            error.details.set("stack", stack);
        JS_FreeValue(ctx, exception);
        return error;
    }

    base::Error take_exception() { return error_from_value(JS_GetException(ctx)); }

    // Run queued microtasks to completion (module evaluation, promises).
    std::optional<base::Error> drain_jobs() {
        for (;;) {
            JSContext* job_ctx = nullptr;
            const int state = JS_ExecutePendingJob(rt, &job_ctx);
            if (state == 0)
                return std::nullopt;
            if (state < 0)
                return error_from_value(JS_GetException(job_ctx != nullptr ? job_ctx : ctx));
        }
    }
};

struct RuntimeBridge {
    static int interrupt(JSRuntime* /*rt*/, void* opaque) {
        auto* impl = static_cast<ScriptRuntime::Impl*>(opaque);
        ++impl->gas_used;
        if (impl->config.gas_limit != 0 && impl->gas_used > impl->config.gas_limit) {
            impl->interrupted = true;
            return 1;
        }
        return 0;
    }

    static char*
    normalize(JSContext* ctx, const char* base_name, const char* specifier, void* opaque) {
        auto* impl = static_cast<ScriptRuntime::Impl*>(opaque);
        std::optional<ModuleSource> resolved;
        if (impl->resolver)
            resolved = impl->resolver(specifier, base_name);
        if (!resolved) {
            JS_ThrowReferenceError(ctx,
                                   "script.module_not_found: cannot resolve '%s' (imported from "
                                   "'%s'); scripts only load modules the engine serves",
                                   specifier,
                                   base_name);
            return nullptr;
        }
        if (impl->find_source(resolved->resolved) == nullptr)
            impl->module_sources.emplace_back(resolved->resolved, std::move(resolved->js_source));
        const std::string& name = resolved->resolved;
        auto* copy = static_cast<char*>(js_malloc(ctx, name.size() + 1));
        if (copy != nullptr)
            std::memcpy(copy, name.c_str(), name.size() + 1);
        return copy;
    }

    static JSModuleDef* load(JSContext* ctx, const char* module_name, void* opaque) {
        auto* impl = static_cast<ScriptRuntime::Impl*>(opaque);
        const std::string* source = impl->find_source(module_name);
        if (source == nullptr) {
            JS_ThrowReferenceError(
                ctx, "script.module_not_found: no source registered for '%s'", module_name);
            return nullptr;
        }
        const JSValue compiled = JS_Eval(ctx,
                                         source->c_str(),
                                         source->size(),
                                         module_name,
                                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled))
            return nullptr;
        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JS_FreeValue(ctx, compiled); // module defs are owned by the context
        return module;
    }

    static JSValue
    host_call(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv, int magic) {
        auto* impl = static_cast<ScriptRuntime::Impl*>(JS_GetContextOpaque(ctx));
        const auto& [name, fn] = impl->hooks[static_cast<std::size_t>(magic)];
        base::Json::Array args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i)
            args.push_back(js_to_json(ctx, argv[i]));
        const HostResult result = fn(args);
        if (result.error)
            return JS_ThrowTypeError(
                ctx, "%s: %s", result.error->code.c_str(), result.error->message.c_str());
        return json_to_js(ctx, result.value);
    }
};

ScriptRuntime::ScriptRuntime(RuntimeConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->rt = JS_NewRuntime();
    JS_SetMemoryLimit(impl_->rt, config.memory_limit_bytes);
    JS_SetMaxStackSize(impl_->rt, config.stack_size_bytes);
    JS_SetRuntimeOpaque(impl_->rt, impl_.get());
    JS_SetInterruptHandler(impl_->rt, &RuntimeBridge::interrupt, impl_.get());
    JS_SetModuleLoaderFunc(impl_->rt, &RuntimeBridge::normalize, &RuntimeBridge::load, impl_.get());
    impl_->ctx = JS_NewContext(impl_->rt);
    JS_SetContextOpaque(impl_->ctx, impl_.get());
    if (config.deterministic) {
        const JSValue result = JS_Eval(impl_->ctx,
                                       kSimPrelude.data(),
                                       kSimPrelude.size(),
                                       "<midday:sim-prelude>",
                                       JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(impl_->ctx, result); // cannot throw: fixed first-party source
    }
}

ScriptRuntime::~ScriptRuntime() {
    JS_FreeContext(impl_->ctx);
    JS_FreeRuntime(impl_->rt);
}

void ScriptRuntime::set_module_resolver(ModuleResolver resolver) {
    impl_->resolver = std::move(resolver);
}

void ScriptRuntime::register_host_fn(std::string name, HostFn fn) {
    std::size_t index = impl_->hooks.size();
    for (std::size_t i = 0; i < impl_->hooks.size(); ++i)
        if (impl_->hooks[i].first == name)
            index = i;
    if (index == impl_->hooks.size())
        impl_->hooks.emplace_back(std::move(name), std::move(fn));
    else
        impl_->hooks[index].second = std::move(fn);
    JSContext* ctx = impl_->ctx;
    const JSValue global = JS_GetGlobalObject(ctx);
    const JSValue value = JS_NewCFunctionMagic(ctx,
                                               &RuntimeBridge::host_call,
                                               impl_->hooks[index].first.c_str(),
                                               0,
                                               JS_CFUNC_generic_magic,
                                               static_cast<int>(index));
    JS_SetPropertyStr(ctx, global, impl_->hooks[index].first.c_str(), value);
    JS_FreeValue(ctx, global);
}

std::optional<base::Error> ScriptRuntime::eval_global(std::string_view source,
                                                      std::string_view filename) {
    JSContext* ctx = impl_->ctx;
    const JSValue result = JS_Eval(
        ctx, source.data(), source.size(), std::string(filename).c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
        return impl_->take_exception();
    JS_FreeValue(ctx, result);
    return impl_->drain_jobs();
}

EvalResult ScriptRuntime::call_json(std::string_view fn, const base::Json& argument) {
    JSContext* ctx = impl_->ctx;
    const JSValue global = JS_GetGlobalObject(ctx);
    const JSValue callee = JS_GetPropertyStr(ctx, global, std::string(fn).c_str());
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, callee)) {
        JS_FreeValue(ctx, callee);
        base::Error error{.code = "script.host",
                          .message = "globalThis." + std::string(fn) + " is not a function",
                          .details = base::Json::object()};
        return {base::Json(), std::move(error)};
    }
    JSValue arg = json_to_js(ctx, argument);
    const JSValue result = JS_Call(ctx, callee, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, callee);
    if (JS_IsException(result))
        return {base::Json(), impl_->take_exception()};
    if (auto error = impl_->drain_jobs()) {
        JS_FreeValue(ctx, result);
        return {base::Json(), std::move(error)};
    }
    base::Json value = js_to_json(ctx, result);
    JS_FreeValue(ctx, result);
    return {std::move(value), std::nullopt};
}

ScriptRuntime::LoadedModule ScriptRuntime::load_module(std::string_view specifier) {
    std::optional<ModuleSource> resolved;
    if (impl_->resolver)
        resolved = impl_->resolver(specifier, "");
    if (!resolved) {
        base::Error error{.code = "script.module_not_found",
                          .message = "cannot resolve module '" + std::string(specifier) +
                                     "': no module resolver serves it",
                          .details = base::Json::object()};
        error.details.set("specifier", specifier);
        return {std::string(), std::move(error)};
    }
    std::string name = resolved->resolved;
    if (impl_->find_source(name) == nullptr)
        impl_->module_sources.emplace_back(name, std::move(resolved->js_source));
    JSContext* ctx = impl_->ctx;
    const std::string* source = impl_->find_source(name);
    const JSValue result =
        JS_Eval(ctx, source->c_str(), source->size(), name.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result))
        return {std::move(name), impl_->take_exception()};
    if (auto error = impl_->drain_jobs()) {
        JS_FreeValue(ctx, result);
        return {std::move(name), std::move(error)};
    }
    std::optional<base::Error> error;
    if (JS_IsPromise(result)) {
        switch (JS_PromiseState(ctx, result)) {
        case JS_PROMISE_REJECTED:
            error = impl_->error_from_value(JS_PromiseResult(ctx, result));
            break;
        case JS_PROMISE_PENDING:
            error = base::Error{.code = "script.exception",
                                .message = "module '" + name +
                                           "' did not settle (top-level await never resolves "
                                           "inside the sim)",
                                .details = base::Json::object()};
            break;
        default:
            break; // fulfilled
        }
    }
    JS_FreeValue(ctx, result);
    return {std::move(name), std::move(error)};
}

std::uint64_t ScriptRuntime::gas_used() const {
    return impl_->gas_used;
}

void annotate_sim_context(base::Error& error,
                          std::uint64_t tick,
                          std::string_view replay_bookmark) {
    error.details.set("tick", static_cast<std::int64_t>(tick));
    error.details.set("replay_bookmark", replay_bookmark);
}

} // namespace midday::script

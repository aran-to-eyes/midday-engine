// ts/codegen/selfhost.cpp — runs ts/codegen/generator.ts on the embedded
// QuickJS: toolchain-built through the content-hash cache, host hooks
// {readInput, writeOutput, log}, one call_json round trip. All generation
// logic lives in TypeScript; this file only moves bytes across the seam.

#include "ts/codegen/selfhost.h"

#include "core/base/json.h"
#include "ts/runtime/script_runtime.h"
#include "ts/toolchain/toolchain.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace midday::selfhost {

using base::Error;
using base::Json;

namespace {

constexpr std::string_view kArtifactNames[] = {
    "engine.d.ts", "schema_manifest.json", "api_docs.md", "bindings_spec.json"};

Error host_misuse(std::string message) {
    return Error{.code = "script.host", .message = std::move(message)};
}

// The generator's {ok:false, error:{code,message,details}} response -> Error.
Error error_from_response(const Json* error_json) {
    if (error_json == nullptr || !error_json->is_object())
        return host_misuse("generator response carried ok:false without an error object");
    Error error;
    if (const Json* code = error_json->find("code"); code != nullptr && code->is_string())
        error.code = code->as_string();
    if (const Json* message = error_json->find("message");
        message != nullptr && message->is_string())
        error.message = message->as_string();
    if (const Json* details = error_json->find("details");
        details != nullptr && details->is_object())
        error.details = *details;
    if (error.code.empty())
        return host_misuse("generator error response without a code");
    return error;
}

} // namespace

std::string bindings_equivalence_view(std::string_view bindings_bytes) {
    Json::ParseResult parsed = Json::parse(bindings_bytes, "<bindings_spec>");
    if (parsed.error || !parsed.value.is_object() || parsed.value.find("batch_envelope") == nullptr)
        return std::string(bindings_bytes);
    // batch_envelope: both generators emit the member (bootstrap froze on
    // the version-0 placeholder), so it nulls. state_script_hooks: the
    // frozen bootstrap never emits it at all, so it DROPS — the view keeps
    // the two artifacts member-for-member comparable without teaching the
    // temporary bootstrap every new selfhost-only seam (D-BUILD-084).
    Json view = Json::object();
    for (const auto& [key, value] : parsed.value.items()) {
        if (key == "batch_envelope")
            view.set(key, Json());
        else if (key != "state_script_hooks")
            view.set(key, value);
    }
    return view.dump() + "\n";
}

RunResult run_generator(std::string_view input_bytes, const Config& config) {
    RunResult out;

    // SIM profile on purpose: the generator is pure text -> text, so the
    // poisoned clock/random doors cost nothing and prove determinism.
    script::ScriptRuntime runtime;
    const std::string input(input_bytes);
    std::vector<std::pair<std::string, std::string>> written;
    runtime.register_host_fn("readInput", [&input](const Json::Array& args) {
        script::HostResult result;
        if (!args.empty())
            result.error = host_misuse("readInput takes no arguments");
        else
            result.value = Json(input);
        return result;
    });
    runtime.register_host_fn("writeOutput", [&written](const Json::Array& args) {
        script::HostResult result;
        if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
            result.error = host_misuse("writeOutput expects (name, content) strings");
        else
            written.emplace_back(args[0].as_string(), args[1].as_string());
        return result;
    });
    runtime.register_host_fn("log", [](const Json::Array& args) {
        std::string line;
        for (const Json& part : args) {
            if (!line.empty())
                line += ' ';
            line += part.is_string() ? part.as_string() : part.dump();
        }
        std::fprintf(stderr, "codegen-selfhost: %s\n", line.c_str());
        return script::HostResult{};
    });

    script::ToolchainConfig tool_config;
    tool_config.engine_dts = config.host_dts; // tool profile: host API, not engine.d.ts
    tool_config.cache_dir = config.cache_dir;
    script::Toolchain toolchain(std::move(tool_config));
    script::Toolchain::LoadOutcome loaded = toolchain.load_module(runtime, config.entry);
    if (loaded.error) {
        out.error = std::move(loaded.error);
        return out;
    }

    Json request = Json::object();
    request.set("origin", config.origin);
    script::EvalResult result = runtime.call_json("__midday_codegen_run", request);
    if (result.error) {
        out.error = std::move(result.error);
        return out;
    }
    const Json* ok = result.value.find("ok");
    if (ok == nullptr || !ok->is_bool()) {
        out.error = host_misuse("generator response is not an {ok, ...} object");
        return out;
    }
    if (!ok->as_bool()) {
        out.error = error_from_response(result.value.find("error"));
        return out;
    }
    if (const Json* hash = result.value.find("api_compat_hash");
        hash != nullptr && hash->is_string())
        out.files.api_compat_hash = hash->as_string();

    // Exactly the four artifacts, each exactly once — anything else is a
    // generator bug, same class as the bootstrap tool's post-run self-check.
    std::string* slots[] = {
        &out.files.dts, &out.files.manifest, &out.files.docs, &out.files.bindings};
    bool seen[std::size(kArtifactNames)] = {};
    for (auto& [name, content] : written) {
        std::size_t index = std::size(kArtifactNames);
        for (std::size_t i = 0; i < std::size(kArtifactNames); ++i)
            if (name == kArtifactNames[i])
                index = i;
        if (index == std::size(kArtifactNames) || seen[index]) {
            out.error =
                Error{.code = "codegen.selfcheck",
                      .message = "generator wrote unexpected or duplicate artifact '" + name + "'"};
            return out;
        }
        seen[index] = true;
        *slots[index] = std::move(content);
    }
    for (std::size_t i = 0; i < std::size(kArtifactNames); ++i)
        if (!seen[i]) {
            out.error = Error{.code = "codegen.selfcheck",
                              .message = "generator did not write '" +
                                         std::string(kArtifactNames[i]) + "'"};
            return out;
        }
    return out;
}

} // namespace midday::selfhost

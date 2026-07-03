// tools/codegen_bootstrap/main.cpp — TEMPORARY bootstrap generator CLI:
// argv + file I/O only; all logic lives in the selftest-covered library.
// Usage/exit contract: README.md + api/CODEGEN.md. Errors are structured
// JSON on stdout (D-BUILD-038); success prints a one-line envelope.

#include "core/base/file_io.h"
#include "tools/codegen_bootstrap/codegen.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using midday::base::Error;
using midday::base::Json;
namespace codegen = midday::codegen;

constexpr std::string_view kUsage =
    R"(codegen_bootstrap [<engine_api.json>] [--out-dir <dir>]

TEMPORARY native bootstrap generator (m0-codegen-bootstrap): parses
engine_api.json (default: api/engine_api.json; `midday api dump --json`
envelopes are unwrapped) and emits engine.d.ts, schema_manifest.json,
api_docs.md, and bindings_spec.json to --out-dir (default: api). Byte
deterministic. The SELF-HOSTED generator (`midday api codegen`) is
authoritative since m0-codegen-selfhost; this tool remains only as the
byte-equivalence pin until it retires post-M0.

Exit: 0 ok, 1 write/self-check failure, 2 usage, 3 invalid input.
Formatting contract: api/CODEGEN.md.
)";

int fail(const Error& error) {
    Json envelope = Json::object();
    envelope.set("ok", false);
    envelope.set("error", error.to_json());
    std::printf("%s\n", envelope.dump().c_str());
    return codegen::exit_code_for(error);
}

int fail_usage(std::string message) {
    return fail(Error{.code = "usage.arguments", .message = std::move(message)});
}

struct Args {
    std::string input = "api/engine_api.json";
    std::string out_dir = "api";
    bool help = false;
    bool saw_input = false;
};

int run_tool(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--out-dir") {
            if (i + 1 == argc)
                return fail_usage("--out-dir needs a value");
            args.out_dir = argv[++i];
        } else if (arg.starts_with("--out-dir=")) {
            args.out_dir = arg.substr(std::string_view("--out-dir=").size());
        } else if (arg.starts_with("-")) {
            return fail_usage("unknown flag '" + std::string(arg) + "' (see: --help)");
        } else if (!args.saw_input) {
            args.input = arg;
            args.saw_input = true;
        } else {
            return fail_usage("unexpected argument '" + std::string(arg) + "'");
        }
    }
    if (args.help) {
        std::fputs(std::string(kUsage).c_str(), stdout);
        return 0;
    }

    auto [bytes, read_error] = midday::base::read_file(args.input, "codegen.io");
    if (read_error)
        return fail(*read_error);

    codegen::LoadResult loaded = codegen::load_document(bytes, args.input);
    if (loaded.error)
        return fail(*loaded.error);

    const codegen::Outputs outputs = codegen::generate(loaded.document);

    // Post-generation structural self-check (formats/engine_dts.meta.md):
    // a failure here is a generator bug, never bad input — exit 1.
    if (const auto shape = codegen::dts_shape_errors(outputs.dts, loaded.document);
        !shape.empty()) {
        Error error{.code = "codegen.selfcheck",
                    .message = "generated engine.d.ts failed its structural shape check"};
        Json list = Json::array();
        for (const std::string& problem : shape)
            list.push(problem);
        error.details.set("errors", std::move(list));
        return fail(error);
    }

    const std::filesystem::path out_dir(args.out_dir);
    std::error_code fs_error;
    std::filesystem::create_directories(out_dir, fs_error);
    if (fs_error)
        return fail(
            Error{.code = "codegen.io.write",
                  .message = "cannot create out-dir " + args.out_dir + ": " + fs_error.message()});

    const std::pair<const char*, const std::string&> files[] = {
        {"engine.d.ts", outputs.dts},
        {"schema_manifest.json", outputs.manifest},
        {"api_docs.md", outputs.docs},
        {"bindings_spec.json", outputs.bindings},
    };
    Json written = Json::array();
    for (const auto& [name, content] : files) {
        if (auto error = midday::base::write_file(out_dir / name, content, "codegen.io.write"))
            return fail(*error);
        written.push(name);
    }

    Json envelope = Json::object();
    envelope.set("ok", true);
    envelope.set("out_dir", args.out_dir);
    envelope.set("api_compat_hash", loaded.document.find("api_compat_hash")->as_string());
    envelope.set("files", std::move(written));
    std::printf("%s\n", envelope.dump().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Structured-error discipline lives in run_tool(); this is the last-resort
    // backstop so an escaping exception still yields a deterministic exit
    // code, never std::terminate (cli/main.cpp precedent).
    try {
        return run_tool(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "codegen_bootstrap: fatal: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fputs("codegen_bootstrap: fatal: unknown exception\n", stderr);
        return 1;
    }
}

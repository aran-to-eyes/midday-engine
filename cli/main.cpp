// midday — the engine's canonical interface (spec section 9).
// Every verb: JSON envelope on stdout (formats/cli_envelope.schema.json),
// exit codes 0 ok / 1 failure / 2 usage / 3 validation, headless-first.
// The framework parses/validates argv against each verb's declared schema
// (cli/verb.h) and generates help (cli/help.h); verbs only ever run on
// already-typed arguments.

#include "cli/envelope.h"
#include "cli/help.h"
#include "cli/verb.h"

#include <cstdio>
#include <exception>
#include <string_view>

namespace midday::cli {
namespace {

int emit(std::string_view verb_name, const VerbOutcome& out, bool json) {
    const bool failed = out.exit != Exit::Ok;
    const Error* error = out.error ? &*out.error : nullptr;
    if (json || failed) {
        // Errors ALWAYS emit the machine envelope on stdout, --json or not
        // (agent-first, D-BUILD-038); success stays human unless --json.
        Json env = make_envelope(verb_name, out.exit, out.payload, error);
        std::fputs(env.dump().c_str(), stdout);
        std::fputc('\n', stdout);
    }
    if (!json) {
        if (failed) {
            const Error fallback{.code = "internal.unspecified",
                                 .message = "verb failed without a structured error"};
            const Error& e = error != nullptr ? *error : fallback;
            std::fprintf(stderr,
                         "midday %.*s: error [%s] %s\n",
                         static_cast<int>(verb_name.size()),
                         verb_name.data(),
                         e.code.c_str(),
                         e.message.c_str());
        } else if (!out.human.empty()) {
            std::fprintf(stdout, "%s\n", out.human.c_str());
        }
    }
    return static_cast<int>(out.exit);
}

VerbOutcome usage_outcome(Error error) {
    VerbOutcome out;
    out.exit = Exit::Usage;
    out.error = std::move(error);
    return out;
}

int run(int argc, char** argv) {
    const Invocation inv = split_invocation(argc, argv);
    if (inv.usage)
        return emit(inv.verb.empty() ? "-" : inv.verb, usage_outcome(*inv.usage), inv.json);

    if (inv.verb.empty()) {
        if (inv.help)
            return emit("help", help_overview(), inv.json);
        Error error{.code = "usage.missing_verb",
                    .message = "usage: midday <verb> [flags] [args] [--json] (see: midday help)"};
        VerbOutcome out = usage_outcome(std::move(error));
        Json names = Json::array();
        for (const VerbSpec* spec : verbs())
            names.push(spec->name);
        out.payload.set("verbs", std::move(names));
        return emit("-", out, inv.json);
    }

    const VerbSpec* spec = find_verb(inv.verb);
    if (spec == nullptr)
        return emit(inv.verb, usage_unknown_verb(inv.verb), inv.json);

    ParsedArgs parsed = parse_verb_args(*spec, inv);
    if (parsed.help)
        return emit(spec->name, help_for(*spec), parsed.args.json);
    if (parsed.usage)
        return emit(spec->name, usage_outcome(std::move(*parsed.usage)), parsed.args.json);
    return emit(spec->name, spec->run(parsed.args), parsed.args.json);
}

} // namespace
} // namespace midday::cli

int main(int argc, char** argv) {
    // Envelope discipline lives in run(); this is the last-resort backstop so an
    // escaping exception still yields a deterministic exit code, never std::terminate.
    try {
        return midday::cli::run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "midday: fatal: %s\n", e.what());
        return static_cast<int>(midday::cli::Exit::Failure);
    } catch (...) {
        std::fputs("midday: fatal: unknown exception\n", stderr);
        return static_cast<int>(midday::cli::Exit::Failure);
    }
}

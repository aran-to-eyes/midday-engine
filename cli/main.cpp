// midday — the engine's canonical interface (spec section 9).
// Every verb: JSON envelope on stdout (formats/cli_envelope.schema.json),
// exit codes 0 ok / 1 failure / 2 usage / 3 validation, headless-first.

#include "cli/envelope.h"
#include "cli/verb.h"

#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

namespace midday::cli {
namespace {

int emit(std::string_view verb_name, const VerbOutcome& out, bool json) {
    const Error* error = out.error ? &*out.error : nullptr;
    if (json) {
        Json env = make_envelope(verb_name, out.exit, out.payload, error);
        std::fputs(env.dump().c_str(), stdout);
        std::fputc('\n', stdout);
    } else if (out.exit != Exit::Ok) {
        const Error fallback{.code = "internal.unspecified",
                             .message = "verb failed without a structured error"};
        const Error& e = error ? *error : fallback;
        std::fprintf(stderr,
                     "midday %.*s: error [%s] %s\n",
                     int(verb_name.size()),
                     verb_name.data(),
                     e.code.c_str(),
                     e.message.c_str());
    } else if (!out.human.empty()) {
        std::fprintf(stdout, "%s\n", out.human.c_str());
    }
    return static_cast<int>(out.exit);
}

int run(int argc, char** argv) {
    VerbArgs args;
    std::string_view verb_name;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--json") {
            args.json = true; // accepted before or after the verb
        } else if (verb_name.empty()) {
            verb_name = arg;
        } else {
            args.rest.push_back(arg);
        }
    }

    if (verb_name.empty()) {
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = Error{.code = "usage.missing_verb",
                          .message = "usage: midday <verb> [--json] [args...]"};
        Json names = Json::array();
        for (const Verb& v : verbs())
            names.push(v.name);
        out.payload.set("verbs", std::move(names));
        return emit("-", out, args.json);
    }

    const Verb* verb = find_verb(verb_name);
    if (verb == nullptr) {
        VerbOutcome out;
        out.exit = Exit::Usage;
        Error error{.code = "usage.unknown_verb", .message = "unknown verb"};
        error.details.set("verb", verb_name);
        out.error = std::move(error);
        Json names = Json::array();
        for (const Verb& v : verbs())
            names.push(v.name);
        out.payload.set("verbs", std::move(names));
        return emit(verb_name, out, args.json);
    }

    return emit(verb_name, verb->run(args), args.json);
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

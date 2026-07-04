// cli/verbs/registry.cpp — THE verb manifest. Adding a verb: one new file
// under cli/verbs/ plus one line here (and its CMake source entry); the
// framework itself never changes (D-BUILD-037). Manifest order is the
// canonical order for the verb list, help, and engine_api.json — explicit
// declaration order, never link order, so the bytes cannot drift across
// platforms or build systems.

#include "cli/verb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace midday::cli {
namespace {

std::span<const VerbSpec* const> manifest() {
    static const VerbSpec* const kManifest[] = {
        &version_spec(),
        &selftest_spec(),
        &help_spec(),
        &api_spec(),
        &script_spec(),
        &run_spec(),
        &journal_spec(),
    };
    return kManifest;
}

// Every spec is validated once, before the first lookup; a malformed spec is
// a build defect and aborts loudly (reflect registration precedent,
// D-BUILD-023) — user input never reaches an invalid schema.
bool validate_registry() {
    std::vector<std::string_view> names;
    for (const VerbSpec* spec : manifest()) {
        if (const std::optional<Error> error = validate_spec(*spec)) {
            std::fprintf(stderr,
                         "midday: fatal: cli: [%s] %s\n",
                         error->code.c_str(),
                         error->message.c_str());
            std::abort();
        }
        if (std::ranges::find(names, spec->name) != names.end()) {
            std::fprintf(stderr,
                         "midday: fatal: cli: duplicate verb '%.*s' in the manifest\n",
                         static_cast<int>(spec->name.size()),
                         spec->name.data());
            std::abort();
        }
        names.push_back(spec->name);
    }
    return true;
}

} // namespace

std::span<const VerbSpec* const> verbs() {
    [[maybe_unused]] static const bool kValidated = validate_registry();
    return manifest();
}

const VerbSpec* find_verb(std::string_view name) {
    for (const VerbSpec* spec : verbs())
        if (spec->name == name)
            return spec;
    return nullptr;
}

VerbOutcome usage_unknown_verb(std::string_view name) {
    VerbOutcome out;
    out.exit = Exit::Usage;
    Error error{.code = "usage.unknown_verb",
                .message = "unknown verb '" + std::string(name) + "' (see: midday help)"};
    error.details.set("verb", name);
    out.error = std::move(error);
    Json names = Json::array();
    for (const VerbSpec* spec : verbs())
        names.push(spec->name);
    out.payload.set("verbs", std::move(names));
    return out;
}

} // namespace midday::cli

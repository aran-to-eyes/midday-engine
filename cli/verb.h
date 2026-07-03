// Verb framework stub (m0-repo-build). The full registry with generated help
// and per-verb flag schemas lands at m0-cli-framework; the shape here — one
// function per verb, envelope-only output — is permanent.

#pragma once

#include "cli/envelope.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace midday::cli {

struct VerbArgs {
    std::vector<std::string_view> rest; // arguments after the verb, minus global flags
    bool json = false;                  // --json requested (before or after the verb)
};

struct VerbOutcome {
    Exit exit = Exit::Ok;
    Json payload = Json::object(); // verb-specific top-level envelope fields
    std::optional<Error> error;    // required by the schema whenever exit != Ok
    std::string human;             // one-line summary for non-JSON mode
};

using VerbFn = VerbOutcome (*)(const VerbArgs&);

struct Verb {
    std::string_view name;
    std::string_view summary;
    VerbFn run;
};

std::span<const Verb> verbs();
const Verb* find_verb(std::string_view name);

// Verb implementations (one translation unit per verb under cli/verbs/).
VerbOutcome verb_version(const VerbArgs& args);
VerbOutcome verb_selftest(const VerbArgs& args);

} // namespace midday::cli

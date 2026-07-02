#include "cli/verb.h"

namespace midday::cli {

std::span<const Verb> verbs() {
    static constexpr Verb kVerbs[] = {
        {.name = "version",
         .summary = "print engine name, version, and build info",
         .run = &verb_version},
        {.name = "selftest",
         .summary = "run the embedded doctest registry (--filter <pattern>)",
         .run = &verb_selftest},
    };
    return kVerbs;
}

const Verb* find_verb(std::string_view name) {
    for (const Verb& verb : verbs()) {
        if (verb.name == name)
            return &verb;
    }
    return nullptr;
}

} // namespace midday::cli

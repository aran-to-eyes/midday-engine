// `midday help [verb]` — the generated-help verb. The verb list and every
// per-verb schema come from cli/help.h generators; nothing here is
// hand-written per verb, so new verbs appear in help automatically.

#include "cli/help.h"

#include "cli/verb.h"

namespace midday::cli {
namespace {

constexpr PositionalSpec kPositionals[] = {
    {.name = "verb",
     .type = "name",
     .doc = "verb to describe; omit for the full verb list",
     .required = false},
};

VerbOutcome verb_help(const VerbArgs& args) {
    if (!args.present("verb"))
        return help_overview();
    const std::string& name = args.get_string("verb");
    const VerbSpec* spec = find_verb(name);
    if (spec == nullptr)
        return usage_unknown_verb(name);
    return help_for(*spec);
}

} // namespace

const VerbSpec& help_spec() {
    static constexpr VerbSpec kSpec{.name = "help",
                                    .summary = "show the verb list or one verb's flags and usage",
                                    .positionals = kPositionals,
                                    .run = &verb_help};
    return kSpec;
}

} // namespace midday::cli

// cli/help.h — generated help. Both the machine schema (--json) and the
// human text are pure functions of VerbSpec metadata in declaration order;
// nothing is hand-written per verb. verb_schema() is the exact JSON that
// m0-api-json embeds per verb in engine_api.json.

#pragma once

#include "cli/verb.h"

namespace midday::cli {

// {"name","summary","flags":[{name,type,required[,default],doc}...],
//  "positionals":[{name,type,required,variadic,doc}...]}
Json verb_schema(const VerbSpec& spec);

// Schemas of the framework globals (--json, --help).
Json global_flags_json();

// Ready-to-emit outcomes: payload = schema, human = generated help text.
VerbOutcome help_for(const VerbSpec& spec); // one verb
VerbOutcome help_overview();                // all verbs + global flags

} // namespace midday::cli

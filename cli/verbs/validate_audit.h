// cli/verbs/validate_audit.h — `midday validate <dir> --audit-missing`
// (m1-warden-contract-audit). Split out of validate.cpp to hold the
// 500-line ratchet (core/loader/scene_ctx.h's precedent) — same verb, one
// registration, two translation units.
#pragma once

#include "cli/verb.h"

#include <string>

namespace midday::cli {

// See validate_audit.cpp's file-top comment for the full design: the
// known-completion-manifest audit over a Warden-shaped example directory
// (present files validate; missing referenced files/components/wiring
// report, never a refusal).
VerbOutcome run_audit_missing(const std::string& root);

} // namespace midday::cli

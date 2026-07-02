// The CLI JSON envelope — the single C++ counterpart of
// formats/cli_envelope.schema.json. Every verb's --json output goes through
// make_envelope(); no verb hand-rolls envelope fields.

#pragma once

#include "cli/json.h"

#include <string>
#include <string_view>

namespace midday::cli {

// Spec section 9 exit codes (plan m0-cli-framework):
//   0 ok · 1 failed assertion/build/runtime · 2 usage · 3 validation
enum class Exit : int {
    Ok = 0,
    Failure = 1,
    Usage = 2,
    Validation = 3,
};

struct Error {
    std::string code;    // stable dotted identifier, e.g. "usage.unknown_verb"
    std::string message; // one-line human summary
    Json details = Json::object();
};

// Builds a schema-valid envelope:
//   { "ok": ..., "verb": ..., "exit_code": ..., ["error": {...},] ...payload }
// Invariants are enforced here, not trusted from callers:
//   * ok == (exit == Exit::Ok)
//   * error present iff !ok (synthesized if the caller failed without one)
//   * payload fields never shadow envelope fields
Json make_envelope(std::string_view verb, Exit exit, const Json& payload, const Error* error);

} // namespace midday::cli

// The CLI JSON envelope — the single C++ counterpart of
// formats/cli_envelope.schema.json. Every verb's --json output goes through
// make_envelope(); no verb hand-rolls envelope fields.
//
// JSON and the structured Error live in core/base (m0-core-primitives);
// the CLI adapts them — it defines only the envelope layout and exit codes.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"

#include <cstdint>
#include <string_view>

namespace midday::cli {

using Json = base::Json;
using Error = base::Error;

// Spec section 9 exit codes (plan m0-cli-framework):
//   0 ok · 1 failed assertion/build/runtime · 2 usage · 3 validation
enum class Exit : std::uint8_t {
    Ok = 0,
    Failure = 1,
    Usage = 2,
    Validation = 3,
};

// Builds a schema-valid envelope:
//   { "ok": ..., "verb": ..., "exit_code": ..., ["error": {...},] ...payload }
// Invariants are enforced here, not trusted from callers:
//   * ok == (exit == Exit::Ok)
//   * error present iff !ok (synthesized if the caller failed without one)
//   * payload fields never shadow envelope fields
Json make_envelope(std::string_view verb, Exit exit, const Json& payload, const Error* error);

} // namespace midday::cli

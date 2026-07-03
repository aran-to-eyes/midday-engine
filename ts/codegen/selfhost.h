// ts/codegen/selfhost.h — the self-hosted code generator's C++ seam
// (m0-codegen-selfhost). The REAL generator lives in ts/codegen/*.ts and
// runs on the embedded QuickJS (SIM profile: it provably cannot read the
// clock); this runner builds it through the TS toolchain's content-hash
// cache, wires the three host hooks {readInput, writeOutput, log}, and
// returns the four artifacts as strings.
//
// Authority: since m0-codegen-selfhost this generator is authoritative for
// the committed api/ artifacts. The native tools/codegen_bootstrap remains
// ONLY as the byte-equivalence pin (`midday api codegen
// --verify-equivalence`, verify.sh, CI drift lane) until it is retired
// post-M0. Byte contract for every formatting rule: api/CODEGEN.md.

#pragma once

#include "core/base/error.h"

#include <optional>
#include <string>
#include <string_view>

namespace midday::selfhost {

struct Config {
    // Repo-root-relative defaults (M0 verbs run from the project root).
    std::string entry = "ts/codegen/generator.ts";
    // The generator is a TOOL: it typechecks against its own host-API
    // declarations, never api/engine.d.ts (no self-dependency loop).
    std::string host_dts = "ts/codegen/host.d.ts";
    std::string cache_dir = ".midday-cache/ts"; // regenerable, gitignored
    std::string origin = "<input>";             // input label for parse errors
};

struct Generated {
    std::string dts;             // engine.d.ts
    std::string manifest;        // schema_manifest.json
    std::string docs;            // api_docs.md
    std::string bindings;        // bindings_spec.json
    std::string api_compat_hash; // copied from the validated input
};

// Error classes (exit mapping in cli/verbs/api.cpp): input problems keep the
// bootstrap codes (json.parse | api.malformed | codegen.unknown_type |
// codegen.malformed | codegen.duplicate_symbol — validation, exit 3);
// codegen.selfcheck / codegen.internal are generator bugs and script.* are
// toolchain/runtime infrastructure (both exit 1).
struct RunResult {
    Generated files;
    std::optional<base::Error> error;
};

RunResult run_generator(std::string_view input_bytes, const Config& config = {});

// Equivalence scope for bindings_spec.json (D-BUILD-069): the batch envelope
// is emitted by the SELF-HOSTED generator only — the bootstrap stays frozen
// on the version-0 placeholder, so the byte-equivalence gate compares
// bindings_spec.json with the "batch_envelope" member replaced by null in
// both artifacts. Every other byte (and every other artifact) stays under
// the full-byte gate. Non-JSON input comes back verbatim (the comparison
// then fails byte-for-byte, which is the right report).
std::string bindings_equivalence_view(std::string_view bindings_bytes);

} // namespace midday::selfhost

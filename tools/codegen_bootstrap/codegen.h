// tools/codegen_bootstrap/codegen.h — the TEMPORARY bootstrap generator
// library (m0-codegen-bootstrap): engine_api.json -> engine.d.ts +
// schema_manifest.json + api_docs.md + bindings_spec.json.
//
// Every formatting rule is specified in api/CODEGEN.md — the byte contract
// the self-hosted TS-on-QuickJS generator (ts/codegen, AUTHORITATIVE since
// m0-codegen-selfhost) reproduces byte-for-byte; this tool survives only as
// the equivalence pin. Keep it deliberately small and cleverness-free.
//
// All functions are pure: same document -> byte-identical strings, every
// platform (pinned by codegen.* selftests, verify.sh, and the CI drift lane).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::codegen {

struct LoadResult {
    base::Json document;
    std::optional<base::Error> error; // json.parse | api.malformed | codegen.*
};

// bytes -> validated format-1 document. Accepts the raw document or a CLI
// envelope with the document under "api" (`midday api dump --json`); then
// api::check_document, then the type-spelling / entry-shape / generated-
// symbol-uniqueness walk (api/CODEGEN.md "Validation order").
LoadResult load_document(std::string_view bytes, std::string_view origin);

struct Outputs {
    std::string dts;      // engine.d.ts
    std::string manifest; // schema_manifest.json
    std::string docs;     // api_docs.md
    std::string bindings; // bindings_spec.json
};

// The four emitters. Pre: `document` passed load_document.
std::string emit_dts(const base::Json& document);
std::string emit_manifest(const base::Json& document);
std::string emit_docs(const base::Json& document);
std::string emit_bindings(const base::Json& document);

Outputs generate(const base::Json& document);

// Structural d.ts shape check (formats/engine_dts.meta.md): balanced braces
// on non-comment lines, every declared entry present, no unresolved-
// generation tokens. Empty result == shape-valid. The tool runs this after
// every generation (failure = codegen.selfcheck, exit 1).
std::vector<std::string> dts_shape_errors(std::string_view dts, const base::Json& document);

// Tool exit classes (api/CODEGEN.md): usage.* -> 2; codegen.io.write /
// codegen.selfcheck -> 1; every other error -> 3 (validation).
int exit_code_for(const base::Error& error);

// Shared text rules (exposed for the codegen.* selftests).
std::string pascal_case(std::string_view name); // "trigger.entered" -> "TriggerEntered"
std::string ts_type(std::string_view spelling); // pre: valid TypeDesc spelling
std::string jsdoc_escape(std::string_view doc); // "*/" -> "*\/", "\n" -> " "
std::string cell_escape(std::string_view text); // "|" -> "\|", "\n" -> " "

} // namespace midday::codegen

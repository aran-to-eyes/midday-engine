// api/engine_api.h — the engine_api.json emitter and differ (m0-api-json).
//
// engine_api.json is THE generated artifact agents and codegen consume: the
// .d.ts generator, the schema manifest, and the agent docs all derive from it
// (m0-codegen-bootstrap). Its emitted shape is a contract — later nodes only
// ADD entries and sections, never reshape (registry precedent).
//
// Ownership call (D-BUILD-040): this lives in api/, not core/reflect, because
// the document composes THREE surfaces — the reflect registry, the expression
// function inventory (registered into the registry at boot), and the CLI verb
// schemas — and core/reflect must not depend on expr or cli. core/reflect
// owns per-entry describe() and compat hashes; api/ owns boot composition,
// document assembly, verb-entry hashes, the top-level hash, and diffing.
//
// Hash contract (extends D-BUILD-021; the drift primitive `midday api diff`
// compares):
//   * per-entry compat_hash = XXH3-64 over the dump() bytes of the entry's
//     signature-only JSON, spelled as 16-digit lowercase hex. Registry
//     entries compute theirs at registration (docs excluded there); verb
//     entries hash cli::verb_schema() with every "doc"/"summary" key
//     stripped recursively.
//   * top-level api_compat_hash = XXH3-64 over the dump() bytes of
//     {"format_version", "classes": [{"name","compat_hash"}...], "events",
//     "functions", "verbs"} in canonical order. IN the hash: the format
//     version, section order, entry order, entry names, and every per-entry
//     signature hash (hence all types, defaults, flags, params, returns,
//     init levels). OUT: engine_version and every doc/summary string — doc
//     edits and releases are not API drift.
// Byte determinism: the document is a pure function of (registry contents,
// verb manifest, engine_version) through the core JSON writer — two dumps
// are byte-identical on every platform (pinned by api.* selftests and the
// verify/CI drift lanes against the committed api/engine_api.json).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/reflect/init_levels.h"
#include "core/reflect/registry.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace midday::api {

// Bumped only when the DOCUMENT SHAPE changes (with a documented migration,
// spec section 8 versioning policy) — never for content growth.
inline constexpr std::int64_t kFormatVersion = 1;

// The canonical fully-initialized registry: the CORE built-in vocabulary
// (events + expression functions) registered, every init level climbed.
// Later nodes append their subsystem hooks HERE — this is the one boot
// composition `midday api dump` and the tests share, until a dedicated
// engine-boot module takes it over.
struct BootRegistry {
    reflect::Registry registry;
    reflect::Lifecycle lifecycle;

    BootRegistry();
};

// Assemble the canonical document from an initialized registry, the CLI verb
// schemas (an array of cli::verb_schema() values, manifest order), and the
// engine version. Key order: format_version, engine_version, api_compat_hash,
// classes, events, functions, verbs.
base::Json build_document(const reflect::Registry& registry,
                          const base::Json& verb_schemas,
                          std::string_view engine_version);

// Structural check that `document` has the format-1 shape diffing relies on
// (top-level fields, sections as arrays, entries with unique names and
// well-formed compat hashes). Full validation lives in
// formats/engine_api.schema.json; this guards `api diff` against garbage
// input with a structured "api.malformed" error instead of UB.
std::optional<base::Error> check_document(const base::Json& document);

// Compat-hash comparison of two format-1 documents (pre: both pass
// check_document). Entry lists are computed per section by name; top-level
// hashes are compared as recorded. `report` is the flat verb payload:
//   {identical, old_api_compat_hash, api_compat_hash,
//    added:   [{kind,name,compat_hash}...],           // new-document order
//    removed: [{kind,name,compat_hash}...],           // old-document order
//    changed: [{kind,name,old_compat_hash,compat_hash}...]}
// identical == top-level hashes equal AND all three lists empty (a pure
// reorder drifts the top-level hash with empty lists — still drift).
struct Diff {
    bool identical = false;
    base::Json report;
};

Diff diff_documents(const base::Json& old_document, const base::Json& new_document);

} // namespace midday::api

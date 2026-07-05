// core/loader/asset_ref.h — the {uid, path} dual-write REFERENCE shape
// (m1-uid-system, spec lines 363-368) as a schema-agnostic YAML pattern:
// recognized structurally, wherever it occurs, exactly like core/loader/
// yaml_emit.h's canonical emitter or format_schema.h's migration ops never
// need to know which FORMAT (scene, machine, or one nobody has invented
// yet) they are working inside. A ref site is a mapping whose key set is
// EXACTLY {"path"} or EXACTLY {"uid", "path"}, both values scalar:
//
//   sprite: {path: assets/sprite.png}                      # legacy, path-only
//   sprite: {uid: uid://0000000000abc, path: assets/sprite.png}  # dual-write
//
// This is deliberately the SAME shape the scene loader's existing
// `instance: {path: ...}` machine reference already uses (core/loader/
// scene_load.cpp) — the mechanism this file establishes is ready for
// m1-scene-format to wire the scene/machine/prefab grammar into (widening
// `instance:`'s allowed inner keys to accept `uid`) without owning any of
// the uid plumbing itself; this node does not make that loader edit (see
// README "scope boundary" — the same judgment call m1-strict-yaml made for
// format_schema.h against a self-contained fixture, not a real scene
// schema).
//
// `midday check`/`midday mv` (cli/verbs/check.cpp, mv.cpp) are the only
// callers: this file has no CLI or filesystem-walk knowledge beyond the
// pure YamlNode tree it is handed and the UidRegistry (core/loader/
// uid_registry.h) it validates against.

#pragma once

#include "core/loader/uid.h"
#include "core/loader/uid_registry.h"
#include "core/loader/yaml.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midday::loader {

// What a ref site's `path` resolves to, and — when present — its `uid`
// scalar. Recursing INTO a matched site is meaningless (the shape is a
// leaf): find_asset_refs() never descends past one.
struct RefFields {
    YamlNode* path = nullptr; // required
    YamlNode* uid = nullptr;  // optional (absent = legacy path-only ref)
};

// nullopt when `node` does not have exactly the ref shape (wrong key set,
// extra keys, or a non-scalar value) — every OTHER map/seq is still walked
// by find_asset_refs, just not treated as a ref.
[[nodiscard]] std::optional<RefFields> match_ref_shape(YamlNode& node);

// A ref site found in one parsed document, ready to classify or rewrite.
struct RefSite {
    YamlNode* map = nullptr; // the {uid?, path} map itself (for in-place edits)
    RefFields fields;
};

// Depth-first collection of every ref site anywhere in `root` (recursing
// into maps/seqs; a matched ref map's own children are leaves, never
// descended into further).
[[nodiscard]] std::vector<RefSite> find_asset_refs(YamlNode& root);

// ---- classification (midday check) -----------------------------------------
enum class RefStatus : std::uint8_t {
    kClean,      // uid present, well-formed, and matches the registry's path for it
    kDrift,      // uid present and registered, but the ref's path is stale
    kMissingUid, // path-only ref, the path resolves to a real asset
    kInvalid,    // uid present but malformed text, OR well-formed but NOT registered
                 // (the "hand-minted" refusal) — OR the path does not resolve at all
};

struct RefFinding {
    std::string file;
    int line = 0;
    int col = 0;
    RefStatus status = RefStatus::kClean;
    std::string uid_text; // as authored; empty when the ref had no uid field
    std::string path;     // as authored (pre-fix)
    bool fixed = false;   // true iff a --fix pass resolved this finding
    std::string detail;   // human-readable explanation
};

// Classifies (and, when `fix` is true, REPAIRS in place) every ref site in
// `doc_root`, a document read from `file` (used only for diagnostics) whose
// own directory is `file_dir` (refs are authored relative to it — the same
// "project root = the referencing file's directory" convention
// core/loader/loader.h's load_project_events already established) and
// which is being scanned as part of a project rooted at `scan_root`
// (registry paths are root-relative; see uid_registry.h).
//
// Repair semantics (spec lines 365-366, "repairs drift in either
// direction"):
//   * kDrift: the ref's `path` is rewritten from the registry's known path
//     for its uid. The uid is NEVER touched.
//   * kMissingUid / kInvalid-but-the-path-resolves: attaches the CORRECT
//     uid for that path — reusing an existing sidecar there if one already
//     exists, otherwise minting a fresh one (mint_uid, `taken`/`rng`) and
//     writing its sidecar (write_uid_sidecar). A missing `uid:` key is
//     inserted; a bogus one is overwritten.
//   * kInvalid where the path does not resolve on disk: never fixable
//     (there is nothing to attach the uid to) — reported regardless of
//     `fix`.
// `taken`/`registry` are updated in place as new uids mint, so multiple
// fixes in the SAME scan never collide with each other.
struct ScanRefsResult {
    std::vector<RefFinding> findings;
    bool changed = false; // true iff `fix` mutated `doc_root` — caller re-emits + writes
};

// PRECONDITION shared by scan_refs and rewrite_ref_paths below: `file_dir`,
// `scan_root`, `moved_from_abs`, and `moved_to_abs` are all ABSOLUTE,
// lexically-normalized paths — the CLI verbs (cli/verbs/check.cpp, mv.cpp)
// resolve every path against the current directory exactly once, up front,
// so this file never queries the filesystem's notion of "current
// directory" itself.
ScanRefsResult scan_refs(YamlNode& doc_root,
                         const std::string& file,
                         const std::string& file_dir,
                         const std::string& scan_root,
                         UidRegistry& registry,
                         UidRng& rng,
                         bool fix);

// ---- path rewriting (midday mv) --------------------------------------------
// Rewrites every ref site in `doc_root` (a document whose own directory is
// `file_dir`) whose resolved `path` equals `moved_from_abs` to point at
// `moved_to_abs` instead (expressed relative to `file_dir`, matching how
// every OTHER ref in this file is authored). The `uid` field, if present,
// is never touched — moving an asset never changes its identity.
bool rewrite_ref_paths(YamlNode& doc_root,
                       const std::string& file_dir,
                       const std::string& moved_from_abs,
                       const std::string& moved_to_abs);

} // namespace midday::loader

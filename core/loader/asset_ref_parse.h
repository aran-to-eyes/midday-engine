// core/loader/asset_ref_parse.h — resolves a `{uid?, path}` or bare-path
// asset reference DURING LOADING (m1-scene-format): the read-only
// counterpart of core/loader/asset_ref.h's mutable, rewrite-capable
// `match_ref_shape`/`scan_refs` (check/mv's job). Shared by scene_load.cpp
// (`prefab: {uid?, path}`) and entity_load.cpp (`machines[].instance`,
// `attachments[].of`, `attachments[].entity.prefab`) so the shape parses
// identically everywhere it appears — one engine, not four copies.
#pragma once

#include "core/base/error.h"
#include "core/loader/entity_format.h"
#include "core/loader/gaps.h"
#include "core/loader/yaml.h"

#include <optional>
#include <string>
#include <string_view>

namespace midday::loader {

struct AssetRefParseResult {
    AssetRefDesc ref;
    std::optional<base::Error> error;
};

// The dual-write shape (asset_ref.h): a mapping whose keys are exactly
// {"path"} or {"uid", "path"}. `root_dir` resolves `path_authored`
// (project-root-relative) into `path_resolved`; `exists` is a plain
// filesystem check (this node never opens the referenced file's content —
// *.model.yaml has no loader yet, and a prefab entity file is opened
// separately, by the caller, only when it decides to resolve it).
AssetRefParseResult
parse_asset_ref(const YamlNode& node, std::string_view file, const std::string& root_dir);

// A bare path scalar (`entity: {prefab: <path>}` — no uid slot in this
// grammar position, examples/warden/prefabs/warden.entity.yaml).
AssetRefParseResult
parse_path_only_ref(const YamlNode& node, std::string_view file, const std::string& root_dir);

// nullopt when `ref.exists`; otherwise the Gap this reference contributes
// (lenient mode) — callers in strict mode build their own hard
// "loader.bad_ref" refusal instead of calling this.
Gap missing_asset_gap(std::string_view kind, const AssetRefDesc& ref, std::string_view file);

} // namespace midday::loader

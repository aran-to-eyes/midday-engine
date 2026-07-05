// core/loader/scene_ctx.h — INTERNAL shared state of the scene-file loader,
// split across scene_load.cpp (file/entity/machine-ref structure),
// scene_components.cpp (the M0 native + m1 generic `components:` list),
// and scene_prefab.cpp (`prefab:` + `at:` + `override:`) to hold the
// 500-line ratchet — the machine_ctx.h precedent. Not installed API;
// loader.h is the public surface.

#pragma once

#include "core/loader/loader.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace midday::loader::detail {

struct SceneCtx {
    const std::string& path;
    const reflect::Registry& registry;
    const ComponentVocab& components_vocab;
    bool lenient = false;
    SceneFile out = {};
    std::optional<base::Error> error = {};

    void fail(base::Error error_value) {
        if (!error.has_value())
            error = std::move(error_value);
    }

    [[nodiscard]] bool failed() const { return error.has_value(); }

    [[nodiscard]] std::string resolve(const std::string& ref) const {
        return (std::filesystem::path(out.root_dir) / ref).generic_string();
    }
};

// scene_components.cpp — the M0 native + m1 generic `components:` list.
void parse_components(SceneCtx& ctx, const YamlNode& node, SceneEntityDesc& entity);

// scene_prefab.cpp — `prefab: {uid?, path}`: resolves (or, in lenient
// mode, Gap-reports) the referenced entity/prefab file. Returns by value
// (never writes into a caller's `std::optional<PrefabInstanceDesc>`
// directly) so callers assign it in ONE place, after `at:`/`override:` are
// folded in too — never an unchecked access to a not-yet-engaged optional.
PrefabInstanceDesc parse_prefab(SceneCtx& ctx, const YamlNode& prefab_node);

} // namespace midday::loader::detail

// core/loader/scene_prefab.cpp — a scene entity's `prefab: {uid?, path}` +
// `at:` + `override:` (m1-scene-format, spec 4.1 "machines are prefab
// subtrees ... instanced as assets with per-entity property-diff
// overrides"). Split out of scene_load.cpp to hold the 500-line ratchet;
// shared context in scene_ctx.h.
//
// A missing prefab FILE is a lenient-only Gap (brand new grammar, no M0
// precedent to stay strict about) — the entity still parses, just without
// a resolved `entity_prefabs` entry to compute effective state from.

#include "core/loader/asset_ref_parse.h"
#include "core/loader/parse_util.h"
#include "core/loader/scene_ctx.h"

#include <utility>

namespace midday::loader::detail {

PrefabInstanceDesc parse_prefab(SceneCtx& ctx, const YamlNode& prefab_node) {
    PrefabInstanceDesc prefab;
    AssetRefParseResult ref = parse_asset_ref(prefab_node, ctx.path, ctx.out.root_dir);
    if (ref.error.has_value()) {
        ctx.fail(std::move(*ref.error));
        return prefab;
    }
    prefab.prefab_ref = ref.ref;

    if (!ref.ref.exists) {
        if (!ctx.lenient) {
            ctx.fail(err_node("loader.bad_ref",
                              ctx.path,
                              prefab_node,
                              "prefab file '" + ref.ref.path_authored +
                                  "' not found (resolved: " + ref.ref.path_resolved + ")"));
            return prefab;
        }
        ctx.out.gaps.push_back(missing_asset_gap("prefab", ref.ref, ctx.path));
        return prefab;
    }

    std::uint32_t index = 0;
    bool known = false;
    for (; index < ctx.out.entity_prefabs.size(); ++index) {
        if (ctx.out.entity_prefabs[index].path == ref.ref.path_resolved) {
            known = true;
            break;
        }
    }
    if (!known) {
        EntityLoadResult loaded = load_entity_file(
            ref.ref.path_resolved, ctx.registry, ctx.out.events, ctx.components_vocab, ctx.lenient);
        if (loaded.error.has_value()) {
            ctx.fail(std::move(*loaded.error));
            return prefab;
        }
        if (!loaded.entity.has_value()) { // defensive: loaders return one or the other
            ctx.fail(err_node("loader.bad_ref", ctx.path, prefab_node, "entity load failed"));
            return prefab;
        }
        index = static_cast<std::uint32_t>(ctx.out.entity_prefabs.size());
        append_gaps(ctx.out.gaps, loaded.entity->gaps);
        ctx.out.entity_prefabs.push_back(std::move(*loaded.entity));
    }
    prefab.resolved = true;
    prefab.entity_index = index;
    return prefab;
}

} // namespace midday::loader::detail

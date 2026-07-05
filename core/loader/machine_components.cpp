// core/loader/machine_components.cpp — m1-scene-format additions split out
// of machine_load.cpp to hold the 500-line ratchet: the generic
// `components:` list a state or a state child may carry (spec 4.1 "states
// owning component sets", core/loader/generic_components.h is the shared
// parsing engine) and `children:` parsing (now components-aware). Shared
// context in machine_ctx.h; pairs/sequences stay in machine_parts.cpp;
// file/region/state structure stays in machine_load.cpp.

#include "core/loader/generic_components.h"
#include "core/loader/machine_ctx.h"
#include "core/loader/parse_util.h"

#include <array>
#include <string_view>
#include <utility>

namespace midday::loader::detail {

namespace {

// Runs `parse_generic_components<T>` against the shared engine and folds
// its result into `ctx` (a hard error -> ctx.fail; gaps -> ctx.out.gaps,
// only ever populated when ctx.lenient) — the ONE adapter between the
// MachineCtx accumulator style and the engine's self-contained result.
template <typename ComponentEntry>
std::vector<ComponentEntry> run(MachineCtx& ctx, const YamlNode& node) {
    GenericComponentsResult<ComponentEntry> result =
        parse_generic_components<ComponentEntry>(node, ctx.path, ctx.components_vocab, ctx.lenient);
    if (result.error.has_value()) {
        ctx.fail(std::move(*result.error));
        return {};
    }
    append_gaps(ctx.out.gaps, std::move(result.gaps));
    return std::move(result.components);
}

} // namespace

std::vector<statechart::StateComponentDesc> parse_state_components(MachineCtx& ctx,
                                                                   const YamlNode& node) {
    return run<statechart::StateComponentDesc>(ctx, node);
}

std::vector<GenericComponentEntry> parse_child_components(MachineCtx& ctx, const YamlNode& node) {
    return run<GenericComponentEntry>(ctx, node);
}

void parse_children(MachineCtx& ctx, RegionCtx& region, base::Name state, const YamlNode& node) {
    if (!node.is_seq()) {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          node,
                          "expected a list of {entity[, at, components]} children"));
        return;
    }
    StateChildren children;
    children.region = region.region;
    children.state = state;
    for (const YamlNode& child : node.seq) {
        if (!child.is_map()) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, child, "expected an {entity[, at, components]} map"));
            return;
        }
        static constexpr std::array<std::string_view, 3> kAllowed = {"entity", "at", "components"};
        if (auto error = check_keys(child, ctx.path, kAllowed)) {
            ctx.fail(std::move(*error));
            return;
        }
        FieldResult entity = require_field(child, ctx.path, "entity", "a state child");
        if (entity.error.has_value()) {
            ctx.fail(std::move(*entity.error));
            return;
        }
        Parsed<std::string> name = get_name(*entity.node, ctx.path);
        if (name.error.has_value()) {
            ctx.fail(std::move(*name.error));
            return;
        }
        StateChildDesc desc;
        desc.entity = base::Name(name.value);
        for (const StateChildDesc& existing : children.children) {
            if (existing.entity == desc.entity) {
                ctx.fail(err_node("loader.duplicate",
                                  ctx.path,
                                  *entity.node,
                                  "child entity '" + name.value + "' is already declared"));
                return;
            }
        }
        if (const YamlNode* at = child.find("at")) {
            Parsed<math::Vec3> translation = get_vec3(*at, ctx.path);
            if (translation.error.has_value()) {
                ctx.fail(std::move(*translation.error));
                return;
            }
            desc.at.translation = translation.value;
        }
        if (const YamlNode* components = child.find("components")) {
            desc.components = parse_child_components(ctx, *components);
            if (ctx.failed())
                return;
        }
        children.children.push_back(desc);
    }
    ctx.out.children.push_back(std::move(children));
}

} // namespace midday::loader::detail

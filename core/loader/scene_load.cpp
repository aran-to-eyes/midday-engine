// core/loader/scene_load.cpp — `*.scene.yaml`: the file an agent runs. A
// scene lists its events files (the project vocabulary loads FIRST), then
// entities with base components from the M0 vocabulary — exactly the
// components that exist in the runtime today: Transform (hierarchy),
// Collider + RigidBody (the M0 physics surface) — and machine instances by
// path. All references are project-root-relative; the project root is the
// scene file's directory in M0 (D-BUILD-077).
//
// M0 physics mapping (D-BUILD-078): Collider{shape: box} + RigidBody{} =
// dynamic box; Collider{shape: plane} alone = the static ground plane.
// Anything else the physics server cannot honor yet refuses loudly instead
// of silently loading dead data.

#include "core/loader/loader.h"
#include "core/loader/override.h"
#include "core/loader/parse_util.h"
#include "core/loader/scene_ctx.h"
#include "core/loader/yaml.h"

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace midday::loader {

using detail::err_at;
using detail::err_node;
using detail::parse_components;
using detail::parse_prefab;
using detail::Parsed;
using detail::SceneCtx;

namespace {

void parse_machines(SceneCtx& ctx, const YamlNode& node, SceneEntityDesc& entity) {
    if (!node.is_seq()) {
        ctx.fail(err_node(
            "loader.bad_value", ctx.path, node, "expected a list of {instance: {path}} entries"));
        return;
    }
    for (const YamlNode& machine : node.seq) {
        if (!machine.is_map()) {
            ctx.fail(
                err_node("loader.bad_value", ctx.path, machine, "expected an {instance: ...} map"));
            return;
        }
        static constexpr std::array<std::string_view, 1> kAllowedOuter = {"instance"};
        if (auto error = detail::check_keys(machine, ctx.path, kAllowedOuter)) {
            ctx.fail(std::move(*error));
            return;
        }
        detail::FieldResult instance =
            detail::require_field(machine, ctx.path, "instance", "a machine entry");
        if (instance.error.has_value()) {
            ctx.fail(std::move(*instance.error));
            return;
        }
        if (!instance.node->is_map()) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, *instance.node, "expected an {path: ...} map"));
            return;
        }
        static constexpr std::array<std::string_view, 1> kAllowedInner = {"path"};
        if (auto error = detail::check_keys(*instance.node, ctx.path, kAllowedInner)) {
            ctx.fail(std::move(*error));
            return;
        }
        detail::FieldResult path_field =
            detail::require_field(*instance.node, ctx.path, "path", "a machine instance");
        if (path_field.error.has_value()) {
            ctx.fail(std::move(*path_field.error));
            return;
        }
        Parsed<std::string> ref = detail::get_name(*path_field.node, ctx.path);
        if (ref.error.has_value()) {
            ctx.fail(std::move(*ref.error));
            return;
        }
        const std::string resolved = ctx.resolve(ref.value);
        std::uint32_t index = 0;
        bool known = false;
        for (; index < ctx.out.machines.size(); ++index) {
            if (ctx.out.machines[index].path == resolved) {
                known = true;
                break;
            }
        }
        if (!known) {
            MachineLoadResult loaded = load_machine_file(resolved,
                                                         ctx.out.root_dir,
                                                         ctx.registry,
                                                         ctx.out.events,
                                                         ctx.components_vocab,
                                                         ctx.lenient);
            if (loaded.error.has_value()) {
                // The referencing location leads; the nested diagnostic
                // (with ITS file:line:col) rides along in details.
                if (loaded.error->code == "loader.io") {
                    base::Error missing = err_node("loader.bad_ref",
                                                   ctx.path,
                                                   *path_field.node,
                                                   "machine file '" + ref.value +
                                                       "' not found (resolved: " + resolved + ")");
                    ctx.fail(std::move(missing));
                } else {
                    ctx.fail(std::move(*loaded.error));
                }
                return;
            }
            if (!loaded.machine.has_value()) { // defensive: loaders return one or the other
                ctx.fail(
                    err_node("loader.bad_ref", ctx.path, *path_field.node, "machine load failed"));
                return;
            }
            index = static_cast<std::uint32_t>(ctx.out.machines.size());
            append_gaps(ctx.out.gaps, loaded.machine->gaps);
            ctx.out.machines.push_back(std::move(*loaded.machine));
        }
        entity.machines.push_back(index);
    }
}

void parse_entity(SceneCtx& ctx, const YamlNode& node) {
    if (!node.is_map()) {
        ctx.fail(err_node("loader.bad_value", ctx.path, node, "expected an entity mapping"));
        return;
    }
    static constexpr std::array<std::string_view, 6> kAllowed = {
        "entity", "components", "machines", "prefab", "at", "override"};
    if (auto error = detail::check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    detail::FieldResult name = detail::require_field(node, ctx.path, "entity", "a scene entity");
    if (name.error.has_value()) {
        ctx.fail(std::move(*name.error));
        return;
    }
    Parsed<std::string> entity_name = detail::get_name(*name.node, ctx.path);
    if (entity_name.error.has_value()) {
        ctx.fail(std::move(*entity_name.error));
        return;
    }
    SceneEntityDesc entity;
    entity.name = base::Name(entity_name.value);
    entity.line = node.line;
    for (const SceneEntityDesc& existing : ctx.out.entities) {
        if (existing.name == entity.name) {
            ctx.fail(err_node("loader.duplicate",
                              ctx.path,
                              *name.node,
                              "entity '" + entity_name.value + "' is already declared"));
            return;
        }
    }

    const bool has_prefab = node.find("prefab") != nullptr;
    const bool has_inline = node.find("components") != nullptr || node.find("machines") != nullptr;
    if (has_prefab && has_inline) {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          node,
                          "entity '" + entity_name.value +
                              "': 'prefab:' cannot combine with 'components:'/'machines:' (an "
                              "entity is either inline or a prefab instance)"));
        return;
    }
    if (!has_prefab && (node.find("at") != nullptr || node.find("override") != nullptr)) {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          node,
                          "entity '" + entity_name.value +
                              "': 'at:'/'override:' only apply to a 'prefab:' entity"));
        return;
    }

    if (has_prefab) {
        entity.kind = SceneEntityKind::kPrefab;
        PrefabInstanceDesc prefab = parse_prefab(ctx, *node.find("prefab"));
        if (ctx.failed())
            return;
        if (const YamlNode* at = node.find("at")) {
            Parsed<math::Vec3> translation = detail::get_vec3(*at, ctx.path);
            if (translation.error.has_value()) {
                ctx.fail(std::move(*translation.error));
                return;
            }
            prefab.at.translation = translation.value;
        }
        if (const YamlNode* override_node = node.find("override")) {
            OverrideParseResult parsed = parse_override_block(*override_node, ctx.path);
            if (parsed.error.has_value()) {
                ctx.fail(std::move(*parsed.error));
                return;
            }
            prefab.overrides = std::move(parsed.entries);
        }
        entity.prefab = std::move(prefab);
        ctx.out.entities.push_back(std::move(entity));
        return;
    }

    if (const YamlNode* components = node.find("components"))
        parse_components(ctx, *components, entity);
    if (ctx.failed())
        return;
    if (const YamlNode* machines = node.find("machines"))
        parse_machines(ctx, *machines, entity);
    if (ctx.failed())
        return;

    // M0 physics semantics — refuse silently-dead data (header comment).
    const ComponentSet& set = entity.components;
    if (set.collider.has_value() && set.collider->box && !set.rigid_body) {
        ctx.fail(err_at("loader.unsupported",
                        ctx.path,
                        entity.line,
                        node.col,
                        "entity '" + entity_name.value +
                            "': a box Collider needs a RigidBody in M0 (static/kinematic boxes "
                            "arrive with m4-physics-full)"));
        return;
    }
    if (set.rigid_body && (!set.collider.has_value() || !set.collider->box)) {
        ctx.fail(
            err_at("loader.bad_value",
                   ctx.path,
                   entity.line,
                   node.col,
                   "entity '" + entity_name.value + "': RigidBody requires a box Collider in M0"));
        return;
    }
    ctx.out.entities.push_back(std::move(entity));
}

} // namespace

SceneLoadResult load_scene(const std::string& scene_path,
                           const reflect::Registry& registry,
                           bool lenient,
                           const ComponentVocab& components_vocab) {
    SceneLoadResult result;
    YamlParseResult parsed = parse_yaml_file(scene_path);
    if (parsed.error.has_value()) {
        result.error = std::move(parsed.error);
        return result;
    }
    const YamlNode& root = parsed.root;
    if (auto error = detail::check_format(root, scene_path, "scene")) {
        result.error = std::move(error);
        return result;
    }
    static constexpr std::array<std::string_view, 4> kAllowed = {
        "format", "scene", "events", "entities"};
    if (auto error = detail::check_keys(root, scene_path, kAllowed)) {
        result.error = std::move(error);
        return result;
    }

    SceneCtx ctx{.path = scene_path,
                 .registry = registry,
                 .components_vocab = components_vocab,
                 .lenient = lenient};
    ctx.out.path = scene_path;
    const std::filesystem::path parent = std::filesystem::path(scene_path).parent_path();
    ctx.out.root_dir = parent.empty() ? std::string(".") : parent.generic_string();

    detail::FieldResult name = detail::require_field(root, scene_path, "scene", "a scene file");
    if (name.error.has_value()) {
        result.error = std::move(name.error);
        return result;
    }
    Parsed<std::string> scene_name = detail::get_name(*name.node, scene_path);
    if (scene_name.error.has_value()) {
        result.error = std::move(scene_name.error);
        return result;
    }
    ctx.out.name = base::Name(scene_name.value);

    if (const YamlNode* events = root.find("events")) {
        if (!events->is_seq()) {
            result.error = err_node(
                "loader.bad_value", scene_path, *events, "expected a list of events file paths");
            return result;
        }
        for (const YamlNode& ref_node : events->seq) {
            Parsed<std::string> ref = detail::get_name(ref_node, scene_path);
            if (ref.error.has_value()) {
                result.error = std::move(ref.error);
                return result;
            }
            const std::string resolved = ctx.resolve(ref.value);
            if (auto error = load_events_file(resolved, registry, ctx.out.events)) {
                if (error->code == "loader.io")
                    error = err_node("loader.bad_ref",
                                     scene_path,
                                     ref_node,
                                     "events file '" + ref.value +
                                         "' not found (resolved: " + resolved + ")");
                result.error = std::move(error);
                return result;
            }
        }
    }

    if (const YamlNode* entities = root.find("entities")) {
        if (!entities->is_seq()) {
            result.error =
                err_node("loader.bad_value", scene_path, *entities, "expected a list of entities");
            return result;
        }
        for (const YamlNode& entity : entities->seq) {
            parse_entity(ctx, entity);
            if (ctx.failed()) {
                result.error = std::move(ctx.error);
                return result;
            }
        }
    }

    result.scene = std::move(ctx.out);
    return result;
}

} // namespace midday::loader

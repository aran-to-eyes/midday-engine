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
#include "core/loader/parse_util.h"
#include "core/loader/yaml.h"

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace midday::loader {

using detail::err_at;
using detail::err_node;
using detail::Parsed;

namespace {

struct SceneCtx {
    const std::string& path;
    const reflect::Registry& registry;
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

void parse_transform(SceneCtx& ctx, const YamlNode& node, ComponentSet& components) {
    static constexpr std::array<std::string_view, 1> kAllowed = {"at"};
    if (auto error = detail::check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    math::Transform transform;
    if (const YamlNode* at = node.find("at")) {
        Parsed<math::Vec3> translation = detail::get_vec3(*at, ctx.path);
        if (translation.error.has_value()) {
            ctx.fail(std::move(*translation.error));
            return;
        }
        transform.translation = translation.value;
    }
    components.transform = transform;
}

void parse_collider(SceneCtx& ctx, const YamlNode& node, ComponentSet& components) {
    static constexpr std::array<std::string_view, 2> kAllowed = {"shape", "size"};
    if (auto error = detail::check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    detail::FieldResult shape = detail::require_field(node, ctx.path, "shape", "a Collider");
    if (shape.error.has_value()) {
        ctx.fail(std::move(*shape.error));
        return;
    }
    Parsed<std::string> kind = detail::get_name(*shape.node, ctx.path);
    if (kind.error.has_value()) {
        ctx.fail(std::move(*kind.error));
        return;
    }
    ColliderDesc collider;
    if (kind.value == "box") {
        collider.box = true;
        detail::FieldResult size = detail::require_field(node, ctx.path, "size", "a box Collider");
        if (size.error.has_value()) {
            ctx.fail(std::move(*size.error));
            return;
        }
        Parsed<math::Vec3> extents = detail::get_vec3(*size.node, ctx.path);
        if (extents.error.has_value()) {
            ctx.fail(std::move(*extents.error));
            return;
        }
        if (!(extents.value.x > 0.0F) || !(extents.value.y > 0.0F) || !(extents.value.z > 0.0F)) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, *size.node, "box size must be positive on all axes"));
            return;
        }
        collider.size = extents.value;
    } else if (kind.value == "plane") {
        if (node.find("size") != nullptr) {
            ctx.fail(err_node("loader.bad_value",
                              ctx.path,
                              node,
                              "a plane collider takes no size (an infinite ground plane)"));
            return;
        }
    } else {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          *shape.node,
                          "unknown collider shape '" + kind.value +
                              "' (M0 physics surface: box, plane)"));
        return;
    }
    components.collider = collider;
}

void parse_components(SceneCtx& ctx, const YamlNode& node, SceneEntityDesc& entity) {
    if (!node.is_seq()) {
        ctx.fail(err_node("loader.bad_value", ctx.path, node, "expected a list of components"));
        return;
    }
    for (const YamlNode& component : node.seq) {
        if (!component.is_map() || component.map.size() != 1) {
            ctx.fail(err_node("loader.bad_value",
                              ctx.path,
                              component,
                              "expected one {ComponentName: {...}} entry"));
            return;
        }
        const YamlEntry& entry = component.map.front();
        const YamlNode& body = entry.node();
        if (!body.is_map() && !body.is_null()) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, body, "expected a component property mapping"));
            return;
        }
        static const YamlNode kEmpty = [] {
            YamlNode node;
            node.kind = YamlNode::Kind::kMap;
            return node;
        }();
        const YamlNode& props = body.is_null() ? kEmpty : body;
        const bool duplicate = (entry.key == "Transform" && entity.components.transform) ||
                               (entry.key == "Collider" && entity.components.collider) ||
                               (entry.key == "RigidBody" && entity.components.rigid_body);
        if (duplicate) {
            ctx.fail(err_at("loader.duplicate",
                            ctx.path,
                            entry.key_line,
                            entry.key_col,
                            "component '" + entry.key + "' is already declared"));
            return;
        }
        if (entry.key == "Transform") {
            parse_transform(ctx, props, entity.components);
        } else if (entry.key == "Collider") {
            parse_collider(ctx, props, entity.components);
        } else if (entry.key == "RigidBody") {
            if (auto error = detail::check_keys(props, ctx.path, {})) {
                ctx.fail(std::move(*error));
                return;
            }
            entity.components.rigid_body = true;
        } else {
            ctx.fail(err_at("loader.unknown_key",
                            ctx.path,
                            entry.key_line,
                            entry.key_col,
                            "unknown component '" + entry.key +
                                "' (M0 vocabulary: Transform, Collider, RigidBody)"));
            return;
        }
        if (ctx.failed())
            return;
    }
}

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
            MachineLoadResult loaded =
                load_machine_file(resolved, ctx.out.root_dir, ctx.registry, ctx.out.events);
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
    static constexpr std::array<std::string_view, 3> kAllowed = {
        "entity", "components", "machines"};
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

SceneLoadResult load_scene(const std::string& scene_path, const reflect::Registry& registry) {
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

    SceneCtx ctx{.path = scene_path, .registry = registry};
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

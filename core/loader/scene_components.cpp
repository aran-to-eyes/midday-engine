// core/loader/scene_components.cpp — a scene entity's `components:` list:
// the M0 native three (Transform, Collider, RigidBody — typed, unchanged
// since m0-yaml-loader-run) plus, m1-scene-format, every OTHER name
// generic (MeshRenderer, Spline, VirtualCamera, ... — checked against a
// supplied component vocabulary; unknown is a hard refusal by default,
// every existing caller's exact M0 behavior, or a Gap when the caller
// opted into lenient mode, core/loader/gaps.h). Split out of scene_load.cpp
// to hold the 500-line ratchet; shared context in scene_ctx.h.

#include "core/loader/parse_util.h"
#include "core/loader/scene_ctx.h"

#include <array>
#include <utility>

namespace midday::loader::detail {

namespace {

void parse_transform(SceneCtx& ctx, const YamlNode& node, ComponentSet& components) {
    static constexpr std::array<std::string_view, 1> kAllowed = {"at"};
    if (auto error = check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    math::Transform transform;
    if (const YamlNode* at = node.find("at")) {
        Parsed<math::Vec3> translation = get_vec3(*at, ctx.path);
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
    if (auto error = check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    FieldResult shape = require_field(node, ctx.path, "shape", "a Collider");
    if (shape.error.has_value()) {
        ctx.fail(std::move(*shape.error));
        return;
    }
    Parsed<std::string> kind = get_name(*shape.node, ctx.path);
    if (kind.error.has_value()) {
        ctx.fail(std::move(*kind.error));
        return;
    }
    ColliderDesc collider;
    if (kind.value == "box") {
        collider.box = true;
        FieldResult size = require_field(node, ctx.path, "size", "a box Collider");
        if (size.error.has_value()) {
            ctx.fail(std::move(*size.error));
            return;
        }
        Parsed<math::Vec3> extents = get_vec3(*size.node, ctx.path);
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

} // namespace

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
            YamlNode empty_node;
            empty_node.kind = YamlNode::Kind::kMap;
            return empty_node;
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
            if (auto error = check_keys(props, ctx.path, {})) {
                ctx.fail(std::move(*error));
                return;
            }
            entity.components.rigid_body = true;
        } else {
            // m1-scene-format: every non-native name (MeshRenderer,
            // Spline, VirtualCamera, ...) is generic — checked against the
            // supplied vocabulary, never the fixed M0 three.
            for (const GenericComponentEntry& existing : entity.extra_components) {
                if (existing.type.view() == entry.key) {
                    ctx.fail(err_at("loader.duplicate",
                                    ctx.path,
                                    entry.key_line,
                                    entry.key_col,
                                    "component '" + entry.key + "' is already declared"));
                    return;
                }
            }
            Parsed<base::Json> fields = yaml_to_json(props, ctx.path);
            if (fields.error.has_value()) {
                ctx.fail(std::move(*fields.error));
                return;
            }
            if (!ctx.components_vocab.known(entry.key)) {
                if (!ctx.lenient) {
                    ctx.fail(err_at("loader.unknown_key",
                                    ctx.path,
                                    entry.key_line,
                                    entry.key_col,
                                    "unknown component '" + entry.key + "'"));
                    return;
                }
                ctx.out.gaps.push_back(
                    Gap{.kind = "component",
                        .what = entry.key,
                        .file = ctx.path,
                        .line = entry.key_line,
                        .col = entry.key_col,
                        .detail = "component type '" + entry.key + "' is not implemented yet"});
            }
            entity.extra_components.push_back(
                GenericComponentEntry{.type = base::Name(entry.key), .fields = fields.value});
        }
        if (ctx.failed())
            return;
    }
}

} // namespace midday::loader::detail

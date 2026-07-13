// ts/runtime/typed_envelope.cpp — see typed_envelope.h for the node shapes.

#include "ts/runtime/typed_envelope.h"

#include "core/ecs/entity.h"

#include <string>
#include <utility>

namespace midday::script {

namespace {

using base::Json;

base::Error hydrate_error(std::string_view field_path, const reflect::TypeDesc& type) {
    base::Error error;
    error.code = "script.hydrate";
    error.message = "payload value does not inhabit its schema type's wire shape";
    error.details.set("field", field_path);
    error.details.set("expected", type.canonical());
    return error;
}

Json json_node(const Json& value) {
    Json node = Json::object();
    node.set("t", "json");
    node.set("v", value);
    return node;
}

// [x, y, ...] numeric tuple -> {t:<tag>, v:[...]} (the prelude spreads it
// into the script-facing {x, y, z[, w]} / {r, g, b, a} object).
EncodedValue tuple_node(std::string_view tag,
                        std::size_t arity,
                        const reflect::TypeDesc& type,
                        const Json& value,
                        std::string_view field_path) {
    EncodedValue out;
    if (!value.is_array() || value.elements().size() != arity) {
        out.error = hydrate_error(field_path, type);
        return out;
    }
    Json list = Json::array();
    for (const Json& item : value.elements()) {
        if (!item.is_number()) {
            out.error = hydrate_error(field_path, type);
            return out;
        }
        list.push(item);
    }
    Json node = Json::object();
    node.set("t", tag);
    node.set("v", std::move(list));
    out.node = std::move(node);
    return out;
}

} // namespace

EncodedValue encode_typed_value(const reflect::TypeDesc& type,
                                const base::Json& value,
                                std::string_view field_path) {
    EncodedValue out;
    switch (type.kind()) {
    case reflect::TypeKind::kBool:
    case reflect::TypeKind::kInt:
    case reflect::TypeKind::kFloat:
    case reflect::TypeKind::kString:
    case reflect::TypeKind::kName:
    case reflect::TypeKind::kAssetRef:
        out.node = json_node(value);
        return out;
    case reflect::TypeKind::kEntityRef: {
        if (!value.is_int() || value.as_int() < 0) {
            out.error = hydrate_error(field_path, type);
            return out;
        }
        const ecs::EntityRef ref =
            ecs::EntityRef::from_bits(static_cast<std::uint64_t>(value.as_int()));
        Json node = Json::object();
        node.set("t", "ref");
        node.set("i", static_cast<std::int64_t>(ref.index));
        node.set("g", static_cast<std::int64_t>(ref.generation));
        out.node = std::move(node);
        return out;
    }
    case reflect::TypeKind::kVec2:
        return tuple_node("vec2", 2, type, value, field_path);
    case reflect::TypeKind::kVec3:
        return tuple_node("vec3", 3, type, value, field_path);
    case reflect::TypeKind::kVec4:
        return tuple_node("vec4", 4, type, value, field_path);
    case reflect::TypeKind::kQuat:
        return tuple_node("quat", 4, type, value, field_path);
    case reflect::TypeKind::kColor:
        return tuple_node("color", 4, type, value, field_path);
    case reflect::TypeKind::kArray: {
        if (!value.is_array()) {
            out.error = hydrate_error(field_path, type);
            return out;
        }
        Json list = Json::array();
        std::size_t index = 0;
        for (const Json& item : value.elements()) {
            EncodedValue element = encode_typed_value(
                type.element(), item, std::string(field_path) + "[" + std::to_string(index) + "]");
            if (element.error.has_value())
                return element;
            list.push(std::move(element.node));
            ++index;
        }
        Json node = Json::object();
        node.set("t", "arr");
        node.set("v", std::move(list));
        out.node = std::move(node);
        return out;
    }
    case reflect::TypeKind::kMap: {
        if (!value.is_object()) {
            out.error = hydrate_error(field_path, type);
            return out;
        }
        Json entries = Json::object();
        for (const auto& [key, item] : value.items()) {
            EncodedValue element =
                encode_typed_value(type.element(), item, std::string(field_path) + "." + key);
            if (element.error.has_value())
                return element;
            entries.set(key, std::move(element.node));
        }
        Json node = Json::object();
        node.set("t", "map");
        node.set("v", std::move(entries));
        out.node = std::move(node);
        return out;
    }
    }
    out.error = hydrate_error(field_path, type); // unreachable
    return out;
}

EncodedValue encode_event_payload(const reflect::EventDesc* schema, const base::Json& payload) {
    EncodedValue out;
    if (schema == nullptr) { // unregistered vocabulary: verbatim, no hydration
        out.node = json_node(payload);
        return out;
    }
    // Schema declaration order — never JSON insertion order (D5 discipline).
    Json entries = Json::object();
    for (const reflect::EventFieldDesc& field : schema->payload) {
        const Json* value = payload.find(field.name.view());
        if (value == nullptr) { // the bus validated; refuse rather than guess
            out.error = hydrate_error(field.name.view(), field.type);
            return out;
        }
        EncodedValue element = encode_typed_value(field.type, *value, field.name.view());
        if (element.error.has_value())
            return element;
        entries.set(field.name.view(), std::move(element.node));
    }
    Json node = Json::object();
    node.set("t", "map");
    node.set("v", std::move(entries));
    out.node = std::move(node);
    return out;
}

} // namespace midday::script

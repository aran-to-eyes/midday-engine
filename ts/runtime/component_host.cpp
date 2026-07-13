// ts/runtime/component_host.cpp — see component_host.h for the seam
// contract and the design notes behind it.

#include "ts/runtime/component_host.h"

#include "core/base/name.h"
#include "core/reflect/registry.h"
#include "core/reflect/type_model.h"
#include "ts/runtime/host_json.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace midday::script {

using base::Error;
using base::Json;

namespace {

// ---- the authoring->wire payload adapter (component_host.h set_registry) --

// An EntityRef INSTANCE crosses the JS boundary as exactly {index,
// generation} (its two own properties). Strict: anything else passes
// through untouched (and the bus refuses it, as before).
bool object_ref_bits(const Json& value, std::uint64_t& bits) {
    if (!value.is_object() || value.items().size() != 2)
        return false;
    const Json* index = value.find("index");
    const Json* generation = value.find("generation");
    if (index == nullptr || generation == nullptr || !index->is_int() || !generation->is_int())
        return false;
    const std::int64_t idx = index->as_int();
    const std::int64_t gen = generation->as_int();
    if (idx < 0 || idx > 0xFFFFFFFFLL || gen < 0 || gen > 0xFFFFFFFFLL)
        return false;
    bits =
        ecs::EntityRef{static_cast<std::uint32_t>(idx), static_cast<std::uint32_t>(gen)}.to_bits();
    return true;
}

// {x, y[, z[, w]]} with EXACTLY the arity's keys, all numbers.
bool exact_numeric_object(const Json& value, std::size_t arity) {
    static constexpr const char* kParts[] = {"x", "y", "z", "w"};
    if (!value.is_object() || value.items().size() != arity)
        return false;
    for (std::size_t i = 0; i < arity; ++i) {
        const Json* part = value.find(kParts[i]);
        if (part == nullptr || !part->is_number())
            return false;
    }
    return true;
}

Json wire_value(const reflect::TypeDesc& type, const Json& value) {
    using reflect::TypeKind;
    static constexpr const char* kParts[] = {"x", "y", "z", "w"};
    switch (type.kind()) {
    case TypeKind::kEntityRef: {
        std::uint64_t bits = 0;
        if (object_ref_bits(value, bits))
            return {static_cast<std::int64_t>(bits)};
        return value;
    }
    case TypeKind::kVec2:
    case TypeKind::kVec3:
    case TypeKind::kVec4:
    case TypeKind::kQuat: {
        const std::size_t arity = type.kind() == TypeKind::kVec2   ? 2
                                  : type.kind() == TypeKind::kVec3 ? 3
                                                                   : 4;
        if (!exact_numeric_object(value, arity))
            return value;
        Json tuple = Json::array();
        for (std::size_t i = 0; i < arity; ++i)
            tuple.push(*value.find(kParts[i]));
        return tuple;
    }
    case TypeKind::kArray: {
        if (!value.is_array())
            return value;
        Json out = Json::array();
        for (const Json& item : value.elements())
            out.push(wire_value(type.element(), item));
        return out;
    }
    case TypeKind::kMap: {
        if (!value.is_object())
            return value;
        Json out = Json::object();
        for (const auto& [key, item] : value.items())
            out.set(key, wire_value(type.element(), item));
        return out;
    }
    default:
        return value; // scalars already speak wire (color object form: later node)
    }
}

// Convert exactly the DECLARED fields; undeclared keys copy verbatim (the
// bus's unknown_field refusal stays intact and exact).
Json wire_payload(const reflect::EventDesc& desc, const Json& payload) {
    Json out = Json::object();
    for (const auto& [key, value] : payload.items()) {
        const reflect::EventFieldDesc* field = nullptr;
        for (const reflect::EventFieldDesc& candidate : desc.payload)
            if (key == candidate.name.view())
                field = &candidate;
        out.set(key, field != nullptr ? wire_value(field->type, value) : value);
    }
    return out;
}

} // namespace

ComponentHost::ComponentHost(ScriptRuntime& runtime,
                             ecs::World& world,
                             bus::Bus& bus,
                             hierarchy::Hierarchy* hierarchy)
    : world_(&world), bus_(&bus), hierarchy_(hierarchy) {
    runtime.register_host_fn(std::string(kStatusFn),
                             [this](const Json::Array& args) { return status(args); });
    runtime.register_host_fn(std::string(kRootFn),
                             [this](const Json::Array& args) { return root(args); });
    runtime.register_host_fn(std::string(kTriggerEntityFn),
                             [this](const Json::Array& args) { return trigger_entity(args); });
    runtime.register_host_fn(std::string(kTriggerNamedFn),
                             [this](const Json::Array& args) { return trigger_named(args); });
}

void ComponentHost::note_despawn(ecs::EntityRef ref, std::uint64_t tick) {
    despawn_ticks_[ref.index] = {ref.generation, tick};
}

HostResult ComponentHost::status(const Json::Array& args) const {
    HostResult result;
    if (args.size() != 2 || !args[0].is_number() || !args[1].is_number()) {
        result.error = host_bad_args(kStatusFn, "index: number, generation: number");
        return result;
    }
    const ecs::EntityRef ref = host_entity_arg(args, 0, 1);
    Json out = Json::object();
    const bool alive = world_->alive(ref);
    out.set("alive", alive);
    Json despawn_tick;
    if (!alive) {
        const auto found = despawn_ticks_.find(ref.index);
        if (found != despawn_ticks_.end() && found->second.first == ref.generation)
            despawn_tick = Json(static_cast<std::int64_t>(found->second.second));
    }
    out.set("despawn_tick", std::move(despawn_tick)); // null unless recorded above
    result.value = std::move(out);
    return result;
}

HostResult ComponentHost::root(const Json::Array& args) const {
    HostResult result;
    if (args.size() != 2 || !args[0].is_number() || !args[1].is_number()) {
        result.error = host_bad_args(kRootFn, "index: number, generation: number");
        return result;
    }
    const ecs::EntityRef ref = host_entity_arg(args, 0, 1);
    ecs::EntityRef owner = hierarchy_ != nullptr ? hierarchy_->owner_of(ref) : ecs::EntityRef{};
    result.value = host_ref_json(owner.is_null() ? ref : owner);
    return result;
}

HostResult
ComponentHost::fire(bus::EventKey key, std::string_view event, const Json& payload) const {
    HostResult result;
    // Inside a component hook/onEvent frame the dispatch record is the
    // cause (push_cause/pop_cause, component_host.h); outside one, 0 —
    // byte-identically the pre-0B behavior.
    const std::uint64_t cause_id = cause_stack_.empty() ? 0 : cause_stack_.back();
    // Authoring->wire adaptation for vocabulary events (set_registry).
    const base::Name name(event);
    Json wire;
    const Json* delivered = &payload;
    if (registry_ != nullptr && payload.is_object()) {
        if (const auto* entry = registry_->find_event(name)) {
            wire = wire_payload(entry->desc, payload);
            delivered = &wire;
        }
    }
    bus::TriggerResult triggered = bus_->trigger(key, name, *delivered, cause_id);
    if (triggered.error.has_value())
        result.error = std::move(triggered.error);
    else
        result.value = Json(static_cast<std::int64_t>(triggered.record_id));
    return result;
}

HostResult ComponentHost::trigger_entity(const Json::Array& args) {
    if (args.size() != 4 || !args[0].is_string() || !args[2].is_number() || !args[3].is_number()) {
        HostResult result;
        result.error = host_bad_args(kTriggerEntityFn,
                                     "event: string, payload, index: number, generation: number");
        return result;
    }
    return fire(bus::EventKey::entity(host_entity_arg(args, 2, 3)), args[0].as_string(), args[1]);
}

HostResult ComponentHost::trigger_named(const Json::Array& args) {
    if (args.size() != 3 || !args[0].is_string() || !args[2].is_string()) {
        HostResult result;
        result.error = host_bad_args(kTriggerNamedFn, "event: string, payload, name: string");
        return result;
    }
    return fire(
        bus::EventKey::named(base::Name(args[2].as_string())), args[0].as_string(), args[1]);
}

} // namespace midday::script

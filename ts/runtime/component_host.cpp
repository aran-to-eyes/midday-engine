// ts/runtime/component_host.cpp — see component_host.h for the seam
// contract and the design notes behind it.

#include "ts/runtime/component_host.h"

#include "core/base/name.h"

#include <cstdint>
#include <utility>

namespace midday::script {

using base::Error;
using base::Json;

namespace {

std::uint32_t as_u32(const Json& value) {
    return static_cast<std::uint32_t>(value.as_double());
}

ecs::EntityRef entity_arg(const Json::Array& args, std::size_t index_pos, std::size_t gen_pos) {
    return ecs::EntityRef{as_u32(args[index_pos]), as_u32(args[gen_pos])};
}

Json ref_json(ecs::EntityRef ref) {
    Json out = Json::object();
    out.set("index", static_cast<std::int64_t>(ref.index));
    out.set("generation", static_cast<std::int64_t>(ref.generation));
    return out;
}

Error bad_args(std::string_view fn, std::string_view shape) {
    return Error{.code = "script.host",
                 .message = std::string(fn) + " expects (" + std::string(shape) + ")"};
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
        result.error = bad_args(kStatusFn, "index: number, generation: number");
        return result;
    }
    const ecs::EntityRef ref = entity_arg(args, 0, 1);
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
        result.error = bad_args(kRootFn, "index: number, generation: number");
        return result;
    }
    const ecs::EntityRef ref = entity_arg(args, 0, 1);
    ecs::EntityRef owner = hierarchy_ != nullptr ? hierarchy_->owner_of(ref) : ecs::EntityRef{};
    result.value = ref_json(owner.is_null() ? ref : owner);
    return result;
}

HostResult
ComponentHost::fire(bus::EventKey key, std::string_view event, const Json& payload) const {
    HostResult result;
    bus::TriggerResult triggered = bus_->trigger(key, base::Name(event), payload, /*cause_id=*/0);
    if (triggered.error.has_value())
        result.error = std::move(triggered.error);
    else
        result.value = Json(static_cast<std::int64_t>(triggered.record_id));
    return result;
}

HostResult ComponentHost::trigger_entity(const Json::Array& args) {
    if (args.size() != 4 || !args[0].is_string() || !args[2].is_number() || !args[3].is_number()) {
        HostResult result;
        result.error =
            bad_args(kTriggerEntityFn, "event: string, payload, index: number, generation: number");
        return result;
    }
    return fire(bus::EventKey::entity(entity_arg(args, 2, 3)), args[0].as_string(), args[1]);
}

HostResult ComponentHost::trigger_named(const Json::Array& args) {
    if (args.size() != 3 || !args[0].is_string() || !args[2].is_string()) {
        HostResult result;
        result.error = bad_args(kTriggerNamedFn, "event: string, payload, name: string");
        return result;
    }
    return fire(
        bus::EventKey::named(base::Name(args[2].as_string())), args[0].as_string(), args[1]);
}

} // namespace midday::script

// ts/runtime/component_instance_seats.cpp — the seat lifecycle half of
// ComponentInstanceHost (component_instance_host.h): materialization
// (manifest -> module -> instance -> typed field hydration -> attach),
// the chart-driven lifecycle hooks, and onEvent dispatch with typed
// payload hydration. Host plumbing lives in component_instance_host.cpp.

#include "core/journal/writer.h"
#include "ts/runtime/component_instance_host.h"
#include "ts/runtime/typed_envelope.h"

#include <algorithm>
#include <string>
#include <utility>

namespace midday::script {

using base::Error;
using base::Json;

namespace {

Error component_error(std::string_view code, std::string_view message, std::string_view component) {
    Error error;
    error.code = std::string(code);
    error.message = std::string(message);
    error.details.set("component", component);
    return error;
}

std::string entity_form(ecs::EntityRef ref) {
    return "entity:" + std::to_string(ref.index) + "#" + std::to_string(ref.generation);
}

} // namespace

std::optional<Error> ComponentInstanceHost::materialize_base(
    ecs::EntityRef entity, const loader::GenericComponentEntry& entry, std::uint64_t cause_id) {
    return materialize_seat(entry.type.view(),
                            entry.fields,
                            entity,
                            cause_id,
                            /*state_scoped=*/false,
                            statechart::kInvalidMachine,
                            {},
                            {});
}

std::optional<Error>
ComponentInstanceHost::materialize_state(statechart::Statechart& chart,
                                         statechart::MachineId machine,
                                         base::Name region,
                                         base::Name state,
                                         ecs::EntityRef host,
                                         const statechart::StateComponentDesc& desc,
                                         std::uint64_t cause_id) {
    if (auto error = materialize_seat(desc.type.view(),
                                      desc.fields,
                                      host,
                                      cause_id,
                                      /*state_scoped=*/true,
                                      machine,
                                      region,
                                      state))
        return error;
    // The enter-2/exit-3 registration: attach order = this call order (the
    // loader dispatcher walks document order) = the chart's replay order.
    return chart.add_component_hooks(machine, region, state, desc.type, *this);
}

std::optional<Error> ComponentInstanceHost::mirror_native_transform(ecs::EntityRef entity,
                                                                    const math::Transform& value,
                                                                    std::uint64_t cause_id) {
    if (auto error = ensure_support())
        return error;
    Seat mirror; // journal shape only — a Transform mirror owns no seat
    mirror.component = "Transform";
    mirror.entity = entity;
    if (auto error = journal_attach(mirror, "mirror", cause_id))
        return error;
    auto vec3_node = [](const math::Vec3& v) {
        Json list = Json::array();
        list.push(static_cast<double>(v.x));
        list.push(static_cast<double>(v.y));
        list.push(static_cast<double>(v.z));
        Json node = Json::object();
        node.set("t", "vec3");
        node.set("v", std::move(list));
        return node;
    };
    Json quat = Json::array();
    quat.push(static_cast<double>(value.rotation.x));
    quat.push(static_cast<double>(value.rotation.y));
    quat.push(static_cast<double>(value.rotation.z));
    quat.push(static_cast<double>(value.rotation.w));
    Json quat_node = Json::object();
    quat_node.set("t", "quat");
    quat_node.set("v", std::move(quat));
    Json call = Json::object();
    call.set("index", static_cast<std::int64_t>(entity.index));
    call.set("generation", static_cast<std::int64_t>(entity.generation));
    call.set("position", vec3_node(value.translation));
    call.set("rotation", std::move(quat_node));
    call.set("scale", vec3_node(value.scale));
    EvalResult attached = runtime_->call_json(kAttachTransformFn, call);
    if (attached.error.has_value())
        return std::move(attached.error);
    return std::nullopt;
}

std::optional<Error> ComponentInstanceHost::materialize_seat(std::string_view component,
                                                             const Json& fields,
                                                             ecs::EntityRef entity,
                                                             std::uint64_t cause_id,
                                                             bool state_scoped,
                                                             statechart::MachineId machine,
                                                             base::Name region,
                                                             base::Name state) {
    const ManifestComponent* manifest = find_manifest(component);
    if (manifest == nullptr)
        return component_error("component.not_in_manifest",
                               "this component name is not in any loaded component manifest "
                               "(midday script extract writes one)",
                               component);
    Toolchain::LoadOutcome loaded = toolchain_->load_module(*runtime_, manifest->file);
    if (loaded.error.has_value()) {
        loaded.error->details.set("component", component);
        return std::move(loaded.error);
    }
    if (auto error = ensure_support())
        return error;

    // Typed FIELD hydration: every authored field must be a manifest field
    // and inhabit its declared type; unauthored fields keep class defaults.
    Json encoded_fields = Json::array();
    if (fields.is_object()) {
        for (const auto& [key, value] : fields.items()) {
            const ManifestField* declared = nullptr;
            for (const ManifestField& field : manifest->fields)
                if (field.name == key)
                    declared = &field;
            if (declared == nullptr) {
                Error error = component_error(
                    "component.unknown_field", "authored field is not in the schema", component);
                error.details.set("field", key);
                return error;
            }
            if (declared->type.kind() == reflect::TypeKind::kEntityRef) {
                Error error = component_error("component.field_unsupported",
                                              "entity_ref fields cannot be authored in 0B "
                                              "(symbolic resolution is a later node)",
                                              component);
                error.details.set("field", key);
                return error;
            }
            if (!declared->type.accepts(value)) {
                Error error = component_error("component.field_type",
                                              "authored value does not inhabit the field's type",
                                              component);
                error.details.set("field", key);
                error.details.set("expected", declared->type.canonical());
                return error;
            }
            EncodedValue encoded = encode_typed_value(declared->type, value, key);
            if (encoded.error.has_value())
                return encoded.error;
            Json field_entry = Json::object();
            field_entry.set("name", key);
            field_entry.set("value", std::move(encoded.node));
            encoded_fields.push(std::move(field_entry));
        }
    }

    // The registration shim: import the module BY ITS CANONICAL NAME and
    // seat the named export (falling back to default). Generated per seat.
    const std::size_t seat_id = seats_.size();
    const std::string name_literal = Json(std::string(component)).dump();
    const std::string shim = "import * as M from " + Json(loaded.resolved).dump() + ";\n" +
                             std::string(kRegisterFn) + "(" + std::to_string(seat_id) + ", M[" +
                             name_literal + "] ?? M.default, " + name_literal + ");\n";
    const std::string shim_name = "<midday:component-seat:" + std::to_string(seat_id) + ">";
    ScriptRuntime::LoadedModule bound = runtime_->load_module_source(shim_name, shim);
    if (bound.error.has_value()) {
        bound.error->details.set("component", std::string(component));
        return std::move(bound.error);
    }
    EvalResult flags = runtime_->call_json(kIntrospectFn, Json(static_cast<std::int64_t>(seat_id)));
    if (flags.error.has_value())
        return std::move(flags.error);

    Seat seat;
    seat.component = std::string(component);
    seat.entity = entity;
    seat.state_scoped = state_scoped;
    seat.machine = machine;
    seat.region = region;
    seat.state = state;
    seat.events = manifest->events;
    const Json* has_enter = flags.value.find("onEnter");
    const Json* has_exit = flags.value.find("onExit");
    const Json* has_event = flags.value.find("onEvent");
    seat.has_enter = has_enter != nullptr && has_enter->is_bool() && has_enter->as_bool();
    seat.has_exit = has_exit != nullptr && has_exit->is_bool() && has_exit->as_bool();
    seat.has_event = has_event != nullptr && has_event->is_bool() && has_event->as_bool();
    if (!seat.events.empty() && !seat.has_event)
        return component_error("component.missing_on_event",
                               "the manifest binds events but the class has no onEvent",
                               component);
    seat.listener = std::make_unique<SeatListener>(*this, seat_id);

    // Record BEFORE the attach effect (the bus discipline).
    if (auto error = journal_attach(seat, state_scoped ? "state" : "base", cause_id))
        return error;

    Json call = Json::object();
    call.set("seat", static_cast<std::int64_t>(seat_id));
    call.set("index", static_cast<std::int64_t>(entity.index));
    call.set("generation", static_cast<std::int64_t>(entity.generation));
    call.set("name", std::string(component));
    call.set("fields", std::move(encoded_fields));
    EvalResult attached = runtime_->call_json(kAttachFn, call);
    if (attached.error.has_value())
        return std::move(attached.error);

    seats_.push_back(std::move(seat));
    Seat& placed = seats_.back();
    if (!state_scoped) { // base: live (and hearing) from materialization on
        placed.active = true;
        subscribe_seat(placed);
    }
    return std::nullopt;
}

ComponentInstanceHost::Seat*
ComponentInstanceHost::find_state_seat(const statechart::ComponentHookContext& context) {
    for (Seat& seat : seats_)
        if (seat.state_scoped && seat.machine == context.machine && seat.region == context.region &&
            seat.state == context.state && seat.component == context.component.view() &&
            !seat.released)
            return &seat;
    return nullptr;
}

void ComponentInstanceHost::invoke_lifecycle(Seat& seat,
                                             bool enter,
                                             const statechart::ComponentHookContext& context) {
    if (enter ? !seat.has_enter : !seat.has_exit)
        return;
    Json call = Json::object();
    call.set("seat", static_cast<std::int64_t>(&seat - seats_.data()));
    call.set("hook", enter ? "onEnter" : "onExit");
    call.set("arg", std::string(context.peer.view()));
    // The chart journaled this hook's record already (record-before-effect);
    // it is the cause of everything the hook emits.
    primitives_->push_cause(context.record_id);
    EvalResult result = runtime_->call_json(kInvokeFn, call);
    primitives_->pop_cause();
    if (result.error.has_value())
        capture_error(std::move(*result.error), context.tick, seat);
}

void ComponentInstanceHost::on_enter(statechart::Statechart& /*chart*/,
                                     const statechart::ComponentHookContext& context) {
    Seat* seat = find_state_seat(context);
    if (seat == nullptr)
        return; // registration and seats are created together; nothing to do
    seat->active = true;
    subscribe_seat(*seat); // enter order = attach order = subscription order (D1)
    invoke_lifecycle(*seat, /*enter=*/true, context);
}

void ComponentInstanceHost::on_exit(statechart::Statechart& /*chart*/,
                                    const statechart::ComponentHookContext& context) {
    Seat* seat = find_state_seat(context);
    if (seat == nullptr)
        return;
    // onExit runs while the component is still live, THEN the seat goes
    // dormant and unsubscribes — the chart's reverse iteration makes the
    // unsubscription order the D1 reverse.
    invoke_lifecycle(*seat, /*enter=*/false, context);
    unsubscribe_seat(*seat);
    seat->active = false;
}

void ComponentInstanceHost::deliver(std::size_t seat_index, const bus::EventView& event) {
    Seat& seat = seats_[seat_index];
    // The `active` gate keeps exact semantics through the bus's deferred
    // structure window (an exited seat's entry may survive to cascade end).
    if (!seat.active || seat.released)
        return;
    if (std::ranges::find(seat.events, event.event) == seat.events.end())
        return; // not one of this component's bindings

    // Record BEFORE the effect: the dispatch record cites the trigger and
    // causes everything onEvent emits.
    Json payload = Json::object();
    payload.set("entity", entity_form(seat.entity));
    payload.set("component", seat.component);
    payload.set("event", event.event.view());
    const std::uint64_t record_id = journal_->record(event.tick,
                                                     journal::Tier::Flight,
                                                     "component.on_event",
                                                     event.record_id,
                                                     std::move(payload));
    if (record_id == 0) {
        capture_error(Error{.code = "component.journal_refused",
                            .message = "the journal refused the component.on_event record"},
                      event.tick,
                      seat);
        return;
    }

    const reflect::Registered<reflect::EventDesc>* entry = registry_->find_event(event.event);
    EncodedValue encoded =
        encode_event_payload(entry != nullptr ? &entry->desc : nullptr, event.payload);
    if (encoded.error.has_value()) {
        capture_error(std::move(*encoded.error), event.tick, seat);
        return;
    }
    Json call = Json::object();
    call.set("seat", static_cast<std::int64_t>(seat_index));
    call.set("event", event.event.view());
    call.set("payload", std::move(encoded.node));
    primitives_->push_cause(record_id);
    EvalResult result = runtime_->call_json(kDispatchFn, call);
    primitives_->pop_cause();
    if (result.error.has_value())
        capture_error(std::move(*result.error), event.tick, seat);
}

} // namespace midday::script

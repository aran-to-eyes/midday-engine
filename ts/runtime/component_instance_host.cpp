// ts/runtime/component_instance_host.cpp — host plumbing: the first-party
// prelude, the support module, the manifest catalog, subscription/release
// bookkeeping, and reap. The seat lifecycle (materialize, lifecycle hooks,
// onEvent dispatch) lives in component_instance_seats.cpp; the seam
// contract in component_instance_host.h.

#include "ts/runtime/component_instance_host.h"

#include "core/base/file_io.h"
#include "core/base/hex.h"
#include "core/journal/writer.h"

#include <ranges>
#include <string>
#include <utility>

namespace midday::script {

using base::Error;
using base::Json;

// The JS half of the component seam: a seat registry keyed by integer,
// instantiation at registration (boot-deterministic), hook introspection so
// absent hooks never cross the boundary, the typed-envelope decoder
// (typed_envelope.h node shapes), attach/dispatch/release. First-party
// fixed source — like the SIM prelude, it cannot throw on load.
namespace {

constexpr std::string_view kComponentPrelude = R"js("use strict";
globalThis.__midday_component_seats = new Map();
globalThis.__midday_component_rt = null;
globalThis.__midday_component_rt_bind = function (rt) { globalThis.__midday_component_rt = rt; };
globalThis.__midday_component_decode = function (node) {
    const rt = globalThis.__midday_component_rt;
    switch (node.t) {
    case "ref": return new rt.EntityRef(node.i, node.g);
    case "vec2": return { x: node.v[0], y: node.v[1] };
    case "vec3": return { x: node.v[0], y: node.v[1], z: node.v[2] };
    case "vec4": case "quat": return { x: node.v[0], y: node.v[1], z: node.v[2], w: node.v[3] };
    case "color": return { r: node.v[0], g: node.v[1], b: node.v[2], a: node.v[3] };
    case "arr": return node.v.map(globalThis.__midday_component_decode);
    case "map": {
        // Own-keys only (council fix G8): a schema-legal map key "__proto__"
        // arrives as an OWN property from JS_ParseJSON, but a [[Set]] on a
        // plain {} would hit the Object.prototype accessor — swapping the
        // decoded object's prototype and silently DROPPING the field. A
        // null-prototype object makes every key round-trip as an own
        // data property.
        const out = Object.create(null);
        for (const key of Object.keys(node.v)) out[key] = __midday_component_decode(node.v[key]);
        return out;
    }
    default: return node.v;
    }
};
globalThis.__midday_register_component = function (seat, cls, name) {
    if (typeof cls !== "function")
        throw new TypeError("component class '" + name + "' is not exported by its module");
    __midday_component_seats.set(seat, new cls());
};
globalThis.__midday_component_hooks_of = function (seat) {
    const s = __midday_component_seats.get(seat);
    return {
        onEnter: typeof s.onEnter === "function",
        onExit: typeof s.onExit === "function",
        onEvent: typeof s.onEvent === "function",
    };
};
globalThis.__midday_component_attach = function (call) {
    const rt = globalThis.__midday_component_rt;
    const s = __midday_component_seats.get(call.seat);
    s.entity = new rt.EntityRef(call.index, call.generation);
    for (const f of call.fields) s[f.name] = __midday_component_decode(f.value);
    rt.attach(call.index, call.generation, call.name, s);
    return null;
};
globalThis.__midday_component_invoke = function (call) {
    const s = __midday_component_seats.get(call.seat);
    s[call.hook](call.arg);
    return null;
};
globalThis.__midday_component_dispatch = function (call) {
    const s = __midday_component_seats.get(call.seat);
    s.onEvent(call.event, __midday_component_decode(call.payload));
    return null;
};
globalThis.__midday_component_attach_transform = function (call) {
    const rt = globalThis.__midday_component_rt;
    const t = new rt.Transform();
    t.entity = new rt.EntityRef(call.index, call.generation);
    t.position = __midday_component_decode(call.position);
    t.rotation = __midday_component_decode(call.rotation);
    t.scale = __midday_component_decode(call.scale);
    rt.attach(call.index, call.generation, "Transform", t);
    return null;
};
globalThis.__midday_component_release = function (seat) {
    __midday_component_seats.delete(seat);
    return null;
};
)js";

// The support module: binds the engine library's EntityRef / Transform /
// __attachComponent into the prelude. A real ES module (imports resolve
// through the toolchain's resolver), generated first-party, never authored.
constexpr std::string_view kSupportSource =
    "import { EntityRef, Transform, __attachComponent } from \"midday\";\n"
    "__midday_component_rt_bind({ EntityRef, Transform, attach: __attachComponent });\n";

std::string entity_form(ecs::EntityRef ref) {
    return "entity:" + std::to_string(ref.index) + "#" + std::to_string(ref.generation);
}

} // namespace

ComponentInstanceHost::~ComponentInstanceHost() {
    // The 0A teardown fence (cli/verbs/run_sim.h): the Bus outlives this
    // host by member order, so a still-subscribed seat (an entity alive at
    // shutdown) would leave its SeatListener address dangling in the bus
    // channels between ~host and ~Bus. Unsubscribe every still-subscribed
    // seat — reverse attach order, the reap idiom. Touches bus_ ONLY (never
    // the runtime or the primitives seat: the RunSim tail hosts die first).
    for (Seat& seat : std::ranges::reverse_view(seats_))
        unsubscribe_seat(seat);
}

ComponentInstanceHost::ComponentInstanceHost(ScriptRuntime& runtime,
                                             Toolchain& toolchain,
                                             ecs::World& world,
                                             bus::Bus& bus,
                                             journal::Writer& journal,
                                             const reflect::Registry& registry,
                                             ComponentHost& primitives,
                                             std::string lib_index_path)
    : runtime_(&runtime), toolchain_(&toolchain), world_(&world), bus_(&bus), journal_(&journal),
      registry_(&registry), primitives_(&primitives), lib_index_path_(std::move(lib_index_path)) {
    // Fixed first-party source: an error here is a build defect, surfaced
    // through first_error_ so the run host still fails loudly.
    first_error_ = runtime_->eval_global(kComponentPrelude, "<midday:component-prelude>");
}

std::optional<Error> ComponentInstanceHost::ensure_support() {
    if (support_ready_)
        return std::nullopt;
    // Build the engine library through the toolchain FIRST: load_module
    // installs the module resolver, and registers the canonical module the
    // support shim's "midday" import resolves back to.
    Toolchain::LoadOutcome lib = toolchain_->load_module(*runtime_, lib_index_path_);
    if (lib.error.has_value())
        return std::move(lib.error);
    ScriptRuntime::LoadedModule shim = runtime_->load_module_source(kSupportModule, kSupportSource);
    if (shim.error.has_value())
        return std::move(shim.error);
    support_ready_ = true;
    return std::nullopt;
}

std::optional<Error> ComponentInstanceHost::load_manifest(const std::string& path) {
    base::ReadFileResult file = base::read_file(path, "component.io");
    if (file.error.has_value())
        return std::move(file.error);
    Json::ParseResult parsed = Json::parse(file.bytes, path);
    if (parsed.error.has_value())
        return base::to_error(*parsed.error);
    auto bad = [&path](std::string message) {
        return Error{.code = "component.bad_manifest", .message = path + ": " + std::move(message)};
    };
    // Fail-closed manifest identity (council fix G7): `run --components`
    // takes an arbitrary path, so a stale or foreign artifact must refuse
    // instead of silently binding against the current schema. The emitter
    // writes format_version 2 (cli/verbs/script.cpp); anything else —
    // including a missing field — refuses, naming found vs expected.
    const Json* version = parsed.value.is_object() ? parsed.value.find("format_version") : nullptr;
    if (version == nullptr || !version->is_int() || version->as_int() != 2) {
        Error error{.code = "component.manifest_version",
                    .message = path + ": unsupported component manifest format_version "
                                      "(re-run `midday script extract`)"};
        error.details.set("expected", static_cast<std::int64_t>(2));
        error.details.set("found", version != nullptr ? *version : Json());
        return error;
    }
    const Json* components = parsed.value.is_object() ? parsed.value.find("components") : nullptr;
    if (components == nullptr || !components->is_array())
        return bad("component manifest needs a 'components' array");
    for (const Json& entry : components->elements()) {
        const Json* name = entry.is_object() ? entry.find("name") : nullptr;
        const Json* module_file = entry.is_object() ? entry.find("file") : nullptr;
        if (name == nullptr || !name->is_string() || module_file == nullptr ||
            !module_file->is_string())
            return bad("every component entry needs string 'name' and 'file'");
        if (find_manifest(name->as_string()) != nullptr)
            return bad("component '" + name->as_string() + "' is declared twice");
        ManifestComponent component;
        component.name = name->as_string();
        component.file = module_file->as_string();
        if (const Json* fields = entry.find("fields"); fields != nullptr && fields->is_array()) {
            for (const Json& field : fields->elements()) {
                const Json* field_name = field.is_object() ? field.find("name") : nullptr;
                const Json* field_type = field.is_object() ? field.find("type") : nullptr;
                if (field_name == nullptr || !field_name->is_string() || field_type == nullptr ||
                    !field_type->is_string())
                    return bad("component '" + component.name +
                               "': every field needs string 'name' and 'type'");
                std::optional<reflect::TypeDesc> type =
                    reflect::TypeDesc::parse(field_type->as_string());
                if (!type.has_value())
                    return bad("component '" + component.name + "' field '" +
                               field_name->as_string() + "': unknown type '" +
                               field_type->as_string() + "'");
                component.fields.push_back(
                    ManifestField{field_name->as_string(), std::move(*type)});
            }
        }
        if (const Json* bindings = entry.find("event_bindings");
            bindings != nullptr && bindings->is_array()) {
            for (const Json& binding : bindings->elements()) {
                const Json* event = binding.is_object() ? binding.find("event") : nullptr;
                if (event == nullptr || !event->is_string())
                    return bad("component '" + component.name +
                               "': every event binding needs a string 'event'");
                // Per-binding schema pin (council fix G7): the manifest
                // carries payload_compat_hash precisely so an extraction
                // against an older schema refuses HERE, not at dispatch.
                // Registered events must match the run registry's hash
                // exactly; unregistered names are the D-BUILD-046 custom
                // pass-through vocabulary — no engine schema exists to
                // drift from, and dispatch hydrates them verbatim.
                const Json* hash = binding.find("payload_compat_hash");
                if (hash != nullptr && !hash->is_string())
                    return bad("component '" + component.name + "': binding '" +
                               event->as_string() + "' payload_compat_hash must be a string");
                if (const reflect::Registered<reflect::EventDesc>* schema =
                        registry_->find_event(base::Name(event->as_string()));
                    schema != nullptr) {
                    const std::string expected = base::hex64(schema->desc.compat_hash);
                    const std::string found = hash != nullptr ? hash->as_string() : std::string();
                    if (found != expected) {
                        Error error{.code = "component.payload_hash",
                                    .message = path + ": component '" + component.name +
                                               "' binds '" + event->as_string() +
                                               "' against a different payload schema than this "
                                               "engine's (re-run `midday script extract`)"};
                        error.details.set("component", component.name);
                        error.details.set("event", event->as_string());
                        error.details.set("found", found);
                        error.details.set("expected", expected);
                        return error;
                    }
                }
                component.events.emplace_back(event->as_string());
            }
        }
        manifest_.push_back(std::move(component));
    }
    return std::nullopt;
}

const ComponentInstanceHost::ManifestComponent*
ComponentInstanceHost::find_manifest(std::string_view name) const {
    for (const ManifestComponent& component : manifest_)
        if (component.name == name)
            return &component;
    return nullptr;
}

std::optional<Error> ComponentInstanceHost::journal_attach(const Seat& seat,
                                                           std::string_view mode,
                                                           std::uint64_t cause_id) const {
    Json payload = Json::object();
    payload.set("entity", entity_form(seat.entity));
    payload.set("component", seat.component);
    payload.set("mode", mode);
    if (seat.state_scoped) {
        payload.set("region", seat.region.view());
        payload.set("state", seat.state.view());
    }
    const std::uint64_t record_id = journal_->record(
        bus_->tick(), journal::Tier::Flight, "component.attach", cause_id, std::move(payload));
    if (record_id == 0) {
        Error error{.code = "component.journal_refused",
                    .message = "the journal refused the component.attach record"};
        const std::optional<Error>& status = journal_->status();
        if (status.has_value())
            error.details.set("journal", status->to_json());
        return error;
    }
    return std::nullopt;
}

void ComponentInstanceHost::subscribe_seat(Seat& seat) {
    if (seat.subscribed || !seat.has_event || seat.events.empty() || seat.released)
        return;
    // Mid-dispatch this defers to the cascade's end (bus rule D-BUILD-047);
    // the `subscribed` flag tracks the LOGICAL state either way, and
    // deliver()'s `active` gate keeps semantics exact in the window.
    (void)bus_->subscribe_entity_listener(
        bus::EventKey::entity(seat.entity), seat.entity, *seat.listener);
    seat.subscribed = true;
}

void ComponentInstanceHost::unsubscribe_seat(Seat& seat) {
    if (!seat.subscribed)
        return;
    // A stale entity's entry may already be auto-dropped: counted no-op.
    (void)bus_->unsubscribe_entity_listener(
        bus::EventKey::entity(seat.entity), seat.entity, *seat.listener);
    seat.subscribed = false;
}

void ComponentInstanceHost::release_seat(Seat& seat) {
    if (seat.released)
        return;
    unsubscribe_seat(seat);
    seat.active = false;
    seat.released = true;
    const auto seat_id = static_cast<std::int64_t>(&seat - seats_.data());
    EvalResult released = runtime_->call_json(kReleaseFn, Json(seat_id));
    if (released.error.has_value())
        capture_error(std::move(*released.error), bus_->tick(), seat);
}

void ComponentInstanceHost::despawn_exit(ecs::EntityRef ref, std::uint64_t cause_id) {
    for (Seat& seat : std::ranges::reverse_view(seats_)) {
        if (seat.state_scoped || seat.entity != ref || seat.released || !seat.active ||
            !seat.has_exit || seat.despawn_exited)
            continue;
        Json payload = Json::object();
        payload.set("entity", entity_form(seat.entity));
        payload.set("component", seat.component);
        const std::uint64_t record_id = journal_->record(bus_->tick(),
                                                         journal::Tier::Flight,
                                                         "component.despawn_exit",
                                                         cause_id,
                                                         std::move(payload));
        if (record_id == 0) {
            capture_error(Error{.code = "component.journal_refused",
                                .message = "the journal refused the component.despawn_exit record"},
                          bus_->tick(),
                          seat);
            return;
        }
        // Marked BETWEEN record and invoke: overlapping same-tick reap
        // walks (nested lingers — the child's own due entry, then its
        // ancestor's subtree pass over the still-unflushed child) run the
        // base onExit exactly ONCE, under the first walk's record.
        seat.despawn_exited = true;
        Json call = Json::object();
        call.set("seat", static_cast<std::int64_t>(&seat - seats_.data()));
        call.set("hook", "onExit");
        call.set("arg", ""); // no target state at despawn
        primitives_->push_cause(record_id);
        EvalResult result = runtime_->call_json(kInvokeFn, call);
        primitives_->pop_cause();
        if (result.error.has_value())
            capture_error(std::move(*result.error), bus_->tick(), seat);
    }
}

void ComponentInstanceHost::note_despawn(ecs::EntityRef ref, std::uint64_t reap_tick) {
    // The one-line third of loader::DespawnHooks (the 0A G2 closure): the
    // primitives seat owns the despawn-tick record — status() answers from
    // it, and ts/lib/component.ts's script.stale_ref message names the tick.
    primitives_->note_despawn(ref, reap_tick);
}

void ComponentInstanceHost::reap_entity(ecs::EntityRef ref) {
    // State seats first, then base seats, each group REVERSE attach order
    // (D2's reap rule). Instance lifetime ended with the entity: the JS
    // seat releases; the directory bucket dies on slot reuse (component.ts
    // re-validates generations, so it is inert until then).
    for (Seat& seat : std::ranges::reverse_view(seats_))
        if (seat.state_scoped && seat.entity == ref)
            release_seat(seat);
    for (Seat& seat : std::ranges::reverse_view(seats_))
        if (!seat.state_scoped && seat.entity == ref)
            release_seat(seat);
}

void ComponentInstanceHost::capture_error(Error error, std::uint64_t tick, const Seat& seat) {
    if (first_error_.has_value())
        return;
    annotate_sim_context(error, tick, "");
    error.details.set("component", seat.component);
    error.details.set("entity", entity_form(seat.entity));
    first_error_ = std::move(error);
}

} // namespace midday::script

// ts/runtime/component_instance_host.h — the TS component instance host
// (M2 0B, #12b): the ONE owner of live script-component instances. It
// implements ALL THREE core-side seams at once (the third — loader::
// DespawnHooks, the spawner's phase-8 despawn lifecycle — is track D;
// production wiring is one line: spawner.set_despawn_hooks(host)):
//   * loader::ScriptComponentMaterializer — the loader dispatcher
//     (core/loader/component_materialize.h) routes every authored script
//     component here: manifest lookup, module build through the toolchain
//     cache, class instantiation on the SIM runtime, typed FIELD hydration
//     (manifest TypeDesc over the authored values), `__attachComponent`
//     into the ts/lib/component.ts directory, and — for base components —
//     the entity-bound bus subscription.
//   * statechart::ComponentHooks — the chart's enter-2/exit-3 slots drive
//     state-scoped seats: onEnter subscribes (attach order = the chart's
//     call order) and invokes the instance's onEnter?; onExit invokes
//     onExit? then unsubscribes (the chart iterates exits in reverse attach
//     order, so unsubscription order is the D1 reverse).
//
// INSTANCE LIFETIME = ENTITY LIFETIME (D2): state activation toggles
// subscription/hook activity, never object identity. reap_entity() (the
// despawn path, track D) releases state seats first, then base seats, each
// group in reverse attach order.
//
// onEvent DISPATCH (D1): each seat owns ONE stable listener, subscribed to
// its entity's private channel through the bus's entity-bound
// listener-pointer flavor (generation-checked; TS components have no C++
// pool type). Bus registration order IS dispatch order, and this host
// subscribes in materialization order — base authored order, state seats at
// enter. A delivery filters against the seat's manifest binding table,
// journals ONE FLIGHT "component.on_event" record (cause: the trigger),
// hydrates the payload through the typed envelope (typed_envelope.h;
// EntityRef fields become real EntityRef instances, vec fields their
// {x, y, z} shapes), and invokes `onEvent(event, payload)` with the record
// as the emit cause frame (ComponentHost::push_cause).
//
// Everything journals BEFORE its effect: "component.attach" per
// materialized seat, "component.on_event" per delivered dispatch; the
// chart's own "statechart.hook" component_enter/component_exit records
// bracket the lifecycle hooks. Script exceptions inside a hook/dispatch
// cannot unwind the machinery — first_error() carries the first one,
// sim-tick annotated (the StateScriptHost discipline).

#pragma once

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/loader/component_materialize.h"
#include "core/reflect/registry.h"
#include "core/reflect/type_model.h"
#include "core/statechart/component_hooks.h"
#include "core/statechart/statechart.h"
#include "ts/runtime/component_host.h"
#include "ts/runtime/script_runtime.h"
#include "ts/toolchain/toolchain.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::journal {
class Writer;
}

namespace midday::script {

class ComponentInstanceHost final : public loader::ScriptComponentMaterializer,
                                    public statechart::ComponentHooks,
                                    public loader::DespawnHooks {
public:
    // The seam contract (the component_host.h kStatusFn-style pins): the
    // first-party prelude defines these exact globals. NOT a generated
    // seam — no bindings_spec entry, no regen (0B's one regen is spent).
    static constexpr std::string_view kRegisterFn = "__midday_register_component";
    static constexpr std::string_view kIntrospectFn = "__midday_component_hooks_of";
    static constexpr std::string_view kAttachFn = "__midday_component_attach";
    static constexpr std::string_view kInvokeFn = "__midday_component_invoke";
    static constexpr std::string_view kDispatchFn = "__midday_component_dispatch";
    static constexpr std::string_view kAttachTransformFn = "__midday_component_attach_transform";
    static constexpr std::string_view kReleaseFn = "__midday_component_release";
    static constexpr std::string_view kSupportModule = "<midday:component-rt>";

    // All collaborators must outlive this host; the host must outlive every
    // chart that holds its ComponentHooks registrations (destroy order:
    // Statechart first — the StateHooks lifetime contract). `primitives` is
    // the entity/emit seat (component_host.h): its trigger functions gain
    // this host's cause frames. `lib_index_path` is ToolchainConfig::
    // lib_ts_dir + "/index.ts" — the engine library module the support shim
    // binds EntityRef/Transform/__attachComponent from.
    ComponentInstanceHost(ScriptRuntime& runtime,
                          Toolchain& toolchain,
                          ecs::World& world,
                          bus::Bus& bus,
                          journal::Writer& journal,
                          const reflect::Registry& registry,
                          ComponentHost& primitives,
                          std::string lib_index_path = "ts/lib/index.ts");

    ComponentInstanceHost(const ComponentInstanceHost&) = delete;
    ComponentInstanceHost& operator=(const ComponentInstanceHost&) = delete;
    ComponentInstanceHost(ComponentInstanceHost&&) = delete;
    ComponentInstanceHost& operator=(ComponentInstanceHost&&) = delete;
    // Unsubscribes every still-subscribed seat listener from the bus (the
    // 0A teardown fence: the bus outlives this host — no dangling
    // SeatListener may survive in its channels). Defined where SeatListener
    // is complete.
    ~ComponentInstanceHost();

    // The project component manifest (`midday script extract --out`):
    // name -> {file, fields, event_bindings}. FAIL-CLOSED identity (council
    // fix G7): format_version must be exactly 2
    // ("component.manifest_version", found vs expected), and every binding
    // on a REGISTERED event must carry the run registry's payload_compat_
    // hash ("component.payload_hash", event + both hashes) — a manifest
    // extracted against an older schema refuses at load, never binds
    // silently. Unregistered event names are the D-BUILD-046 pass-through
    // vocabulary: no engine schema exists, no hash is checked. Merges
    // across calls; a duplicate component name refuses.
    std::optional<base::Error> load_manifest(const std::string& path);

    // loader::ScriptComponentMaterializer ------------------------------------
    std::optional<base::Error> materialize_base(ecs::EntityRef entity,
                                                const loader::GenericComponentEntry& entry,
                                                std::uint64_t cause_id) override;
    std::optional<base::Error> materialize_state(statechart::Statechart& chart,
                                                 statechart::MachineId machine,
                                                 base::Name region,
                                                 base::Name state,
                                                 ecs::EntityRef host,
                                                 const statechart::StateComponentDesc& desc,
                                                 std::uint64_t cause_id) override;
    std::optional<base::Error> mirror_native_transform(ecs::EntityRef entity,
                                                       const math::Transform& value,
                                                       std::uint64_t cause_id) override;

    // statechart::ComponentHooks (the enter-2 / exit-3 slots) ----------------
    void on_enter(statechart::Statechart& chart,
                  const statechart::ComponentHookContext& context) override;
    void on_exit(statechart::Statechart& chart,
                 const statechart::ComponentHookContext& context) override;

    // loader::DespawnHooks — the PrefabSpawner's phase-8 despawn seam
    // (track D; prefab_spawn.h wires this host via set_despawn_hooks) ------

    // The despawn-path BASE-component exit hooks (D4 phase-8 prepare, track
    // D): after the statechart exit chains ran (state seats exit THROUGH
    // the chart's exit-3 slot), invoke onExit on `ref`'s base seats in
    // REVERSE attach order. Journals one FLIGHT "component.despawn_exit"
    // per invocation (citing `cause_id`; the record is the emit cause
    // frame); onExit's peer argument is "" — there is no target state.
    // Idempotent per seat (overlapping same-tick reap walks — nested
    // lingers — run each base onExit exactly once, under the FIRST walk's
    // record). Does NOT release seats: call reap_entity after the flush.
    void despawn_exit(ecs::EntityRef ref, std::uint64_t cause_id) override;

    // The 0A G2 closure: record the actual REAP tick on the primitives seat
    // (ComponentHost::note_despawn) — a later stale-ref query for exactly
    // this (index, generation) names the tick instead of null.
    void note_despawn(ecs::EntityRef ref, std::uint64_t reap_tick) override;

    // Reap (the despawn path, track D): `ref`'s state seats first, then its
    // base seats, each group in REVERSE attach order — unsubscribe (a
    // counted no-op if the bus already auto-dropped the stale entry) and
    // release the JS instance. Idempotent per seat; invokes NO hooks
    // (despawn_exit + the chart's exit chains own those, BEFORE the flush).
    void reap_entity(ecs::EntityRef ref) override;

    // The first hook/dispatch error, sim-tick annotated (empty = clean).
    [[nodiscard]] const std::optional<base::Error>& first_error() const { return first_error_; }

    [[nodiscard]] std::size_t seat_count() const { return seats_.size(); }

private:
    struct ManifestField {
        std::string name;
        reflect::TypeDesc type = reflect::TypeDesc::scalar(reflect::TypeKind::kFloat);
    };

    struct ManifestComponent {
        std::string name;
        std::string file;
        std::vector<ManifestField> fields;
        std::vector<base::Name> events; // binding table, manifest order
    };

    // One live component instance. `listener` is heap-anchored: the bus
    // stores its address (stable across seats_ growth).
    struct SeatListener;

    struct Seat {
        std::string component;
        ecs::EntityRef entity; // the OWNING entity (channel + directory key)
        bool state_scoped = false;
        statechart::MachineId machine = statechart::kInvalidMachine;
        base::Name region;
        base::Name state;
        std::vector<base::Name> events; // bound events (manifest order)
        bool has_enter = false;
        bool has_exit = false;
        bool has_event = false;
        bool active = false;         // base: from attach; state: enter/exit toggles
        bool subscribed = false;     // logical subscription state (bus ops may defer)
        bool released = false;       // reaped
        bool despawn_exited = false; // base onExit ran (idempotent across nested reap walks)
        std::unique_ptr<SeatListener> listener;
    };

    [[nodiscard]] const ManifestComponent* find_manifest(std::string_view name) const;
    [[nodiscard]] Seat* find_state_seat(const statechart::ComponentHookContext& context);
    // The shared materialization tail (manifest -> module -> instance ->
    // fields -> journal -> attach); state seats add hook registration and
    // defer their subscription to on_enter.
    std::optional<base::Error> materialize_seat(std::string_view component,
                                                const base::Json& fields,
                                                ecs::EntityRef entity,
                                                std::uint64_t cause_id,
                                                bool state_scoped,
                                                statechart::MachineId machine,
                                                base::Name region,
                                                base::Name state);
    std::optional<base::Error> ensure_support();
    [[nodiscard]] std::optional<base::Error>
    journal_attach(const Seat& seat, std::string_view mode, std::uint64_t cause_id) const;
    void subscribe_seat(Seat& seat);
    void unsubscribe_seat(Seat& seat);
    void release_seat(Seat& seat);
    // Bus delivery -> binding filter -> journal -> hydrate -> JS onEvent.
    void deliver(std::size_t seat_index, const bus::EventView& event);
    // Invoke onEnter/onExit on the JS instance with a cause frame.
    void invoke_lifecycle(Seat& seat, bool enter, const statechart::ComponentHookContext& context);
    void capture_error(base::Error error, std::uint64_t tick, const Seat& seat);

    ScriptRuntime* runtime_;
    Toolchain* toolchain_;
    ecs::World* world_;
    bus::Bus* bus_;
    journal::Writer* journal_;
    const reflect::Registry* registry_;
    ComponentHost* primitives_;
    std::string lib_index_path_;
    bool support_ready_ = false;
    std::vector<ManifestComponent> manifest_;
    std::vector<Seat> seats_;
    std::optional<base::Error> first_error_;
};

// The stable bus delivery target: ONE per seat, heap-anchored so its
// address survives seats_ growth (the bus stores the pointer). Defined
// here — both host TUs (plumbing + seats) need it complete.
struct ComponentInstanceHost::SeatListener final : bus::EventListener {
    ComponentInstanceHost* host = nullptr;
    std::size_t seat = 0;

    SeatListener(ComponentInstanceHost& host_in, std::size_t seat_in)
        : host(&host_in), seat(seat_in) {}

    void on_event(bus::Bus& /*bus*/, const bus::EventView& event) override {
        host->deliver(seat, event);
    }
};

} // namespace midday::script

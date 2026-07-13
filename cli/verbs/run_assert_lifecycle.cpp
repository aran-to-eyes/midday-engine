// cli/verbs/run_assert_lifecycle.cpp — the "component_event_lifecycle" pack
// (M2 node 0B, FUSED-SPEC D6): the milestone's THIRD golden, driven over
// the authored examples/lifecycle corpus for 241 ticks @ 60 Hz. Every
// expected value below derives from the DESIGN — the codec's pinned v1
// layout, the A.2.1 exit template, ceil(4.0 * 60) — never from observed
// output.
//
// The timeline:
//   * tick 1, phase 5 (this driver — the phase A.3 assigns synthetic
//     systems): contact.began triggers at the probe's private channel with
//     DISTINCT self/other refs, asymmetric position [7, -0.0, 3] and
//     impulse -0.0 (the -0s make the float falsifier non-vacuous).
//   * ContactRelay (base component) hydrates the typed payload — real
//     EntityRefs, Vec3, the -0s normalized to +0 by the canonical codec —
//     reads the MIRRORED base Transform (x == 7), journals every verdict
//     as relay.verify, and emits golden.kill.
//   * The kill transitions life Alive/Armed -> Dead INSIDE the cascade;
//     the exit chain journals EXACTLY seven hook invocations (the A.2.1
//     order pinned line by line below).
//   * The REAL examples/warden/states/dead.ts enters: broadcasts boss.died
//     @ global (once, AT ENTER — never at the reap) and schedules
//     world.despawn(self, {after: 4.0}) -> due = 1 + ceil(4.0 * 60) = 241.
//   * The corpse stays fully alive through phase 7 of tick 241; phase 8
//     runs prefab.despawn -> the Dead exit chain -> ContactRelay's
//     component.despawn_exit -> flush -> entity.despawned, all citing the
//     despawn record (D4/G2).
//
// D5 rides along: the contact.began record's payload_bytes are pinned as a
// LITERAL hex golden (self = entity 0#0, other = entity 6#0 — fixed by
// scene document order), and every enveloped trigger in the bundle is
// DECODE-verified against the run registry (bytes authoritative, the
// projection must be exactly their decode — replay's reading discipline).

#include "cli/verbs/run_assert.h"
#include "cli/verbs/run_assert_walk.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/reflect/registry.h"
#include "core/statechart/statechart.h"
#include "core/tick/tick_loop.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace midday::cli {
namespace {

using assertwalk::is_event_trigger;
using assertwalk::payload_is;
using journal::Record;

// ---- the designed timeline -------------------------------------------------
constexpr std::uint64_t kContactTick = 1;
// dead.ts requests {after: 4.0} from tick 1: 1 + ceil(4.0 * 60) = 241
// (4.0 * 60 = 240.0 exactly in IEEE doubles — no ceiling surprise).
constexpr std::uint64_t kReapTick = 241;

// The canonical v1 bytes of the injected contact.began payload
// {self: 0#0, other: 6#0, position: [7, -0.0, 3], normal: [0, 1, 0],
// impulse: -0.0}, derived from the pinned wire layout (payload_codec.h;
// the -0s normalize to +0): version 01, compat hash 08d68516245c6356 LE,
// field count 5, presence 1f, then entity_ref(0), entity_ref(6),
// vec3(7,+0,3), vec3(0,1,0), float(+0).
constexpr const char* kContactBytesHex =
    "0156635c241685d608050000001f"                       // header + presence
    "0b0000000000000000"                                 // self  = bits 0
    "0b0600000000000000"                                 // other = bits 6
    "070000000000001c4000000000000000000000000000000840" // position [7,+0,3]
    "070000000000000000000000000000f03f0000000000000000" // normal   [0,1,0]
    "030000000000000000";                                // impulse  +0

class ComponentEventLifecyclePack final : public RunAssertPack,
                                          public tick::PhaseHook,
                                          public bus::EventListener {
public:
    ComponentEventLifecyclePack() = default;
    ComponentEventLifecyclePack(const ComponentEventLifecyclePack&) = delete;
    ComponentEventLifecyclePack& operator=(const ComponentEventLifecyclePack&) = delete;
    ComponentEventLifecyclePack(ComponentEventLifecyclePack&&) = delete;
    ComponentEventLifecyclePack& operator=(ComponentEventLifecyclePack&&) = delete;

    ~ComponentEventLifecyclePack() override {
        if (loop_ != nullptr)
            (void)loop_->remove_hook(tick::Phase::kUpdate, *this);
        if (bus_ != nullptr)
            (void)bus_->unsubscribe(*this, bus::EventKey::named(base::Name("global")));
    }

    [[nodiscard]] std::string_view name() const override { return "component_event_lifecycle"; }

    std::optional<Error> attach(ecs::World& live_world,
                                hierarchy::Hierarchy& /*hierarchy*/,
                                bus::Bus& event_bus,
                                tick::TickLoop& tick_loop,
                                journal::Writer& flight_recorder,
                                const reflect::Registry& vocabulary) override {
        // The driver tail first (its hook/subscription only ever fire at
        // tick/dispatch time, never inside attach), then the seats.
        std::optional<Error> attached =
            assertwalk::attach_update_driver(tick_loop, event_bus, *this);
        registry_ = &vocabulary; // the D5 decode-verify schema source
        writer_ = &flight_recorder;
        loop_ = &tick_loop;
        bus_ = &event_bus;
        world_ = &live_world;
        return attached;
    }

    std::optional<Error> bind(statechart::Statechart& chart,
                              const loader::SpawnResult& spawned,
                              std::uint64_t cause_id) override {
        chart_ = &chart;
        const BoundActors actors =
            locate_actors(*world_, spawned, base::Name("Probe"), base::Name("Bystander"));
        machine_ = actors.machine;
        probe_ = actors.host;
        bystander_ = actors.marker;
        if (machine_ == statechart::kInvalidMachine || bystander_.is_null()) {
            Error error{.code = "run.assert_scene",
                        .message = "component_event_lifecycle expects the examples/lifecycle "
                                   "corpus: entity 'Probe' carrying the lifecycle machine and a "
                                   "distinct 'Bystander' entity"};
            return error;
        }
        return assertwalk::journal_case_presence(*writer_, name(), cause_id);
    }

    // The phase-5 driver: ONE injected contact at tick 1; the whole kill
    // cascade (hydration -> golden.kill -> exit chain -> dead.ts) runs
    // inline on this trigger's stack. At the due tick the same hook probes
    // the corpse still alive in phase 5 (phases 1-7 belong to the corpse).
    void on_phase(tick::TickLoop& /*loop*/, const tick::PhaseContext& context) override {
        if (context.tick == kReapTick) {
            corpse_alive_due_phase5_ = world_->alive(probe_);
            return;
        }
        if (context.tick != kContactTick)
            return;
        armed_before_hit_ = chart_->in_state(machine_, base::Name("life"), base::Name("Alive")) &&
                            chart_->in_state(machine_, base::Name("life"), base::Name("Armed"));

        Json payload = Json::object();
        payload.set("self", static_cast<std::int64_t>(probe_.to_bits()));
        payload.set("other", static_cast<std::int64_t>(bystander_.to_bits()));
        Json position = Json::array();
        position.push(7.0);
        position.push(-0.0); // MUST normalize to +0 in bytes AND projection
        position.push(3.0);
        payload.set("position", std::move(position));
        Json normal = Json::array();
        normal.push(0.0);
        normal.push(1.0);
        normal.push(0.0);
        payload.set("normal", std::move(normal));
        payload.set("impulse", -0.0);
        (void)bus_->trigger(bus::EventKey::entity(probe_),
                            base::Name("contact.began"),
                            payload,
                            context.phase_record_id);
        dead_after_hit_ = chart_->in_state(machine_, base::Name("life"), base::Name("Dead"));
    }

    // The "UI/score listener" stand-in: boss.died arrives mid-cascade,
    // inside dead.ts's onEnter — the corpse must still be fully alive.
    void on_event(bus::Bus& /*bus*/, const bus::EventView& event) override {
        if (event.event != base::Name("boss.died"))
            return;
        boss_died_live_tick_ = event.tick;
        probe_alive_at_boss_died_ = world_->alive(probe_);
    }

    Verdict evaluate(statechart::Statechart& chart,
                     const std::string& bundle,
                     const RunProbes& probes) override;

private:
    struct Facts; // journal-walk collector (below)

    ecs::World* world_ = nullptr;
    bus::Bus* bus_ = nullptr;
    tick::TickLoop* loop_ = nullptr;
    journal::Writer* writer_ = nullptr;
    const reflect::Registry* registry_ = nullptr;
    statechart::Statechart* chart_ = nullptr;
    statechart::MachineId machine_ = statechart::kInvalidMachine;
    ecs::EntityRef probe_;
    ecs::EntityRef bystander_;
    // Live probes (evaluate() reconciles them with the journal).
    bool armed_before_hit_ = false;
    bool dead_after_hit_ = false;
    std::uint64_t boss_died_live_tick_ = 0;
    bool probe_alive_at_boss_died_ = false;
    bool corpse_alive_due_phase5_ = false;
};

// One streaming pass, keeping exactly what the D6 assertions cite.
struct ComponentEventLifecyclePack::Facts {
    const reflect::Registry* registry = nullptr;

    std::vector<std::string> entry_hooks; // tick-0 statechart.hook spellings
    std::vector<std::string> exit_hooks;  // tick-1 statechart.hook spellings
    std::vector<std::uint64_t> exit_hook_causes;
    std::optional<Record> contact;        // the injected contact.began
    std::optional<Record> relay_dispatch; // component.on_event (ContactRelay)
    std::optional<Record> relay_verify;   // the hydration verdicts
    std::optional<Record> golden_kill;
    std::optional<Record> transition; // life Alive -> Dead @ 1
    std::optional<Record> enter_dead; // the 7th line (cause of boss.died)
    int boss_died_count = 0;
    std::optional<Record> boss_died;
    std::optional<Record> prefab_despawn; // @241 {entity, requested, due}
    std::optional<Record> exit_dead_hook; // @241, the despawn exit chain
    std::optional<Record> despawn_exit;   // component.despawn_exit @241
    std::optional<Record> despawned;      // entity.despawned @241
    int enveloped_triggers = 0;           // records carrying payload_codec
    int envelope_verified = 0;            // ... whose bytes decode-verify

    static std::string hook_spelling(const Record& record) {
        const std::string* hook = assertwalk::payload_str(record, "hook");
        const std::string* state = assertwalk::payload_str(record, "state");
        std::string line = (hook != nullptr ? *hook : std::string("?")) + ":" +
                           (state != nullptr ? *state : std::string("?"));
        if (const base::Json* component = record.payload.find("component"))
            line += "/" + component->as_string();
        return line;
    }

    void collect(const Record& record) {
        if (record.kind == "statechart.hook") {
            if (record.tick == 0)
                entry_hooks.push_back(hook_spelling(record));
            if (record.tick == kContactTick) {
                exit_hooks.push_back(hook_spelling(record));
                exit_hook_causes.push_back(record.cause_id);
                if (payload_is(record, "hook", "enter") && payload_is(record, "state", "Dead"))
                    enter_dead = record;
            }
            if (record.tick == kReapTick && payload_is(record, "hook", "exit") &&
                payload_is(record, "state", "Dead"))
                exit_dead_hook = record;
            return;
        }
        if (record.kind == "statechart.transition" && record.tick == kContactTick &&
            payload_is(record, "region", "life")) {
            transition = record;
            return;
        }
        if (record.kind == "component.on_event" &&
            payload_is(record, "component", "ContactRelay")) {
            relay_dispatch = record;
            return;
        }
        if (record.kind == "prefab.despawn") {
            prefab_despawn = record;
            return;
        }
        if (record.kind == "component.despawn_exit" &&
            payload_is(record, "component", "ContactRelay")) {
            despawn_exit = record;
            return;
        }
        if (record.kind != "event.trigger")
            return;
        // D5 reader discipline: every enveloped trigger decode-verifies.
        if (record.payload.find("payload_codec") != nullptr) {
            ++enveloped_triggers;
            const std::string* event = assertwalk::payload_str(record, "event");
            const auto* entry =
                event != nullptr ? registry->find_event(base::Name(*event)) : nullptr;
            if (entry != nullptr && assertwalk::canonical_payload_verified(entry->desc, record))
                ++envelope_verified;
        }
        if (is_event_trigger(record, "contact.began"))
            contact = record;
        else if (is_event_trigger(record, "relay.verify"))
            relay_verify = record;
        else if (is_event_trigger(record, "golden.kill"))
            golden_kill = record;
        else if (is_event_trigger(record, "boss.died")) {
            ++boss_died_count;
            boss_died = record;
        } else if (is_event_trigger(record, "entity.despawned")) {
            despawned = record;
        }
    }
};

RunAssertPack::Verdict ComponentEventLifecyclePack::evaluate(statechart::Statechart& /*chart*/,
                                                             const std::string& bundle,
                                                             const RunProbes& probes) {
    Verdict verdict;
    Facts facts;
    facts.registry = registry_;
    if (auto error = assertwalk::walk_bundle(bundle, facts)) {
        verdict.error = std::move(error);
        return verdict;
    }

    const auto& id_of = assertwalk::record_id_of; // shared probes (walk header)
    const auto cites = &assertwalk::record_cites;
    const auto verdict_field = [&](std::string_view key) -> const base::Json* {
        if (!facts.relay_verify.has_value())
            return nullptr;
        const base::Json* body = facts.relay_verify->payload.find("payload");
        return body != nullptr && body->is_object() ? body->find(key) : nullptr;
    };
    const auto verdict_true = [&](std::string_view key) {
        const base::Json* value = verdict_field(key);
        return value != nullptr && value->is_bool() && value->as_bool();
    };
    const auto verdict_num = [&](std::string_view key, double expected) {
        const base::Json* value = verdict_field(key);
        return value != nullptr && value->is_number() &&
               (value->is_int() ? static_cast<double>(value->as_int()) : value->as_double()) ==
                   expected;
    };

    // D2 seating: the deferred split means tick-zero enter chains reach
    // every seated component and script, in the A.2.1 order.
    const std::vector<std::string> entry_expected = {
        "component_enter:Alive/ParentExitA",
        "component_enter:Alive/ParentExitB",
        "component_enter:Armed/ChildExitA",
        "component_enter:Armed/ChildExitB",
        "enter:Armed",
        "enter:Alive",
    };
    const bool initial_entry_seated_order = facts.entry_hooks == entry_expected;

    // THE 7-line exit chain (D6): brain first while its parts live,
    // substates deepest-completing, components REVERSE attach per level,
    // then Dead enters — every line citing the ONE kill transition.
    const std::vector<std::string> exit_expected = {
        "exit:Alive",
        "exit:Armed",
        "component_exit:Armed/ChildExitB",
        "component_exit:Armed/ChildExitA",
        "component_exit:Alive/ParentExitB",
        "component_exit:Alive/ParentExitA",
        "enter:Dead",
    };
    bool exit_hooks_cite_transition = facts.transition.has_value();
    if (exit_hooks_cite_transition)
        for (const std::uint64_t cause : facts.exit_hook_causes)
            exit_hooks_cite_transition =
                exit_hooks_cite_transition && cause == id_of(facts.transition);
    const bool exit_chain_seven_lines =
        facts.exit_hooks == exit_expected && exit_hooks_cite_transition;

    // D5: the injected contact's canonical bytes, pinned LITERALLY.
    const bool contact_payload_bytes_pinned =
        facts.contact.has_value() && facts.contact->tick == kContactTick &&
        payload_is(*facts.contact, "payload_codec", reflect::kPayloadCodecName) &&
        payload_is(*facts.contact, "payload_schema", "08d68516245c6356") &&
        payload_is(*facts.contact, "payload_bytes", kContactBytesHex);
    const bool canonical_payloads_verified =
        facts.enveloped_triggers >= 5 && facts.envelope_verified == facts.enveloped_triggers;

    // D1: typed hydration, verified INSIDE the component and journaled.
    const bool typed_hydration_verified =
        facts.relay_verify.has_value() && verdict_true("self_is_me") &&
        verdict_true("other_distinct") && verdict_num("x", 7.0) && verdict_num("z", 3.0);
    const bool signed_zero_normalized =
        verdict_true("y_plus_zero") && verdict_true("impulse_plus_zero");
    const bool base_transform_mirror_read = verdict_num("tx", 7.0);

    // The cause chain: contact -> dispatch -> golden.kill -> transition
    // (the 7 hooks cite it above) -> enter:Dead -> boss.died @ global.
    const bool kill_cause_chain =
        cites(facts.relay_dispatch, facts.contact) &&
        cites(facts.golden_kill, facts.relay_dispatch) &&
        cites(facts.transition, facts.golden_kill) && cites(facts.boss_died, facts.enter_dead) &&
        facts.transition.has_value() && payload_is(*facts.transition, "from", "Alive") &&
        payload_is(*facts.transition, "to", "Dead") && armed_before_hit_ && dead_after_hit_;

    // dead.ts fires boss.died AT ENTER (tick 1), exactly once — never at
    // the reap — carrying the probe ref and its mirrored Transform.
    const base::Json* boss_body =
        facts.boss_died.has_value() ? facts.boss_died->payload.find("payload") : nullptr;
    const base::Json* boss_ref =
        boss_body != nullptr && boss_body->is_object() ? boss_body->find("boss") : nullptr;
    const base::Json* boss_at =
        boss_body != nullptr && boss_body->is_object() ? boss_body->find("at") : nullptr;
    const bool boss_died_at_enter_once =
        facts.boss_died_count == 1 && facts.boss_died.has_value() &&
        facts.boss_died->tick == kContactTick && payload_is(*facts.boss_died, "key", "global") &&
        boss_died_live_tick_ == kContactTick && probe_alive_at_boss_died_ && boss_ref != nullptr &&
        boss_ref->is_int() && boss_ref->as_int() == static_cast<std::int64_t>(probe_.to_bits()) &&
        boss_at != nullptr && boss_at->dump() == "[7,0,3]";

    // D4: the linger arithmetic — requested tick 1, due 1 + ceil(4*60).
    const base::Json* due =
        facts.prefab_despawn.has_value() ? facts.prefab_despawn->payload.find("due") : nullptr;
    const base::Json* requested = facts.prefab_despawn.has_value()
                                      ? facts.prefab_despawn->payload.find("requested")
                                      : nullptr;
    const base::Json* despawn_entity =
        facts.prefab_despawn.has_value() ? facts.prefab_despawn->payload.find("entity") : nullptr;
    const bool despawn_scheduled_due_241 =
        facts.prefab_despawn.has_value() && facts.prefab_despawn->tick == kReapTick &&
        due != nullptr && due->is_int() && due->as_int() == kReapTick && requested != nullptr &&
        requested->is_int() && requested->as_int() == kContactTick && despawn_entity != nullptr &&
        despawn_entity->is_int() &&
        despawn_entity->as_int() == static_cast<std::int64_t>(probe_.to_bits());

    // Phase-8 order at the due tick: the despawn record causes the Dead
    // exit chain, then the base seat's despawn_exit, then (post-flush)
    // entity.despawned — ascending journal ids, one cause.
    const base::Json* despawned_body =
        facts.despawned.has_value() ? facts.despawned->payload.find("payload") : nullptr;
    const base::Json* despawned_ref = despawned_body != nullptr && despawned_body->is_object()
                                          ? despawned_body->find("entity")
                                          : nullptr;
    const bool despawn_exit_order =
        cites(facts.exit_dead_hook, facts.prefab_despawn) &&
        cites(facts.despawn_exit, facts.prefab_despawn) &&
        cites(facts.despawned, facts.prefab_despawn) &&
        id_of(facts.prefab_despawn) < id_of(facts.exit_dead_hook) &&
        id_of(facts.exit_dead_hook) < id_of(facts.despawn_exit) &&
        id_of(facts.despawn_exit) < id_of(facts.despawned) && despawned_ref != nullptr &&
        despawned_ref->is_int() &&
        despawned_ref->as_int() == static_cast<std::int64_t>(probe_.to_bits());

    // Exact-tick reap: alive through phase 5 of tick 241, despawned in its
    // phase 8, dead after the run.
    const bool reaped_at_exactly_241 = corpse_alive_due_phase5_ && facts.despawned.has_value() &&
                                       facts.despawned->tick == kReapTick &&
                                       probes.ticks == kReapTick && !world_->alive(probe_);

    struct Named {
        const char* name;
        bool passed;
    };

    const Named named[] = {
        {"initial_entry_seated_order", initial_entry_seated_order},
        {"exit_chain_seven_lines", exit_chain_seven_lines},
        {"contact_payload_bytes_pinned", contact_payload_bytes_pinned},
        {"canonical_payloads_verified", canonical_payloads_verified},
        {"typed_hydration_verified", typed_hydration_verified},
        {"signed_zero_normalized", signed_zero_normalized},
        {"base_transform_mirror_read", base_transform_mirror_read},
        {"kill_cause_chain", kill_cause_chain},
        {"boss_died_at_enter_once", boss_died_at_enter_once},
        {"despawn_scheduled_due_241", despawn_scheduled_due_241},
        {"despawn_exit_order", despawn_exit_order},
        {"reaped_at_exactly_241", reaped_at_exactly_241},
    };

    for (const Named& entry : named) {
        verdict.assertions.set(entry.name, entry.passed);
        if (!entry.passed)
            verdict.failed.emplace_back(entry.name);
    }
    // Diagnostics: raw counts so a red lane names the dead axis directly.
    verdict.assertions.set("enveloped_triggers",
                           static_cast<std::int64_t>(facts.enveloped_triggers));
    verdict.assertions.set("envelope_verified", static_cast<std::int64_t>(facts.envelope_verified));
    verdict.assertions.set("boss_died_count", static_cast<std::int64_t>(facts.boss_died_count));
    return verdict;
}

} // namespace

std::unique_ptr<RunAssertPack> make_component_event_lifecycle_pack() {
    return std::make_unique<ComponentEventLifecyclePack>();
}

} // namespace midday::cli

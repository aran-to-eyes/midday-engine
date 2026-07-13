// cli/verbs/run_assert.cpp — the "appendix_a_golden" pack: MIDDAY_ENGINE_SPEC
// Appendix A.3, executed end to end from the authored corpus and asserted
// against the NORMATIVE TEXT (expected values derived from A.3 before the
// sim ever ran — the sim conforms to the text, never the reverse).
//
// The trace timeline, tick-locked from the text (60 Hz, D-BUILD-057 math):
//   * player.spotted  -> locomotion Idle -> Chasing (stays Chasing forever).
//   * player.inRange injected during tick E-1 delivers at the tick-E input
//     phase: combat Passive -> SlashAttack at E = 3164, so at tick 3200 the
//     playhead sits at 36 ticks = 0.6 s — exactly A.3's setup line.
//   * attack.swoosh at E+18 = 3182 (t = 0.30 s); span HitboxLive opens at
//     E+24 = 3188 (0.40 s) and would close at E+48 = 3212 — so it is OPEN
//     at 3200 and must close inside the death exit chain instead.
//   * tick 3200, phase 5 (update): the damage-system stand-in (the pack's
//     driver — Health + player content land at m1, D-BUILD-081/083) emits
//     damage.dealt, then death.dealt (any-state -> Dead wins, prio 100),
//     then stagger.hit (voided: region already transitioned this tick).
//   * dead.ts onEnter broadcasts boss.died @ global — the depth-2 cascade;
//     the pack IS the listening "UI/score" stand-in and probes the Hurtbox
//     dormant at that exact moment (the no-zombie-hitbox rule, A.2.1).

#include "cli/verbs/run_assert.h"

#include "cli/verbs/run_assert_walk.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/reader.h"
#include "core/journal/writer.h"
#include "core/statechart/statechart.h"
#include "core/tick/tick_loop.h"

#include <optional>
#include <string>
#include <utility>

namespace midday::cli {
namespace {

using assertwalk::is_event_trigger;
using assertwalk::payload_is;
using journal::Record;

// ---- the A.3 timeline, derived from the normative text -----------------------
constexpr std::uint64_t kHitTick = 3200;                 // A.3: "Tick 3200."
constexpr std::uint64_t kEnterTick = 3164;               // E: playhead 0.6 s at 3200
constexpr std::uint64_t kSwooshTick = kEnterTick + 18;   // 0.30 s
constexpr std::uint64_t kSpanOpenTick = kEnterTick + 24; // 0.40 s
constexpr std::uint64_t kSpotTick = 9;                   // approach: Chasing from tick 10

// The driver's synthetic player inputs (inject during tick T -> the tick
// T+1 input phase, A.1 phase 2). Payloads carry the null entity_ref: the
// player entity itself is m1 content (D-BUILD-083).
struct InjectRow {
    std::uint64_t tick;
    const char* event;
    double distance;
};

constexpr InjectRow kApproach[] = {
    {kSpotTick, "player.spotted", 12.0},
    {kEnterTick - 1, "player.inRange", 1.5},
};

class AppendixAGoldenPack final : public RunAssertPack,
                                  public tick::PhaseHook,
                                  public bus::EventListener {
public:
    AppendixAGoldenPack() = default;
    AppendixAGoldenPack(const AppendixAGoldenPack&) = delete;
    AppendixAGoldenPack& operator=(const AppendixAGoldenPack&) = delete;
    AppendixAGoldenPack(AppendixAGoldenPack&&) = delete;
    AppendixAGoldenPack& operator=(AppendixAGoldenPack&&) = delete;

    ~AppendixAGoldenPack() override {
        if (loop_ != nullptr)
            (void)loop_->remove_hook(tick::Phase::kUpdate, *this);
        if (bus_ != nullptr)
            (void)bus_->unsubscribe(*this, bus::EventKey::named(base::Name("global")));
    }

    [[nodiscard]] std::string_view name() const override { return "appendix_a_golden"; }

    std::optional<Error> attach(ecs::World& world,
                                hierarchy::Hierarchy& hierarchy,
                                bus::Bus& bus,
                                tick::TickLoop& loop,
                                journal::Writer& writer,
                                const reflect::Registry& /*registry*/) override {
        world_ = &world;
        hierarchy_ = &hierarchy;
        bus_ = &bus;
        loop_ = &loop;
        writer_ = &writer;
        return assertwalk::attach_update_driver(loop, bus, *this);
    }

    std::optional<Error> bind(statechart::Statechart& chart,
                              const loader::SpawnResult& spawned,
                              std::uint64_t cause_id) override {
        chart_ = &chart;
        const BoundActors actors =
            locate_actors(*world_, spawned, base::Name("Boss"), base::Name("Hurtbox"));
        machine_ = actors.machine;
        boss_ = actors.host;
        hurtbox_ = actors.marker;
        if (machine_ == statechart::kInvalidMachine || hurtbox_.is_null()) {
            Error error{.code = "run.assert_scene",
                        .message = "appendix_a_golden expects the examples/appendix_a corpus: "
                                   "entity 'Boss' with the boss machine and a 'Hurtbox' child"};
            return error;
        }
        return assertwalk::journal_case_presence(*writer_, name(), cause_id);
    }

    // The driver: A.1 phase 5, registered before the Statechart's hook —
    // a pre-update system, exactly where A.3 places the damage system.
    void on_phase(tick::TickLoop& loop, const tick::PhaseContext& context) override {
        for (const InjectRow& row : kApproach)
            if (context.tick == row.tick) {
                Json payload = Json::object();
                payload.set("player", static_cast<std::int64_t>(ecs::EntityRef{}.to_bits()));
                payload.set("distance", row.distance);
                (void)loop.inject_input(
                    bus::EventKey::entity(boss_), base::Name(row.event), std::move(payload));
            }
        if (context.tick != kHitTick)
            return;

        // A.3 setup line: "Currently SlashAttack, playhead 0.6 s — span
        // open, Hurtbox live" — probed live, right before the hit.
        hurtbox_live_before_hit_ =
            chart_->in_state(machine_, base::Name("combat"), base::Name("SlashAttack")) &&
            chart_->in_state(machine_, base::Name("combat"), base::Name("HitboxLive")) &&
            !hierarchy_->is_dormant(hurtbox_);

        // "boss.get(Health).damage(40) -> Health.value hits 0": the damage
        // event is the causal head standing in for contact -> Health (m1).
        const bus::EventKey boss_key = bus::EventKey::entity(boss_);
        Json damage = Json::object();
        damage.set("amount", 40.0);
        damage.set("by", static_cast<std::int64_t>(ecs::EntityRef{}.to_bits()));
        const bus::TriggerResult dealt =
            bus_->trigger(boss_key, base::Name("damage.dealt"), damage, context.phase_record_id);

        Json death = Json::object();
        death.set("by", static_cast<std::int64_t>(ecs::EntityRef{}.to_bits()));
        (void)bus_->trigger(boss_key, base::Name("death.dealt"), death, dealt.record_id);

        // "damage system continues: emits stagger.hit" — same causal head.
        Json stagger = Json::object();
        stagger.set("force", 1.0);
        (void)bus_->trigger(boss_key, base::Name("stagger.hit"), stagger, dealt.record_id);
    }

    // The A.3 "UI/score listeners react" stand-in: boss.died arrives mid-
    // cascade, inside Dead's onEnter — THE moment the no-zombie-hitbox rule
    // is observable live.
    void on_event(bus::Bus& /*bus*/, const bus::EventView& event) override {
        if (event.event != base::Name("boss.died"))
            return;
        saw_boss_died_ = true;
        hurtbox_dormant_at_boss_died_ = !hurtbox_.is_null() && hierarchy_->is_dormant(hurtbox_);
    }

    Verdict evaluate(statechart::Statechart& chart,
                     const std::string& bundle,
                     const RunProbes& probes) override;

private:
    struct Facts; // journal-walk collector (below)

    ecs::World* world_ = nullptr;
    hierarchy::Hierarchy* hierarchy_ = nullptr;
    bus::Bus* bus_ = nullptr;
    tick::TickLoop* loop_ = nullptr;
    journal::Writer* writer_ = nullptr;
    statechart::Statechart* chart_ = nullptr;
    statechart::MachineId machine_ = statechart::kInvalidMachine;
    ecs::EntityRef boss_;
    ecs::EntityRef hurtbox_;
    // Live probes (evaluate() reconciles them with the journal).
    bool hurtbox_live_before_hit_ = false;
    bool saw_boss_died_ = false;
    bool hurtbox_dormant_at_boss_died_ = false;
};

// One streaming pass over the bundle, keeping exactly the records the A.3
// assertions cite.
struct AppendixAGoldenPack::Facts {
    int combat_transitions_at_hit = 0;
    int locomotion_transitions = 0;
    std::optional<Record> chasing_transition;   // locomotion Idle -> Chasing
    std::optional<Record> slash_transition;     // combat Passive -> SlashAttack @ E
    std::optional<Record> hitbox_transition;    // combat -> HitboxLive @ E+24
    std::optional<Record> hit_transition;       // combat @ 3200 (the death rule)
    std::optional<Record> voided_stagger;       // @3200, reason region_already_transitioned
    std::optional<Record> voided_hitbox_closed; // @3200, the mid-exit span echo
    std::optional<Record> hook_exit_slash;      // @3200 exit SlashAttack
    std::optional<Record> hook_enter_dead;      // @3200 enter Dead
    std::optional<Record> span_open;            // @ E+24
    std::optional<Record> span_close;           // @3200, via "exit"
    std::optional<Record> swoosh;               // @ E+18
    std::optional<Record> in_range;             // player.inRange delivery @ E
    std::optional<Record> damage;               // @3200 (the causal head)
    std::optional<Record> death;                // @3200
    std::optional<Record> stagger;              // @3200
    std::optional<Record> boss_died;            // @3200 on "global"

    void collect(const Record& record) {
        if (record.kind == "statechart.transition") {
            if (payload_is(record, "region", "combat")) {
                if (record.tick == kHitTick) {
                    ++combat_transitions_at_hit;
                    hit_transition = record;
                } else if (payload_is(record, "to", "SlashAttack")) {
                    slash_transition = record;
                } else if (payload_is(record, "to", "HitboxLive")) {
                    hitbox_transition = record;
                }
            } else if (payload_is(record, "region", "locomotion")) {
                ++locomotion_transitions;
                if (!chasing_transition.has_value())
                    chasing_transition = record;
            }
            return;
        }
        if (record.kind == "statechart.voided" && record.tick == kHitTick) {
            if (payload_is(record, "event", "stagger.hit"))
                voided_stagger = record;
            else if (payload_is(record, "event", "HitboxLive.closed"))
                voided_hitbox_closed = record;
            return;
        }
        if (record.kind == "statechart.hook" && record.tick == kHitTick) {
            if (payload_is(record, "hook", "exit") && payload_is(record, "state", "SlashAttack"))
                hook_exit_slash = record;
            else if (payload_is(record, "hook", "enter") && payload_is(record, "state", "Dead"))
                hook_enter_dead = record;
            return;
        }
        if (record.kind == "sequence.span_open" && payload_is(record, "span", "HitboxLive")) {
            span_open = record;
            return;
        }
        if (record.kind == "sequence.span_close" && record.tick == kHitTick) {
            span_close = record;
            return;
        }
        if (record.kind != "event.trigger")
            return;
        if (is_event_trigger(record, "attack.swoosh"))
            swoosh = record;
        else if (is_event_trigger(record, "player.inRange"))
            in_range = record;
        else if (is_event_trigger(record, "damage.dealt") && record.tick == kHitTick)
            damage = record;
        else if (is_event_trigger(record, "death.dealt") && record.tick == kHitTick)
            death = record;
        else if (is_event_trigger(record, "stagger.hit") && record.tick == kHitTick)
            stagger = record;
        else if (is_event_trigger(record, "boss.died") && record.tick == kHitTick)
            boss_died = record;
    }
};

RunAssertPack::Verdict AppendixAGoldenPack::evaluate(statechart::Statechart& chart,
                                                     const std::string& bundle,
                                                     const RunProbes& /*probes*/) {
    Verdict verdict;
    Facts facts;
    if (auto error = assertwalk::walk_bundle(bundle, facts)) {
        verdict.error = std::move(error);
        return verdict;
    }

    const auto& id_of = assertwalk::record_id_of; // shared probes (walk header)
    const auto cites = &assertwalk::record_cites;
    const auto cause_of = [](const std::optional<Record>& record) {
        return record.has_value() ? record->cause_id : std::uint64_t{0};
    };

    // ---- the five item-21 verdicts (names are the exit-test contract) -------
    const bool one_combat_transition = facts.combat_transitions_at_hit == 1;

    const bool hurtbox_inactive_before_dead_enter =
        saw_boss_died_ && hurtbox_dormant_at_boss_died_ && facts.span_close.has_value() &&
        facts.hook_enter_dead.has_value() && id_of(facts.span_close) < id_of(facts.hook_enter_dead);

    const bool voided_stagger =
        facts.voided_stagger.has_value() &&
        payload_is(*facts.voided_stagger, "target", "Staggered") &&
        payload_is(*facts.voided_stagger, "reason", "region_already_transitioned") &&
        cites(facts.voided_stagger, facts.stagger) && cites(facts.stagger, facts.damage);

    const bool locomotion_still_chasing =
        facts.locomotion_transitions == 1 && facts.chasing_transition.has_value() &&
        facts.chasing_transition->tick == kSpotTick + 1 &&
        payload_is(*facts.chasing_transition, "from", "Idle") &&
        payload_is(*facts.chasing_transition, "to", "Chasing") &&
        chart.active_state(machine_, base::Name("locomotion")) == base::Name("Chasing");

    // A.3: "cause chain reads contact -> damage -> death.dealt -> transition
    // -> boss.died end to end" (contact -> damage collapses into the
    // damage.dealt head until m1 lands Health + player content).
    const bool cause_chain_cited = cites(facts.boss_died, facts.hook_enter_dead) &&
                                   cites(facts.hook_enter_dead, facts.hit_transition) &&
                                   cites(facts.hit_transition, facts.death) &&
                                   cites(facts.death, facts.damage);
    const bool cause_chain_shape = facts.damage.has_value() && facts.damage->tick == kHitTick &&
                                   facts.hit_transition.has_value() &&
                                   payload_is(*facts.hit_transition, "from", "SlashAttack") &&
                                   payload_is(*facts.hit_transition, "to", "Dead") &&
                                   payload_is(*facts.hit_transition, "via", "any-state");
    const bool cause_chain_complete = cause_chain_cited && cause_chain_shape;

    // ---- the rest of the A.3 text, each line its own named verdict ----------
    const bool slash_entered_at_e = facts.slash_transition.has_value() &&
                                    facts.slash_transition->tick == kEnterTick &&
                                    payload_is(*facts.slash_transition, "from", "Passive") &&
                                    cites(facts.slash_transition, facts.in_range);
    const bool swoosh_at_e18 = facts.swoosh.has_value() && facts.swoosh->tick == kSwooshTick;
    const bool span_open_at_e24 =
        facts.span_open.has_value() && facts.span_open->tick == kSpanOpenTick &&
        facts.hitbox_transition.has_value() && facts.hitbox_transition->tick == kSpanOpenTick;
    const bool span_closed_by_exit_chain = facts.span_close.has_value() &&
                                           payload_is(*facts.span_close, "via", "exit") &&
                                           cites(facts.span_close, facts.hit_transition);
    const bool voided_hitbox_closed =
        facts.voided_hitbox_closed.has_value() &&
        payload_is(*facts.voided_hitbox_closed, "reason", "region_already_transitioned");
    // A.2.1 exit order in journal-id arithmetic: script onExit(Dead) FIRST
    // (brain runs while its parts are live), then the open span closes
    // inside the exit chain, then the mid-exit HitboxLive.closed echo voids
    // on the marked region, and only then does Dead enter. (Substate exits
    // journal no statechart.hook record here: hook records are INVOCATIONS,
    // and HitboxLive seats no script — the m0-statechart-core policy; the
    // subtree-deactivation half of step 2 is the live dormancy probe in
    // hurtbox_inactive_before_dead_enter.)
    const bool exit_chain_order = facts.hook_exit_slash.has_value() &&
                                  payload_is(*facts.hook_exit_slash, "peer", "Dead") &&
                                  cites(facts.hook_exit_slash, facts.hit_transition) &&
                                  id_of(facts.hook_exit_slash) < id_of(facts.span_close) &&
                                  id_of(facts.span_close) < id_of(facts.voided_hitbox_closed) &&
                                  id_of(facts.voided_hitbox_closed) < id_of(facts.hook_enter_dead);
    const bool boss_died_broadcast = facts.boss_died.has_value() &&
                                     payload_is(*facts.boss_died, "key", "global") &&
                                     cause_of(facts.boss_died) != 0;
    const bool dead_at_end = chart.in_state(machine_, base::Name("combat"), base::Name("Dead"));

    struct Named {
        const char* name;
        bool passed;
    };

    const Named named[] = {
        {"hurtbox_inactive_before_dead_enter", hurtbox_inactive_before_dead_enter},
        {"voided_stagger", voided_stagger},
        {"locomotion_still_chasing", locomotion_still_chasing},
        {"cause_chain_complete", cause_chain_complete},
        {"slash_entered_at_e", slash_entered_at_e},
        {"swoosh_at_e18", swoosh_at_e18},
        {"span_open_at_e24", span_open_at_e24},
        {"hurtbox_live_before_hit", hurtbox_live_before_hit_},
        {"span_closed_by_exit_chain", span_closed_by_exit_chain},
        {"voided_hitboxlive_closed", voided_hitbox_closed},
        {"exit_chain_order_a21", exit_chain_order},
        {"boss_died_broadcast", boss_died_broadcast},
        {"dead_state_active_at_end", dead_at_end},
    };

    verdict.assertions.set("combat_transitions_at_3200",
                           static_cast<std::int64_t>(facts.combat_transitions_at_hit));
    if (!one_combat_transition)
        verdict.failed.emplace_back("combat_transitions_at_3200");
    for (const Named& entry : named) {
        verdict.assertions.set(entry.name, entry.passed);
        if (!entry.passed)
            verdict.failed.emplace_back(entry.name);
    }
    return verdict;
}

} // namespace

BoundActors locate_actors(ecs::World& world,
                          const loader::SpawnResult& spawned,
                          base::Name machine_entity,
                          base::Name marker_entity) {
    BoundActors actors;
    for (const loader::MachineSeat& seat : spawned.machines)
        if (seat.entity == machine_entity) {
            actors.machine = seat.id;
            actors.host = seat.host;
        }
    world.view<loader::SceneEntity>().include_inactive().each(
        [&](ecs::EntityRef ref, loader::SceneEntity& tag) {
            if (tag.name == marker_entity)
                actors.marker = ref;
        });
    return actors;
}

std::unique_ptr<RunAssertPack> make_assert_pack(std::string_view name) {
    if (name == "appendix_a_golden")
        return std::make_unique<AppendixAGoldenPack>();
    if (name == "determinism_kata")
        return make_determinism_kata_pack();
    if (name == "component_event_lifecycle")
        return make_component_event_lifecycle_pack();
    return nullptr;
}

std::string assert_pack_names() {
    return "appendix_a_golden, determinism_kata, component_event_lifecycle";
}

} // namespace midday::cli

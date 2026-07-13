// core/bus/bus.h — the keyed, journaled, immediate event bus: the game's
// nervous system and its ONLY transition mechanism (spec section 4.2).
//
// Mechanics:
//   * KEYS SCOPE PRIVACY. A channel is an EventKey — an entity handle (the
//     entity-private channel) or an interned name (group / well-known
//     broadcast). Holding the key IS the capability: you hear and speak only
//     on channels whose key you hold. Subscription is per KEY; every event
//     name triggered at that key reaches every subscriber on it (listeners
//     match names themselves — the statechart's Transition tables do exactly
//     that). Same event name, different key: never delivered across.
//   * IMMEDIATE DISPATCH. trigger() runs every subscriber on the SAME call
//     stack, in REGISTRATION ORDER (the only dispatch order; the channel
//     lookup map is lookup-only and never iterated — D-BUILD-023 precedent).
//     Listeners may trigger further events (cascades run nested, Appendix
//     A.2.5) up to kMaxCascadeDepth levels; the offending deeper trigger
//     gets a structured "bus.cascade_depth" Error and dispatches nothing,
//     while every enclosing level completes normally — no unwinding.
//   * EVERYTHING JOURNALS. Every accepted trigger writes ONE FLIGHT record
//     {kind:"event.trigger", payload:{event,key,...,subscribers},
//     cause_id: caller's cause} BEFORE dispatch; the consumed record id is
//     handed to every listener (EventView::record_id) as THE cause id for
//     their effects — causality chains reconstruct mechanically. If the
//     journal refuses the record, the trigger refuses too
//     ("bus.journal_refused"): unjournaled effects do not exist. Refused
//     triggers journal their refusal ("bus.cascade_depth",
//     "bus.payload_invalid") so the journal explains itself.
//   * TYPED PAYLOADS ARE CANONICAL BYTES (M2 0B, D5). Events registered in
//     the reflect vocabulary validate strictly against their payload schema
//     (missing/unknown/mistyped fields refuse "bus.payload_invalid"), then
//     encode through core/reflect/payload_codec.h AT TRIGGER: the record
//     gains payload_codec ("midday.payload/1") + payload_schema (the
//     compat hash, hex64) + payload_bytes (lowercase hex — AUTHORITATIVE)
//     + payload (the decoded normalized projection, produced by DECODING
//     the bytes, never built independently). Values the fixed layout cannot
//     canonically spell — NaN/±inf floats, invalid UTF-8 — refuse
//     "bus.payload_invalid" WITH the exact field path. Dispatch delivers
//     the PROJECTION (schema field order, -0 normalized to +0), so
//     listeners, journal, and replay all read one truth. Unregistered event
//     names pass through verbatim, envelope-free — custom key-scoped
//     vocabularies are legal (D-BUILD-046); their canonical encoding is a
//     later node's decision (payload_codec.h refuses schema-less encodes).
//   * STRUCTURE DEFERS DURING DISPATCH. subscribe/unsubscribe mid-dispatch
//     queue and apply at the END of the outermost dispatch, in queue order
//     (the ECS structural-queue rule): new subscribers never see the cascade
//     that created them, unsubscribed listeners keep receiving until the
//     cascade ends. Duplicate subscribe: structured error immediately,
//     counted drop when deferred; absent unsubscribe: counted no-op
//     (D-BUILD-047).
//
// Cost model (dispatch is a hot path):
//   * Per trigger: one channel-map lookup (integer-mix hash of the key, map
//     never iterated) + one registry event lookup (precomputed Name id) +
//     the journal record + the dispatch loop. Payload allocation class:
//     vocabulary events pay one canonical encode + decode (bytes + the
//     projection both land in the record — inherent in "the bytes are the
//     journal truth", D5); unregistered events pay one deep copy of the
//     caller's payload into the record, as before.
//   * Per delivery: plain listener = one virtual call; entity listener =
//     one generation check (two vector loads) + two paged sparse-set finds
//     inside the thunk (activity + row; core/bus/entity_listener.h); the
//     entity-bound LISTENER flavor (M2 0B) = the generation check + one
//     virtual call (no pool row exists to find).
//   * The dispatch machinery itself — lookup, iteration, depth tracking,
//     dead-marking — allocates nothing and hashes nothing further. No
//     exceptions anywhere.
//
// Determinism: dispatch order, deferred-op application, auto-unsubscribe,
// and every journal byte are pure functions of the (subscribe/trigger)
// operation sequence — pinned by the bus.determinism dual-run test.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/ecs/entity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace midday::ecs {
class World;
}

namespace midday::journal {
class Writer;
}

namespace midday::reflect {
class Registry;
}

namespace midday::bus {

class Bus;

// A channel key: WHO may hear. Channel identity is (kind, bits) — an entity
// key and a name key never alias, whatever their bit patterns.
class EventKey {
public:
    enum class Kind : std::uint8_t {
        kNamed = 0,  // group / well-known broadcast channel
        kEntity = 1, // entity-private channel (spec: "entity UUID as key")
    };

    // The empty named key — a null capability; subscribe/trigger refuse it.
    EventKey() = default;

    static EventKey named(base::Name name) {
        EventKey key;
        key.kind_ = Kind::kNamed;
        key.name_ = name;
        return key;
    }

    static EventKey entity(ecs::EntityRef ref) {
        EventKey key;
        key.kind_ = Kind::kEntity;
        key.entity_ = ref;
        return key;
    }

    [[nodiscard]] Kind kind() const { return kind_; }

    [[nodiscard]] bool is_null() const { return kind_ == Kind::kNamed && name_.empty(); }

    [[nodiscard]] base::Name name() const { return name_; } // empty unless kNamed

    [[nodiscard]] ecs::EntityRef entity_ref() const { return entity_; } // null unless kEntity

    // Channel identity bits: name id / EntityRef::to_bits().
    [[nodiscard]] std::uint64_t bits() const {
        return kind_ == Kind::kNamed ? name_.id() : entity_.to_bits();
    }

    // Diagnostic journal spelling: a named key is its text verbatim, an
    // entity key is "entity:<index>#<generation>". Channel identity never
    // depends on this string (a name may legally look like the latter).
    [[nodiscard]] std::string journal_form() const;

    friend bool operator==(const EventKey& a, const EventKey& b) {
        return a.kind_ == b.kind_ && a.bits() == b.bits();
    }

private:
    Kind kind_ = Kind::kNamed;
    base::Name name_;
    ecs::EntityRef entity_;
};

// One delivered event, as every listener sees it.
struct EventView {
    base::Name event;          // the triggered event name
    EventKey key;              // the channel it arrived on
    const base::Json& payload; // vocabulary events: the decoded normalized
                               // projection of the canonical bytes (D5);
                               // unregistered events: the trigger's payload
                               // verbatim. Never mutated mid-dispatch.
    std::uint64_t record_id;   // journal id of the trigger — THE cause id for effects
    std::uint64_t tick;        // sim tick of the trigger
};

// The subscriber interface (spec 4.2 EventListener). The bus stores the
// pointer and never owns it: a plain listener must have a stable address
// and must unsubscribe before it dies (component-resident listeners use
// core/bus/entity_listener.h instead — pool rows MOVE; see D-BUILD-048).
class EventListener {
public:
    EventListener() = default;
    EventListener(const EventListener&) = default;
    EventListener& operator=(const EventListener&) = default;
    EventListener(EventListener&&) = default;
    EventListener& operator=(EventListener&&) = default;

    virtual void on_event(Bus& bus, const EventView& event) = 0;

protected:
    ~EventListener() = default; // the bus never deletes through this interface
};

// Entity-bound delivery thunk (generated by subscribe_component<T>): fetches
// the live component through the World and delivers, or reports "skipped"
// (missing/inactive component). Never caches pool pointers.
using EntityThunk = bool (*)(ecs::World&, ecs::EntityRef, Bus&, const EventView&);

struct TriggerResult {
    std::uint64_t record_id = 0;      // journal id of the trigger record; 0 when refused
    std::uint32_t delivered = 0;      // listeners actually run (skips excluded)
    std::optional<base::Error> error; // bus.null_key | bus.cascade_depth |
                                      // bus.payload_invalid | bus.journal_refused
};

struct BusStats {
    std::uint64_t triggers = 0;           // accepted triggers (journaled event.trigger)
    std::uint64_t deliveries = 0;         // listener invocations
    std::uint64_t skipped_inactive = 0;   // entity flavor: component missing or inactive
    std::uint64_t skipped_pending = 0;    // entity flavor: entity still queue-pending
    std::uint64_t auto_unsubscribed = 0;  // stale entity subscriptions dropped at dispatch
    std::uint64_t noop_unsubscribes = 0;  // unsubscribe of an absent subscription
    std::uint64_t dropped_subscribes = 0; // deferred subscribe found duplicate at apply
};

class Bus {
public:
    // Cascade depth cap (spec 4.2 / Appendix A.2.5): a trigger at nesting
    // level kMaxCascadeDepth + 1 is the structured error "bus.cascade_depth".
    static constexpr std::uint32_t kMaxCascadeDepth = 32;

    // All three collaborators must outlive the bus (and must not be moved
    // from under it). The canonical composition wires the engine's World,
    // its boot Registry, and THE flight recorder.
    Bus(ecs::World& world, const reflect::Registry& registry, journal::Writer& journal);

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    Bus(Bus&&) = delete;
    Bus& operator=(Bus&&) = delete;
    ~Bus() = default;

    // The sim tick stamped on every journal record — driven by the tick
    // loop (m0-tick-loop); D-BUILD-013 discipline: never wall clock.
    void set_tick(std::uint64_t tick) { tick_ = tick; }

    [[nodiscard]] std::uint64_t tick() const { return tick_; }

    // ---- subscription (deferred to dispatch end while dispatching) --------
    std::optional<base::Error> subscribe(EventListener& listener, EventKey key);
    std::optional<base::Error> unsubscribe(EventListener& listener, EventKey key);

    // Entity-bound flavor (normally via subscribe_component<T>): identity is
    // (key, entity, thunk), auto-unsubscribed when the entity's generation
    // goes stale. Pending entities stay subscribed and start hearing events
    // once the structural flush makes them alive.
    std::optional<base::Error>
    subscribe_entity(EventKey key, ecs::EntityRef entity, EntityThunk thunk);
    std::optional<base::Error>
    unsubscribe_entity(EventKey key, ecs::EntityRef entity, EntityThunk thunk);

    // Entity-bound LISTENER flavor (M2 0B, #12b): a stable listener pointer
    // whose delivery is generation-gated on `entity` — pending skips and
    // stays, stale auto-unsubscribes WITHOUT touching the listener, exactly
    // the thunk flavor's lifecycle. For runtime seats with no C++ component
    // type (TS-authored components live as QuickJS objects, never in a typed
    // ecs::Pool<T>): a pool thunk cannot exist for them and a plain
    // subscription would dangle after reap. Identity is (key, entity,
    // listener); the listener must outlive the subscription or the entity.
    std::optional<base::Error>
    subscribe_entity_listener(EventKey key, ecs::EntityRef entity, EventListener& listener);
    std::optional<base::Error>
    unsubscribe_entity_listener(EventKey key, ecs::EntityRef entity, EventListener& listener);

    // ---- the nervous impulse ----------------------------------------------
    // Journal first, then dispatch immediately (registration order, same
    // call stack). `cause_id` is the journal id of the causing record
    // (0 = external/root); listeners chain their own effects from
    // EventView::record_id. Payload is taken by reference: one copy goes
    // into the journal record, listeners see the caller's object.
    TriggerResult
    trigger(EventKey key, base::Name event, const base::Json& payload, std::uint64_t cause_id);

    // ---- introspection -----------------------------------------------------
    // Live subscriptions on the channel (dead-marked entries excluded).
    [[nodiscard]] std::uint32_t subscriber_count(EventKey key) const;

    [[nodiscard]] bool dispatching() const { return depth_ != 0; }

    [[nodiscard]] std::uint32_t cascade_depth() const { return depth_; }

    [[nodiscard]] const BusStats& stats() const { return stats_; }

private:
    // Three flavors, discriminated by which fields engage: plain (listener,
    // null entity), entity thunk (thunk + entity), entity listener
    // (listener + entity — M2 0B). A null entity ONLY ever means plain.
    struct Subscription {
        EventListener* listener = nullptr; // plain + entity-listener flavors
        EntityThunk thunk = nullptr;       // entity-thunk flavor (listener null)
        ecs::EntityRef entity;             // entity flavors' generation gate
        bool dead = false;                 // deferred removal mark

        [[nodiscard]] bool same_identity(const Subscription& other) const {
            return listener == other.listener && thunk == other.thunk && entity == other.entity;
        }
    };

    struct Channel {
        std::vector<Subscription> entries; // REGISTRATION ORDER — the dispatch order
        std::uint32_t dead_count = 0;      // dead-marked entries awaiting compaction
        bool dirty = false;                // in dirty_channels_ already
    };

    struct ChannelId {
        std::uint64_t bits = 0;
        EventKey::Kind kind = EventKey::Kind::kNamed;

        friend bool operator==(const ChannelId&, const ChannelId&) = default;
    };

    struct ChannelIdHash {
        std::size_t operator()(const ChannelId& id) const {
            // splitmix64 finalizer over domain-salted bits; the map is
            // lookup-only (never iterated), so distribution is all that
            // matters — equality on the full (kind, bits) pair.
            std::uint64_t x =
                id.bits ^ (id.kind == EventKey::Kind::kEntity ? 0x9E3779B97F4A7C15ULL : 0ULL);
            x ^= x >> 30U;
            x *= 0xBF58476D1CE4E5B9ULL;
            x ^= x >> 27U;
            x *= 0x94D049BB133111EBULL;
            x ^= x >> 31U;
            return static_cast<std::size_t>(x);
        }
    };

    struct DeferredOp {
        enum class Kind : std::uint8_t { kSubscribe, kUnsubscribe };

        Kind kind = Kind::kSubscribe;
        EventKey key;
        Subscription entry; // the new entry / the identity to remove
    };

    [[nodiscard]] const Channel* find_channel(EventKey key) const;
    [[nodiscard]] std::uint32_t ensure_channel(EventKey key); // index (may grow channels_)

    std::optional<base::Error> add_subscription(EventKey key, const Subscription& entry);
    std::optional<base::Error> remove_subscription(EventKey key, const Subscription& identity);
    static Subscription* find_live(Channel& channel, const Subscription& identity);
    bool erase_live(EventKey key, const Subscription& identity); // false = was not subscribed
    void apply_deferred();

    // Runs one dispatch level over the channel's registration-order entries;
    // returns the delivery count. channels_ cannot grow mid-dispatch (channel
    // creation happens only at depth 0), so entry references are stable.
    std::uint32_t dispatch(std::uint32_t channel_index, const EventView& view);

    // Journals a refusal record (kind = code, payload = details, best-effort
    // if the writer is poisoned) and builds the matching Error — refusal
    // record payload and error details are ONE shape.
    base::Error refuse(std::string_view code,
                       std::string_view message,
                       base::Json details,
                       std::uint64_t cause_id);

    ecs::World* world_;
    const reflect::Registry* registry_;
    journal::Writer* journal_;
    std::unordered_map<ChannelId, std::uint32_t, ChannelIdHash> channel_index_; // lookup ONLY
    std::vector<Channel> channels_;
    std::vector<DeferredOp> deferred_;
    std::vector<std::uint32_t> dirty_channels_; // first-marked order (deterministic)
    BusStats stats_;
    std::uint64_t tick_ = 0;
    std::uint32_t depth_ = 0;
};

} // namespace midday::bus

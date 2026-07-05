// core/bus/bus.cpp — keyed immediate dispatch with journal-first causality.
// Semantics and cost model: bus.h. Decision trail: D-BUILD-046..049.

#include "core/bus/bus.h"

#include "core/ecs/world.h"
#include "core/journal/writer.h"
#include "core/reflect/registry.h"
#include "core/reflect/type_model.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <utility>

namespace midday::bus {
namespace {

using base::Error;
using base::Json;

// Runtime payload shape per field type. One deviation from the authoring
// model (TypeDesc::accepts): entity_ref fields carry EntityRef::to_bits()
// integers at runtime — the symbolic string forms (self/root/...) are a
// loader concern, resolved at spawn (D-BUILD-046).
bool runtime_accepts(const reflect::TypeDesc& type, const Json& value) {
    switch (type.kind()) {
    case reflect::TypeKind::kEntityRef:
        return value.is_int() && value.as_int() >= 0;
    case reflect::TypeKind::kArray: {
        if (!value.is_array())
            return false;
        for (const Json& item : value.elements())
            if (!runtime_accepts(type.element(), item))
                return false;
        return true;
    }
    case reflect::TypeKind::kMap: {
        if (!value.is_object())
            return false;
        for (const auto& [key, item] : value.items())
            if (!runtime_accepts(type.element(), item))
                return false;
        return true;
    }
    default:
        return type.accepts(value);
    }
}

Json diag_base(base::Name event, const EventKey& key) {
    Json diag = Json::object();
    diag.set("event", event.view());
    diag.set("key", key.journal_form());
    return diag;
}

// Returns the refusal diagnosis (event, key, reason[, field][, expected]) when
// the payload does not inhabit the event's schema; nullopt on the happy path
// (which then allocates no diagnostic at all). Strict both ways: every
// declared field present and typed, no undeclared fields (agents deserve
// refusal over silent tolerance — the D-BUILD-012 ethos).
std::optional<Json> payload_invalid(const reflect::EventDesc& desc,
                                    const Json& payload,
                                    base::Name event,
                                    const EventKey& event_key) {
    if (!payload.is_object()) {
        Json diag = diag_base(event, event_key);
        diag.set("reason", "not_object");
        return diag;
    }
    for (const reflect::EventFieldDesc& field : desc.payload) {
        const Json* value = payload.find(field.name.view());
        if (value == nullptr) {
            Json diag = diag_base(event, event_key);
            diag.set("reason", "missing_field");
            diag.set("field", field.name.view());
            return diag;
        }
        if (!runtime_accepts(field.type, *value)) {
            Json diag = diag_base(event, event_key);
            diag.set("reason", "field_type");
            diag.set("field", field.name.view());
            diag.set("expected", field.type.canonical());
            return diag;
        }
    }
    for (const auto& [key, value] : payload.items()) {
        const bool declared = std::ranges::any_of(
            desc.payload, [&](const auto& field) { return key == field.name.view(); });
        if (!declared) {
            Json diag = diag_base(event, event_key);
            diag.set("reason", "unknown_field");
            diag.set("field", key);
            return diag;
        }
    }
    return std::nullopt;
}

Error null_key_error(std::string_view operation) {
    Json details = Json::object();
    details.set("operation", operation);
    return Error{"bus.null_key",
                 "the empty key is a null capability: nothing hears it, nothing speaks on it",
                 std::move(details)};
}

// nullopt when the entity may be subscribed (alive or pending); the ECS
// refusal (ecs.stale_handle) otherwise.
std::optional<Error> check_subscribable(const ecs::World& world, ecs::EntityRef entity) {
    if (world.alive(entity))
        return std::nullopt;
    std::optional<Error> refusal = world.check_alive(entity);
    if (refusal.has_value() && refusal->code == "ecs.entity_pending")
        return std::nullopt; // wakes at the structural flush
    return refusal;
}

} // namespace

std::string EventKey::journal_form() const {
    if (kind_ == Kind::kNamed)
        return std::string(name_.view());
    return "entity:" + std::to_string(entity_.index) + "#" + std::to_string(entity_.generation);
}

Bus::Bus(ecs::World& world, const reflect::Registry& registry, journal::Writer& journal)
    : world_(&world), registry_(&registry), journal_(&journal) {}

// ---- subscription ----------------------------------------------------------

std::optional<Error> Bus::subscribe(EventListener& listener, EventKey key) {
    Subscription entry;
    entry.listener = &listener;
    return add_subscription(key, entry);
}

std::optional<Error> Bus::unsubscribe(EventListener& listener, EventKey key) {
    Subscription identity;
    identity.listener = &listener;
    return remove_subscription(key, identity);
}

std::optional<Error> Bus::subscribe_entity(EventKey key, ecs::EntityRef entity, EntityThunk thunk) {
    assert(thunk != nullptr);
    if (auto refusal = check_subscribable(*world_, entity))
        return refusal;
    Subscription entry;
    entry.thunk = thunk;
    entry.entity = entity;
    return add_subscription(key, entry);
}

std::optional<Error>
Bus::unsubscribe_entity(EventKey key, ecs::EntityRef entity, EntityThunk thunk) {
    Subscription identity;
    identity.thunk = thunk;
    identity.entity = entity;
    return remove_subscription(key, identity);
}

std::optional<Error> Bus::add_subscription(EventKey key, const Subscription& entry) {
    if (key.is_null())
        return null_key_error("subscribe");
    if (depth_ != 0) { // structure defers during dispatch (D-BUILD-047)
        deferred_.push_back(DeferredOp{DeferredOp::Kind::kSubscribe, key, entry});
        return std::nullopt;
    }
    Channel& channel = channels_[ensure_channel(key)];
    if (find_live(channel, entry) != nullptr) {
        Json details = Json::object();
        details.set("key", key.journal_form());
        return Error{"bus.duplicate_subscription",
                     "this listener is already subscribed on this key",
                     std::move(details)};
    }
    channel.entries.push_back(entry);
    return std::nullopt;
}

std::optional<Error> Bus::remove_subscription(EventKey key, const Subscription& identity) {
    if (key.is_null())
        return null_key_error("unsubscribe");
    if (depth_ != 0) {
        deferred_.push_back(DeferredOp{DeferredOp::Kind::kUnsubscribe, key, identity});
        return std::nullopt;
    }
    if (!erase_live(key, identity))
        ++stats_.noop_unsubscribes; // absent unsubscribe: counted no-op (D-BUILD-047)
    return std::nullopt;
}

Bus::Subscription* Bus::find_live(Channel& channel, const Subscription& identity) {
    for (Subscription& entry : channel.entries)
        if (!entry.dead && entry.same_identity(identity))
            return &entry;
    return nullptr;
}

bool Bus::erase_live(EventKey key, const Subscription& identity) {
    const auto found = channel_index_.find(ChannelId{key.bits(), key.kind()});
    if (found == channel_index_.end())
        return false;
    std::vector<Subscription>& entries = channels_[found->second].entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it->dead && it->same_identity(identity)) {
            entries.erase(it); // depth 0 only: stable order, no dispatch in flight
            return true;
        }
    }
    return false;
}

void Bus::apply_deferred() {
    // Queue order, exactly like the ECS structural flush. Applying ops runs
    // no listener code, so the queue cannot grow while it drains.
    for (const DeferredOp& op : deferred_) {
        if (op.kind == DeferredOp::Kind::kSubscribe) {
            Channel& channel = channels_[ensure_channel(op.key)];
            if (find_live(channel, op.entry) != nullptr)
                ++stats_.dropped_subscribes; // duplicate at apply: counted drop
            else
                channel.entries.push_back(op.entry);
        } else if (!erase_live(op.key, op.entry)) {
            ++stats_.noop_unsubscribes;
        }
    }
    deferred_.clear();
    // Compact dead-marked entries, in first-marked channel order.
    for (const std::uint32_t index : dirty_channels_) {
        Channel& channel = channels_[index];
        std::erase_if(channel.entries, [](const Subscription& entry) { return entry.dead; });
        channel.dead_count = 0;
        channel.dirty = false;
    }
    dirty_channels_.clear();
}

// ---- channels ----------------------------------------------------------------

const Bus::Channel* Bus::find_channel(EventKey key) const {
    const auto it = channel_index_.find(ChannelId{key.bits(), key.kind()});
    return it == channel_index_.end() ? nullptr : &channels_[it->second];
}

std::uint32_t Bus::ensure_channel(EventKey key) {
    assert(depth_ == 0); // creation would invalidate in-flight dispatch references
    const auto [it, inserted] = channel_index_.try_emplace(
        ChannelId{key.bits(), key.kind()}, static_cast<std::uint32_t>(channels_.size()));
    if (inserted)
        channels_.emplace_back();
    return it->second;
}

std::uint32_t Bus::subscriber_count(EventKey key) const {
    const Channel* channel = find_channel(key);
    if (channel == nullptr)
        return 0;
    return static_cast<std::uint32_t>(channel->entries.size()) - channel->dead_count;
}

// ---- the nervous impulse ------------------------------------------------------

TriggerResult
Bus::trigger(EventKey key, base::Name event, const Json& payload, std::uint64_t cause_id) {
    TriggerResult result;
    if (key.is_null()) {
        // Nothing happened, so nothing journals — a null key is a caller bug,
        // not an engine effect.
        result.error = null_key_error("trigger");
        return result;
    }

    // Depth cap FIRST (Appendix A.2.5): the offending call refuses before any
    // side effect; every enclosing dispatch level completes normally.
    if (depth_ >= kMaxCascadeDepth) {
        Json diag = diag_base(event, key);
        diag.set("depth", static_cast<std::int64_t>(depth_) + 1);
        diag.set("cap", static_cast<std::int64_t>(kMaxCascadeDepth));
        result.error = refuse(
            "bus.cascade_depth", "event cascade exceeded the depth cap", std::move(diag), cause_id);
        return result;
    }

    // Typed validation for vocabulary events (unknown names pass through:
    // custom key-scoped vocabularies are legal, D-BUILD-046).
    if (const auto* entry = registry_->find_event(event)) {
        if (auto diag = payload_invalid(entry->desc, payload, event, key)) {
            result.error = refuse("bus.payload_invalid",
                                  "payload does not inhabit the event's schema",
                                  std::move(*diag),
                                  cause_id);
            return result;
        }
    }

    // Journal BEFORE dispatch: the consumed id is the cause of every effect.
    // THE one keyed lookup of the dispatch path (cost model, bus.h).
    const auto found = channel_index_.find(ChannelId{key.bits(), key.kind()});
    const Channel* channel = found == channel_index_.end() ? nullptr : &channels_[found->second];
    const std::uint32_t subscribers =
        channel == nullptr
            ? 0
            : static_cast<std::uint32_t>(channel->entries.size()) - channel->dead_count;
    Json record_payload = diag_base(event, key);
    if (!payload.is_null() && !(payload.is_object() && payload.items().empty()))
        record_payload.set("payload", payload); // the one per-trigger deep copy (cost model)
    record_payload.set("subscribers", subscribers);
    const std::uint64_t record_id = journal_->record(
        tick_, journal::Tier::Flight, "event.trigger", cause_id, std::move(record_payload));
    if (record_id == 0) {
        // No record, no dispatch: unjournaled effects do not exist.
        Json details = diag_base(event, key);
        const std::optional<Error>& sticky = journal_->status();
        details.set("journal",
                    sticky.has_value() ? std::string_view(sticky->code)
                                       : std::string_view("unknown"));
        result.error = Error{"bus.journal_refused",
                             "the flight recorder refused the trigger record",
                             std::move(details)};
        return result;
    }
    result.record_id = record_id;
    ++stats_.triggers;

    if (channel != nullptr && !channel->entries.empty()) {
        const EventView view{event, key, payload, record_id, tick_};
        result.delivered = dispatch(found->second, view);
    }
    return result;
}

std::uint32_t Bus::dispatch(std::uint32_t channel_index, const EventView& view) {
    ++depth_;
    std::uint32_t delivered = 0;
    {
        Channel& channel = channels_[channel_index];
        // Entries appended by deferred subscribes arrive only after the
        // OUTERMOST dispatch ends, so the vector is frozen here: iteration
        // by index over the snapshot count is re-entrancy-safe.
        const std::size_t count = channel.entries.size();
        for (std::size_t i = 0; i < count; ++i) {
            Subscription& entry = channel.entries[i];
            if (entry.dead)
                continue;
            if (entry.thunk == nullptr) {
                entry.listener->on_event(*this, view);
                ++delivered;
                ++stats_.deliveries;
                continue;
            }
            // Entity flavor: generation check BEFORE any component access.
            if (!world_->alive(entry.entity)) {
                std::optional<Error> refusal = world_->check_alive(entry.entity);
                if (refusal.has_value() && refusal->code == "ecs.entity_pending") {
                    ++stats_.skipped_pending; // not yet flushed alive: keep it
                } else {
                    entry.dead = true; // stale: auto-unsubscribe (D-BUILD-048)
                    ++channel.dead_count;
                    if (!channel.dirty) {
                        channel.dirty = true;
                        dirty_channels_.push_back(channel_index);
                    }
                    ++stats_.auto_unsubscribed;
                }
                continue;
            }
            if (entry.thunk(*world_, entry.entity, *this, view)) {
                ++delivered;
                ++stats_.deliveries;
            } else {
                ++stats_.skipped_inactive; // dormant parts hear nothing (spec 4.1)
            }
        }
    }
    --depth_;
    if (depth_ == 0)
        apply_deferred(); // may grow channels_: no channel refs live past here
    return delivered;
}

base::Error
Bus::refuse(std::string_view code, std::string_view message, Json details, std::uint64_t cause_id) {
    // The refusal itself is journaled (the journal explains itself); if the
    // writer is poisoned this is best-effort — the caller still gets the
    // primary structured error.
    journal_->record(tick_, journal::Tier::Flight, code, cause_id, details);
    return Error{std::string(code), std::string(message), std::move(details)};
}

} // namespace midday::bus

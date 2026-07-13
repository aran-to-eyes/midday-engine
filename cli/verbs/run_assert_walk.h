// cli/verbs/run_assert_walk.h — shared journal-walk helpers for assertion
// packs (hoisted from run_assert.cpp at m0-determinism-spike on the
// second-consumer rule): payload probes over journal::Record plus the one
// streaming pass every pack's evaluate() opens with. Collectors keep
// exactly the records/counts their assertions cite; the walk owns the
// reader plumbing and the infrastructure-error shape.

#pragma once

#include "core/base/error.h"
#include "core/base/hex.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/journal/reader.h"
#include "core/journal/writer.h"
#include "core/reflect/payload_codec.h"
#include "core/reflect/registry.h"
#include "core/tick/tick_loop.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::cli::assertwalk {

// Journal a pack's presence from bind(): ONE FLIGHT "assert.case" {case}
// record citing `cause_id` — a driven run says so in its own causality
// stream. Shared by every registered pack.
inline std::optional<base::Error>
journal_case_presence(journal::Writer& writer, std::string_view name, std::uint64_t cause_id) {
    base::Json presence = base::Json::object();
    presence.set("case", std::string(name));
    if (writer.record(0, journal::Tier::Flight, "assert.case", cause_id, std::move(presence)) == 0)
        return writer.status().value_or(
            base::Error{.code = "journal.refused", .message = "assert.case record refused"});
    return std::nullopt;
}

// The driven packs' shared attach tail: a phase-5 (kUpdate) driver hook —
// registered before the Statechart's, so pack systems run first within the
// phase (A.1) — plus the "global" broadcast subscription their live
// listener probes ride. The pack detaches both in its destructor.
template <typename Pack>
inline std::optional<base::Error>
attach_update_driver(tick::TickLoop& loop, bus::Bus& bus, Pack& pack) {
    if (auto error = loop.add_hook(tick::Phase::kUpdate, pack))
        return error;
    return bus.subscribe(pack, bus::EventKey::named(base::Name("global")));
}

// Cause-chain probes over optionally-collected records (evaluate() style):
// id 0 = "not collected"; citation demands both ends exist.
inline std::uint64_t record_id_of(const std::optional<journal::Record>& record) {
    return record.has_value() ? record->id : 0;
}

inline bool record_cites(const std::optional<journal::Record>& effect,
                         const std::optional<journal::Record>& cause) {
    return effect.has_value() && cause.has_value() && effect->cause_id == cause->id;
}

inline const std::string* payload_str(const journal::Record& record, std::string_view key) {
    const base::Json* value = record.payload.find(key);
    return value != nullptr && value->is_string() ? &value->as_string() : nullptr;
}

inline bool
payload_is(const journal::Record& record, std::string_view key, std::string_view expected) {
    const std::string* value = payload_str(record, key);
    return value != nullptr && *value == expected;
}

inline bool is_event_trigger(const journal::Record& record, std::string_view event) {
    return record.kind == "event.trigger" && payload_is(record, "event", event);
}

// D5 (M2 0B) replay-side verification: a vocabulary event.trigger record's
// payload_bytes are the AUTHORITATIVE payload — decode them against the
// event's schema and demand the stored projection is EXACTLY their decode
// (canonical dump bytes; an omitted `payload` key means the empty
// projection). A reader must never trust the projection over the bytes —
// this is that rule, executable. False for a missing/foreign envelope,
// undecodable bytes, or a projection that disagrees with the decode.
inline bool canonical_payload_verified(const reflect::EventDesc& desc,
                                       const journal::Record& record) {
    const std::string* codec = payload_str(record, "payload_codec");
    if (codec == nullptr || *codec != reflect::kPayloadCodecName)
        return false;
    const std::string* schema = payload_str(record, "payload_schema");
    if (schema == nullptr || *schema != base::hex64(desc.compat_hash))
        return false;
    const std::string* hex = payload_str(record, "payload_bytes");
    if (hex == nullptr)
        return false;
    const std::optional<std::vector<std::byte>> bytes = base::hex_to_bytes(*hex);
    if (!bytes.has_value())
        return false;
    reflect::DecodeResult decoded = reflect::decode_payload(&desc, *bytes);
    if (decoded.error.has_value())
        return false;
    const base::Json* projection = record.payload.find("payload");
    const std::string stored = projection != nullptr ? projection->dump() : std::string("{}");
    return stored == decoded.payload.dump();
}

// One streaming pass over the bundle: collector.collect(record) sees every
// record in journal order. Returns the infrastructure error, if any.
template <typename Collector>
std::optional<base::Error> walk_bundle(const std::string& bundle, Collector& collector) {
    journal::ReaderOpenResult opened = journal::Reader::open(bundle);
    if (opened.error.has_value() || !opened.reader.has_value())
        return std::move(opened.error)
            .value_or(base::Error{.code = "journal.io", .message = "cannot open bundle"});
    while (true) {
        journal::Reader::NextResult next = opened.reader->next();
        if (next.error.has_value())
            return std::move(next.error);
        if (!next.record.has_value())
            return std::nullopt;
        collector.collect(*next.record);
    }
}

} // namespace midday::cli::assertwalk

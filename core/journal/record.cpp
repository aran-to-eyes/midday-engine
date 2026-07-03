#include "core/journal/record.h"

#include "core/journal/json_fields.h"

#include <utility>

namespace midday::journal {

namespace {

base::Error corrupt(std::string_view origin, std::string_view reason) {
    base::Error error;
    error.code = "journal.record_corrupt";
    error.message = std::string(origin) + ": " + std::string(reason);
    error.details.set("origin", origin);
    error.details.set("reason", reason);
    return error;
}

} // namespace

std::string_view to_string(Tier tier) {
    switch (tier) {
    case Tier::Flight:
        return "flight";
    case Tier::Snapshot:
        return "snapshot";
    case Tier::Trace:
        return "trace";
    }
    return "flight"; // unreachable; enum is exhaustive
}

std::optional<Tier> tier_from_string(std::string_view text) {
    if (text == "flight")
        return Tier::Flight;
    if (text == "snapshot")
        return Tier::Snapshot;
    if (text == "trace")
        return Tier::Trace;
    return std::nullopt;
}

std::string to_jsonl(const Record& record) {
    base::Json line = base::Json::object();
    line.set("tick", static_cast<std::int64_t>(record.tick));
    line.set("tier", to_string(record.tier));
    line.set("kind", record.kind);
    line.set("cause_id", static_cast<std::int64_t>(record.cause_id));
    line.set("id", static_cast<std::int64_t>(record.id));
    if (!(record.payload.is_object() && record.payload.items().empty()))
        line.set("payload", record.payload);
    return line.dump();
}

RecordParseResult record_from_line(std::string_view line, std::string_view origin) {
    base::Json::ParseResult parsed = base::Json::parse(line, origin);
    if (parsed.error)
        return {std::nullopt, base::to_error(*parsed.error)};
    const base::Json& json = parsed.value;
    if (!json.is_object())
        return {std::nullopt, corrupt(origin, "record line is not a JSON object")};

    static constexpr std::string_view kKeys[] = {
        "tick", "tier", "kind", "cause_id", "id", "payload"};
    if (!detail::only_keys(json, kKeys))
        return {std::nullopt, corrupt(origin, "record has unknown keys")};

    const auto tick = detail::get_u64(json, "tick");
    const auto tier_text = detail::get_string(json, "tier");
    auto kind = detail::get_string(json, "kind");
    const auto cause_id = detail::get_u64(json, "cause_id");
    const auto id = detail::get_u64(json, "id");
    if (!tick || !tier_text || !kind || !cause_id || !id)
        return {std::nullopt, corrupt(origin, "record field missing or mistyped")};

    const auto tier = tier_from_string(*tier_text);
    if (!tier)
        return {std::nullopt, corrupt(origin, "unknown tier \"" + *tier_text + "\"")};
    if (kind->empty())
        return {std::nullopt, corrupt(origin, "record kind is empty")};
    if (*id == 0)
        return {std::nullopt, corrupt(origin, "record id 0 is reserved for external/root")};

    Record record;
    record.tick = *tick;
    record.tier = *tier;
    record.kind = std::move(*kind);
    record.cause_id = *cause_id;
    record.id = *id;
    if (const base::Json* payload = json.find("payload")) {
        if (!payload->is_object())
            return {std::nullopt, corrupt(origin, "record payload is not an object")};
        record.payload = *payload;
    }
    return {std::move(record), std::nullopt};
}

} // namespace midday::journal

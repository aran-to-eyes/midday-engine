#include "core/journal/bundle.h"

#include "core/base/hex.h"
#include "core/journal/json_fields.h"

#define XXH_INLINE_ALL
#include "xxhash.h"
#include "zstd.h"

#include <utility>

namespace midday::journal {

namespace {

base::Error corrupt(std::string_view code, std::string_view reason) {
    base::Error error;
    error.code = std::string(code);
    error.message = std::string(reason);
    error.details.set("reason", reason);
    return error;
}

base::Error header_corrupt(std::string_view reason) {
    return corrupt("journal.header_corrupt", reason);
}

base::Error index_corrupt(std::string_view reason) {
    return corrupt("journal.index_corrupt", reason);
}

} // namespace

base::Json Header::identity_json() const {
    base::Json tiers_json = base::Json::object();
    tiers_json.set("flight", true); // always on, by construction (Zenith D026)
    tiers_json.set("snapshot", tiers.snapshot);
    tiers_json.set("trace", tiers.trace);

    base::Json compression = base::Json::object();
    compression.set("codec", "zstd");
    compression.set("version", ZSTD_versionString());
    compression.set("level", kZstdLevel);
    compression.set("checksum", kZstdChecksum);
    compression.set("single_thread", true);

    base::Json identity = base::Json::object();
    identity.set("engine_version", engine_version);
    identity.set("api_compat_hash", api_compat_hash);
    identity.set("seed", static_cast<std::int64_t>(seed));
    identity.set("tiers", std::move(tiers_json));
    identity.set("compression", std::move(compression));
    identity.set("index_stride_ticks", static_cast<std::int64_t>(index_stride_ticks));
    return identity;
}

std::string Header::replay_identity() const {
    const std::string bytes = identity_json().dump();
    return base::hex64(XXH3_64bits(bytes.data(), bytes.size()));
}

base::Json Header::to_json() const {
    base::Json info = base::Json::object();
    info.set("platform", platform);
    if (!created_at.empty())
        info.set("created_at", created_at); // informational ONLY — never hashed

    base::Json json = base::Json::object();
    json.set("schema", kHeaderSchema);
    json.set("identity", identity_json());
    json.set("replay_identity", replay_identity());
    json.set("info", std::move(info));
    return json;
}

HeaderParseResult Header::from_json(const base::Json& json) {
    if (!json.is_object())
        return {std::nullopt, header_corrupt("header is not a JSON object")};
    static constexpr std::string_view kTopKeys[] = {
        "schema", "identity", "replay_identity", "info"};
    if (!detail::only_keys(json, kTopKeys))
        return {std::nullopt, header_corrupt("header has unknown keys")};

    const auto schema = detail::get_string(json, "schema");
    if (!schema || *schema != kHeaderSchema)
        return {std::nullopt, header_corrupt("unknown header schema")};

    const base::Json* identity = json.find("identity");
    const base::Json* info = json.find("info");
    const auto stored_hash = detail::get_string(json, "replay_identity");
    if (identity == nullptr || !identity->is_object() || info == nullptr || !info->is_object() ||
        !stored_hash)
        return {std::nullopt, header_corrupt("header field missing or mistyped")};

    static constexpr std::string_view kIdentityKeys[] = {
        "engine_version", "api_compat_hash", "seed", "tiers", "compression", "index_stride_ticks"};
    if (!detail::only_keys(*identity, kIdentityKeys))
        return {std::nullopt, header_corrupt("identity has unknown keys")};

    auto engine_version = detail::get_string(*identity, "engine_version");
    auto api_compat_hash = detail::get_string(*identity, "api_compat_hash");
    const auto seed = detail::get_u64(*identity, "seed");
    const auto stride = detail::get_u64(*identity, "index_stride_ticks");
    const base::Json* tiers = identity->find("tiers");
    const base::Json* compression = identity->find("compression");
    if (!engine_version || !api_compat_hash || !seed || !stride || stride == 0U ||
        tiers == nullptr || !tiers->is_object() || compression == nullptr ||
        !compression->is_object())
        return {std::nullopt, header_corrupt("identity field missing or mistyped")};

    const auto flight = detail::get_bool(*tiers, "flight");
    const auto snapshot = detail::get_bool(*tiers, "snapshot");
    const auto trace = detail::get_bool(*tiers, "trace");
    if (!flight || !snapshot || !trace)
        return {std::nullopt, header_corrupt("tier config missing or mistyped")};
    if (!*flight)
        return {std::nullopt, header_corrupt("flight tier cannot be off (always-on contract)")};

    const auto codec = detail::get_string(*compression, "codec");
    if (!codec || *codec != "zstd")
        return {std::nullopt, header_corrupt("unknown compression codec")};

    auto platform = detail::get_string(*info, "platform");
    if (!platform)
        return {std::nullopt, header_corrupt("info.platform missing or mistyped")};

    // Integrity: the stored hash must match XXH3 over the parsed identity
    // bytes (parse -> dump is a fixed point of the core JSON writer).
    const std::string identity_bytes = identity->dump();
    const std::string computed =
        base::hex64(XXH3_64bits(identity_bytes.data(), identity_bytes.size()));
    if (computed != *stored_hash) {
        base::Error error = header_corrupt("replay_identity does not match identity content");
        error.details.set("stored", *stored_hash);
        error.details.set("computed", computed);
        return {std::nullopt, std::move(error)};
    }

    Header header;
    header.engine_version = std::move(*engine_version);
    header.api_compat_hash = std::move(*api_compat_hash);
    header.seed = *seed;
    header.tiers = TierConfig{.snapshot = *snapshot, .trace = *trace};
    header.index_stride_ticks = static_cast<std::uint32_t>(*stride);
    header.platform = std::move(*platform);
    header.created_at = detail::get_string(*info, "created_at").value_or("");
    return {std::move(header), std::nullopt};
}

base::Json Index::to_json() const {
    base::Json entries_json = base::Json::array();
    for (const IndexEntry& entry : entries) {
        base::Json entry_json = base::Json::object();
        entry_json.set("tick", static_cast<std::int64_t>(entry.tick));
        entry_json.set("record_id", static_cast<std::int64_t>(entry.record_id));
        entry_json.set("offset", static_cast<std::int64_t>(entry.offset));
        entry_json.set("frame_offset", static_cast<std::int64_t>(entry.frame_offset));
        entries_json.push(std::move(entry_json));
    }

    base::Json json = base::Json::object();
    json.set("schema", kIndexSchema);
    json.set("stride_ticks", static_cast<std::int64_t>(stride_ticks));
    json.set("records", static_cast<std::int64_t>(records));
    json.set("journal_bytes", static_cast<std::int64_t>(journal_bytes));
    json.set("entries", std::move(entries_json));
    return json;
}

IndexParseResult Index::from_json(const base::Json& json) {
    if (!json.is_object())
        return {std::nullopt, index_corrupt("index is not a JSON object")};
    static constexpr std::string_view kKeys[] = {
        "schema", "stride_ticks", "records", "journal_bytes", "entries"};
    if (!detail::only_keys(json, kKeys))
        return {std::nullopt, index_corrupt("index has unknown keys")};

    const auto schema = detail::get_string(json, "schema");
    if (!schema || *schema != kIndexSchema)
        return {std::nullopt, index_corrupt("unknown index schema")};

    const auto stride = detail::get_u64(json, "stride_ticks");
    const auto records = detail::get_u64(json, "records");
    const auto journal_bytes = detail::get_u64(json, "journal_bytes");
    const base::Json* entries = json.find("entries");
    if (!stride || stride == 0U || !records || !journal_bytes || entries == nullptr ||
        !entries->is_array())
        return {std::nullopt, index_corrupt("index field missing or mistyped")};

    Index index;
    index.stride_ticks = static_cast<std::uint32_t>(*stride);
    index.records = *records;
    index.journal_bytes = *journal_bytes;
    for (const base::Json& entry_json : entries->elements()) {
        if (!entry_json.is_object())
            return {std::nullopt, index_corrupt("index entry is not an object")};
        static constexpr std::string_view kEntryKeys[] = {
            "tick", "record_id", "offset", "frame_offset"};
        if (!detail::only_keys(entry_json, kEntryKeys))
            return {std::nullopt, index_corrupt("index entry has unknown keys")};
        const auto tick = detail::get_u64(entry_json, "tick");
        const auto record_id = detail::get_u64(entry_json, "record_id");
        const auto offset = detail::get_u64(entry_json, "offset");
        const auto frame_offset = detail::get_u64(entry_json, "frame_offset");
        if (!tick || !record_id || !offset || !frame_offset)
            return {std::nullopt, index_corrupt("index entry field missing or mistyped")};
        IndexEntry entry{.tick = *tick,
                         .record_id = *record_id,
                         .offset = *offset,
                         .frame_offset = *frame_offset};
        if (!index.entries.empty()) {
            const IndexEntry& previous = index.entries.back();
            if (entry.tick <= previous.tick || entry.record_id <= previous.record_id ||
                entry.offset <= previous.offset || entry.frame_offset <= previous.frame_offset)
                return {std::nullopt, index_corrupt("index entries are not strictly increasing")};
        }
        index.entries.push_back(entry);
    }
    return {std::move(index), std::nullopt};
}

} // namespace midday::journal

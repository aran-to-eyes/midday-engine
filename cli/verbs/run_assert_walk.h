// cli/verbs/run_assert_walk.h — shared journal-walk helpers for assertion
// packs (hoisted from run_assert.cpp at m0-determinism-spike on the
// second-consumer rule): payload probes over journal::Record plus the one
// streaming pass every pack's evaluate() opens with. Collectors keep
// exactly the records/counts their assertions cite; the walk owns the
// reader plumbing and the infrastructure-error shape.

#pragma once

#include "core/base/error.h"
#include "core/journal/reader.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace midday::cli::assertwalk {

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

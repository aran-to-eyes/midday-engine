// `midday journal diff <a> <b>` — the M0 divergence verb (Meridian D17,
// permanent; m2-replay-queries' run-diff extends it to full bundle
// semantics). Streams both bundles' RECORD CONTENT and reports the first
// divergent tick.
//
// Normalized compare (what "the same run" means here, D-BUILD-080):
//   * compared per record: tick, tier, kind, cause_id, id, and the
//     payload's canonical dump() bytes — the full causality content;
//   * NOT compared: compressed bytes / zstd framing / flush cadence (the
//     readers decompress), index granularity, and header content — the
//     identity hashes are REPORTED (identity_a/identity_b) so a config
//     mismatch is visible, but divergence is decided by records alone
//     (info fields like platform/created_at never participate, D-BUILD-033).
// A missing record (one stream ends early) IS divergence, at the tick of
// the first unmatched record.
//
// Exit contract (cmp/diff convention): 0 identical, 1 divergent (error
// journal.divergent, first_divergent_tick in the payload), 1 unreadable
// bundle, 2 usage.

#include "cli/verb.h"
#include "core/journal/reader.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace midday::cli {
namespace {

Json record_summary(const journal::Record& record) {
    Json out = Json::object();
    out.set("tick", static_cast<std::int64_t>(record.tick));
    out.set("id", static_cast<std::int64_t>(record.id));
    out.set("kind", record.kind);
    out.set("cause_id", static_cast<std::int64_t>(record.cause_id));
    out.set("tier", journal::to_string(record.tier));
    return out;
}

VerbOutcome fail(Error error) {
    VerbOutcome out;
    out.exit = Exit::Failure;
    out.error = std::move(error);
    return out;
}

VerbOutcome journal_verb(const VerbArgs& args) {
    const std::string& op = args.get_string("op");
    if (op != "diff") {
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = Error{.code = "usage.unknown_op",
                          .message = "unknown journal operation '" + op + "' (available: diff)"};
        return out;
    }

    journal::ReaderOpenResult opened_a = journal::Reader::open(args.get_string("a"));
    if (opened_a.error.has_value() || !opened_a.reader.has_value())
        return fail(std::move(opened_a.error)
                        .value_or(Error{.code = "journal.io", .message = "cannot open bundle"}));
    journal::ReaderOpenResult opened_b = journal::Reader::open(args.get_string("b"));
    if (opened_b.error.has_value() || !opened_b.reader.has_value())
        return fail(std::move(opened_b.error)
                        .value_or(Error{.code = "journal.io", .message = "cannot open bundle"}));
    journal::Reader& reader_a = *opened_a.reader;
    journal::Reader& reader_b = *opened_b.reader;

    std::int64_t compared = 0;
    std::optional<std::uint64_t> divergent_tick;
    Json divergence;
    while (true) {
        journal::Reader::NextResult next_a = reader_a.next();
        if (next_a.error.has_value())
            return fail(std::move(*next_a.error));
        journal::Reader::NextResult next_b = reader_b.next();
        if (next_b.error.has_value())
            return fail(std::move(*next_b.error));
        const bool has_a = next_a.record.has_value();
        const bool has_b = next_b.record.has_value();
        if (!has_a && !has_b)
            break;
        if (has_a != has_b) {
            const journal::Record& lone = has_a ? *next_a.record : *next_b.record;
            divergent_tick = lone.tick;
            divergence = Json::object();
            divergence.set("index", compared);
            divergence.set(has_a ? "a" : "b", record_summary(lone));
            divergence.set(has_a ? "b" : "a", "ended");
            break;
        }
        const journal::Record& a = *next_a.record;
        const journal::Record& b = *next_b.record;
        const bool equal = a.tick == b.tick && a.tier == b.tier && a.kind == b.kind &&
                           a.cause_id == b.cause_id && a.id == b.id &&
                           a.payload.dump() == b.payload.dump();
        if (!equal) {
            divergent_tick = a.tick < b.tick ? a.tick : b.tick;
            divergence = Json::object();
            divergence.set("index", compared);
            divergence.set("a", record_summary(a));
            divergence.set("b", record_summary(b));
            break;
        }
        compared += 1;
    }

    VerbOutcome out;
    out.payload.set("a", args.get_string("a"));
    out.payload.set("b", args.get_string("b"));
    out.payload.set("records_compared", compared);
    out.payload.set("identity_a", reader_a.header().replay_identity());
    out.payload.set("identity_b", reader_b.header().replay_identity());
    out.payload.set("identical", !divergent_tick.has_value());
    out.payload.set("first_divergent_tick",
                    divergent_tick.has_value() ? Json(static_cast<std::int64_t>(*divergent_tick))
                                               : Json());
    if (divergent_tick.has_value()) {
        out.payload.set("divergence", std::move(divergence));
        out.exit = Exit::Failure;
        Error error{.code = "journal.divergent",
                    .message = "journals diverge at tick " + std::to_string(*divergent_tick)};
        error.details.set("first_divergent_tick", static_cast<std::int64_t>(*divergent_tick));
        out.error = std::move(error);
        out.human = "DIVERGENT at tick " + std::to_string(*divergent_tick) + " (record " +
                    std::to_string(compared) + ")";
    } else {
        out.human = "identical: " + std::to_string(compared) + " records";
    }
    return out;
}

constexpr PositionalSpec kPositionals[] = {
    {.name = "op", .type = "string", .doc = "operation: diff"},
    {.name = "a", .type = "string", .doc = "first run.mrj bundle"},
    {.name = "b", .type = "string", .doc = "second run.mrj bundle"},
};

} // namespace

const VerbSpec& journal_spec() {
    static const VerbSpec spec{
        .name = "journal",
        .summary = "interrogate run.mrj bundles (diff: first-divergent-tick over two runs)",
        .positionals = kPositionals,
        .run = &journal_verb,
    };
    return spec;
}

} // namespace midday::cli

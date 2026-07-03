// journal.record.* — the record model: tier names, fixed JSONL byte shape,
// strict line parsing. The known-answer strings here pin the wire format
// (formats/mrj_record.schema.json mirrors them; change together).

#include "core/journal/record.h"
#include "core/journal/test_support.h"
#include "testkit/doctest.h"

using midday::journal::Record;
using midday::journal::record_from_line;
using midday::journal::Tier;
using midday::journal::tier_from_string;
using midday::journal::to_jsonl;
using midday::journal::to_string;
using midday::journal::test::unwrap;
namespace base = midday::base;

TEST_CASE("journal.record.tier_names_round_trip") {
    CHECK(to_string(Tier::Flight) == "flight");
    CHECK(to_string(Tier::Snapshot) == "snapshot");
    CHECK(to_string(Tier::Trace) == "trace");
    CHECK(tier_from_string("flight") == Tier::Flight);
    CHECK(tier_from_string("snapshot") == Tier::Snapshot);
    CHECK(tier_from_string("trace") == Tier::Trace);
    CHECK_FALSE(tier_from_string("FLIGHT").has_value());
    CHECK_FALSE(tier_from_string("").has_value());
}

TEST_CASE("journal.record.jsonl_known_answer") {
    Record record;
    record.tick = 42;
    record.tier = Tier::Flight;
    record.kind = "bus.trigger";
    record.cause_id = 3;
    record.id = 9;
    record.payload.set("event", "hit");
    CHECK(
        to_jsonl(record) ==
        R"({"tick":42,"tier":"flight","kind":"bus.trigger","cause_id":3,"id":9,"payload":{"event":"hit"}})");

    // An empty payload object is omitted entirely.
    Record bare;
    bare.tick = 0;
    bare.kind = "tick.begin";
    bare.id = 1;
    CHECK(to_jsonl(bare) ==
          R"({"tick":0,"tier":"flight","kind":"tick.begin","cause_id":0,"id":1})");
}

TEST_CASE("journal.record.line_round_trip") {
    Record record;
    record.tick = 1000;
    record.tier = Tier::Trace;
    record.kind = "ecs.delta";
    record.cause_id = 17;
    record.id = 18;
    record.payload.set("entity", 5);
    record.payload.set("component", "Transform");

    const std::string line = to_jsonl(record);
    auto parsed = record_from_line(line);
    CHECK_FALSE(parsed.error.has_value());
    const Record& round = unwrap(parsed.record);
    CHECK(round.tick == record.tick);
    CHECK(round.tier == record.tier);
    CHECK(round.kind == record.kind);
    CHECK(round.cause_id == record.cause_id);
    CHECK(round.id == record.id);
    CHECK(to_jsonl(round) == line); // canonical fixed point
}

TEST_CASE("journal.record.strict_parse_rejections") {
    const auto refused = [](std::string_view line) {
        auto parsed = record_from_line(line);
        CHECK_FALSE(parsed.record.has_value());
        return unwrap(parsed.error).code;
    };

    // Structure violations.
    CHECK(refused("[1,2,3]") == "journal.record_corrupt");
    CHECK(refused(R"({"tick":0,"tier":"flight","kind":"k","cause_id":0})") ==
          "journal.record_corrupt"); // id missing
    CHECK(refused(R"({"tick":0,"tier":"flight","kind":"k","cause_id":0,"id":1,"extra":true})") ==
          "journal.record_corrupt"); // unknown key
    CHECK(refused(R"({"tick":0,"tier":"warp","kind":"k","cause_id":0,"id":1})") ==
          "journal.record_corrupt"); // unknown tier
    CHECK(refused(R"({"tick":0,"tier":"flight","kind":"","cause_id":0,"id":1})") ==
          "journal.record_corrupt"); // empty kind
    CHECK(refused(R"({"tick":0,"tier":"flight","kind":"k","cause_id":0,"id":0})") ==
          "journal.record_corrupt"); // id 0 reserved
    CHECK(refused(R"({"tick":-1,"tier":"flight","kind":"k","cause_id":0,"id":1})") ==
          "journal.record_corrupt"); // negative tick
    CHECK(refused(R"({"tick":0,"tier":"flight","kind":"k","cause_id":0,"id":1,"payload":[]})") ==
          "journal.record_corrupt"); // payload not an object

    // Not even JSON: the parse diagnostic surfaces as json.parse.
    auto parsed = record_from_line("not json at all");
    CHECK(unwrap(parsed.error).code == "json.parse");
}

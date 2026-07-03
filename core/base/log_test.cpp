// core.log.* — tests for machine-readable logging (core/base/log.h).
// Every emitted line must conform to formats/log_record.schema.json; the
// schema checks here mirror that file field-for-field (keep them in sync).
// ts is a deterministic monotonic counter — NEVER wall clock (spec section 4.3).

#include "core/base/json.h"
#include "core/base/log.h"
#include "core/base/name.h"
#include "doctest/doctest.h"

#include <memory>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string>
#include <string_view>
#include <vector>

using midday::base::CaptureSink;
using midday::base::Json;
using midday::base::Logger;
using midday::base::LogLevel;
using midday::base::Name;

namespace {

struct TestLog {
    Logger logger;
    CaptureSink* capture = nullptr;

    TestLog() {
        auto sink = std::make_unique<CaptureSink>();
        capture = sink.get();
        logger.add_sink(std::move(sink));
    }
};

} // namespace

TEST_CASE("core.log: records serialize as schema-conformant JSONL") {
    TestLog log;
    log.logger.log(LogLevel::Info, Name("core.test"), Name("test.hello"), "hi");
    REQUIRE(log.capture->lines().size() == 1);
    CHECK(log.capture->lines()[0] ==
          R"({"ts":0,"level":"info","subsystem":"core.test","code":"test.hello","message":"hi"})");

    // Schema check, mirroring formats/log_record.schema.json:
    // required ts/level/subsystem/code/message, no additional properties.
    Json::ParseResult parsed = Json::parse(log.capture->lines()[0]);
    REQUIRE(parsed);
    const Json& record = parsed.value;
    REQUIRE(record.is_object());
    CHECK(record.items().size() == 5);
    CHECK(record.find("ts")->as_int() >= 0);
    CHECK(record.find("level")->as_string() == "info");
    CHECK(!record.find("subsystem")->as_string().empty());
    CHECK(!record.find("code")->as_string().empty());
    CHECK(record.find("message")->is_string());
}

TEST_CASE("core.log: ts is a monotonic counter, not wall clock") {
    TestLog log;
    for (int i = 0; i < 3; ++i)
        log.logger.log(LogLevel::Info, Name("core.test"), Name("test.tick"), "t");
    REQUIRE(log.capture->records().size() == 3);
    CHECK(log.capture->records()[0].ts == 0);
    CHECK(log.capture->records()[1].ts == 1);
    CHECK(log.capture->records()[2].ts == 2);
}

TEST_CASE("core.log: two independent loggers produce byte-identical streams") {
    // The determinism contract's proof shape: two runs, diffed.
    auto run = [] {
        TestLog log;
        Json fields = Json::object();
        fields.set("entity", "boss");
        fields.set("hp", 40);
        log.logger.log(LogLevel::Warn,
                       Name("core.statechart"),
                       Name("state.voided"),
                       "stagger",
                       std::move(fields));
        log.logger.log(LogLevel::Error, Name("core.loader"), Name("json.parse"), "bad ref");
        return log.capture->lines();
    };
    CHECK(run() == run());
}

TEST_CASE("core.log: levels serialize to their schema enum strings") {
    TestLog log;
    log.logger.log(LogLevel::Debug, Name("s"), Name("c"), "");
    log.logger.log(LogLevel::Info, Name("s"), Name("c"), "");
    log.logger.log(LogLevel::Warn, Name("s"), Name("c"), "");
    log.logger.log(LogLevel::Error, Name("s"), Name("c"), "");
    const auto& records = log.capture->records();
    REQUIRE(records.size() == 4);
    CHECK(midday::base::to_string(records[0].level) == "debug");
    CHECK(midday::base::to_string(records[1].level) == "info");
    CHECK(midday::base::to_string(records[2].level) == "warn");
    CHECK(midday::base::to_string(records[3].level) == "error");
}

TEST_CASE("core.log: min-level filter drops records without consuming ts") {
    TestLog log;
    log.logger.set_min_level(LogLevel::Warn);
    log.logger.log(LogLevel::Debug, Name("s"), Name("c"), "dropped");
    log.logger.log(LogLevel::Info, Name("s"), Name("c"), "dropped");
    log.logger.log(LogLevel::Warn, Name("s"), Name("c"), "kept");
    REQUIRE(log.capture->records().size() == 1);
    CHECK(log.capture->records()[0].ts == 0); // filtered records never existed
    CHECK(log.capture->records()[0].message == "kept");
}

TEST_CASE("core.log: structured fields nest under 'fields'; empty fields are omitted") {
    TestLog log;
    Json fields = Json::object();
    fields.set("tick", 18);
    fields.set("cause", "trigger.entered");
    log.logger.log(
        LogLevel::Info, Name("core.bus"), Name("event.dispatched"), "boom", std::move(fields));
    CHECK(log.capture->lines()[0] ==
          R"({"ts":0,"level":"info","subsystem":"core.bus","code":"event.dispatched",)"
          R"("message":"boom","fields":{"tick":18,"cause":"trigger.entered"}})");
    log.logger.log(LogLevel::Info, Name("core.bus"), Name("event.dispatched"), "quiet");
    CHECK(log.capture->lines()[1].find("fields") == std::string::npos);
}

TEST_CASE("core.log: every registered sink receives every line") {
    Logger logger;
    auto first = std::make_unique<CaptureSink>();
    auto second = std::make_unique<CaptureSink>();
    CaptureSink* a = first.get();
    CaptureSink* b = second.get();
    logger.add_sink(std::move(first));
    logger.add_sink(std::move(second));
    logger.log(LogLevel::Info, Name("core.test"), Name("test.fanout"), "x");
    REQUIRE(a->lines().size() == 1);
    CHECK(a->lines() == b->lines());
}

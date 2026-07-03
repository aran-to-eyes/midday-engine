#include "core/base/log.h"

#include <cassert>
#include <cstdio>
#include <utility>

namespace midday::base {

std::string_view to_string(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "error"; // unreachable with a valid enum
}

std::string to_jsonl(const LogRecord& record) {
    // Field set and order mirror formats/log_record.schema.json exactly.
    Json line = Json::object();
    line.set("ts", static_cast<std::int64_t>(record.ts));
    line.set("level", to_string(record.level));
    line.set("subsystem", record.subsystem.view());
    line.set("code", record.code.view());
    line.set("message", record.message);
    if (record.fields.is_object() && !record.fields.items().empty())
        line.set("fields", record.fields);
    return line.dump();
}

void StderrSink::write(const LogRecord&, std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
}

void CaptureSink::write(const LogRecord& record, std::string_view line) {
    records_.push_back(record);
    lines_.emplace_back(line);
}

void Logger::add_sink(std::unique_ptr<LogSink> sink) {
    sinks_.push_back(std::move(sink));
}

void Logger::set_min_level(LogLevel level) {
    min_level_ = level;
}

void Logger::log(LogLevel level, Name subsystem, Name code, std::string_view message, Json fields) {
    if (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(min_level_))
        return; // dropped records never consume a ts (determinism note in log.h)
    assert(!subsystem.empty() && !code.empty() && "schema requires non-empty subsystem/code");

    LogRecord record{.ts = next_ts_++,
                     .level = level,
                     .subsystem = subsystem,
                     .code = code,
                     .message = std::string(message),
                     .fields = std::move(fields)};
    const std::string line = to_jsonl(record);
    for (const std::unique_ptr<LogSink>& sink : sinks_)
        sink->write(record, line);
}

Logger& global_logger() {
    static Logger logger = [] {
        Logger built;
        built.add_sink(std::make_unique<StderrSink>());
        return built;
    }();
    return logger;
}

} // namespace midday::base

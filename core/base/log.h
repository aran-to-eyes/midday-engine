// core/base/log.h — machine-readable logging: every record is one JSONL line
// conforming to formats/log_record.schema.json (keep the two in sync).
//
// This is the diagnostics seam, spec-staged ahead of its consumers: it awaits
// the editor and the language/asset servers (the tools that surface diagnostics
// to humans). Sim-side records are NOT logged here — core/journal owns the
// causality skeleton (record-before-effect). "No printf in sim code" stays a
// convention until a scanner lands to enforce it.
//
// Determinism (spec section 4.3): `ts` is a per-logger monotonic counter
// (sim subsystems will carry the tick number once the tick loop exists) —
// NEVER wall clock. Two identically driven loggers emit byte-identical
// streams. Records dropped by the min-level filter never consume a ts.

#pragma once

#include "core/base/json.h"
#include "core/base/name.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace midday::base {

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

// The schema's `level` enum strings: "debug" | "info" | "warn" | "error".
std::string_view to_string(LogLevel level);

struct LogRecord {
    std::uint64_t ts = 0; // monotonic counter or sim tick — never wall clock
    LogLevel level = LogLevel::Info;
    Name subsystem;               // dotted origin, e.g. "core.loader"
    Name code;                    // stable dotted event id, e.g. "json.parse"
    std::string message;          // one-line human summary
    Json fields = Json::object(); // structured payload, omitted from JSONL when empty
};

// One record -> one JSONL line (no trailing newline; sinks own framing).
// Key order is fixed: ts, level, subsystem, code, message[, fields].
std::string to_jsonl(const LogRecord& record);

// Sinks receive both the record and its already-serialized line, so no sink
// ever re-serializes (one writer, byte-identical everywhere).
class LogSink {
public:
    LogSink() = default;
    virtual ~LogSink() = default;
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&) = delete;
    LogSink& operator=(LogSink&&) = delete;

    virtual void write(const LogRecord& record, std::string_view line) = 0;
};

// Default sink: JSONL to stderr (stdout belongs to verb envelopes, spec §9).
class StderrSink final : public LogSink {
public:
    void write(const LogRecord& record, std::string_view line) override;
};

// Test sink: captures records and lines in emission order.
class CaptureSink final : public LogSink {
public:
    void write(const LogRecord& record, std::string_view line) override;

    [[nodiscard]] const std::vector<LogRecord>& records() const { return records_; }

    [[nodiscard]] const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<LogRecord> records_;
    std::vector<std::string> lines_;
};

class Logger {
public:
    void add_sink(std::unique_ptr<LogSink> sink);

    // Records below `level` are dropped before a ts is assigned.
    void set_min_level(LogLevel level);

    // Pre: subsystem and code are non-empty (schema requires minLength 1).
    void log(LogLevel level,
             Name subsystem,
             Name code,
             std::string_view message,
             Json fields = Json::object());

private:
    std::vector<std::unique_ptr<LogSink>> sinks_;
    std::uint64_t next_ts_ = 0;
    LogLevel min_level_ = LogLevel::Debug;
};

// The engine-wide logger, born with a single StderrSink. All engine code logs
// here unless it owns a dedicated Logger (tests build their own).
Logger& global_logger();

} // namespace midday::base

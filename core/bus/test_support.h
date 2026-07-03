// core/bus/test_support.h — shared fixtures for the bus.* selftests ONLY
// (compiled into midday_bus_tests, never into the library). One canonical
// composition: Registry + World + a pinned journal Writer + the Bus, plus a
// journal read-back helper — every fixture works under a testkit TempDir
// (selftest never does cwd-dependent file IO, D-BUILD-013).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/journal/reader.h"
#include "core/journal/writer.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::bus::test {

using testkit::unwrap;

// A listener that appends "<tag>:<event>" to a shared log and optionally
// runs an action (the cascade / re-entrancy hook).
struct RecordingListener : EventListener {
    std::string tag;
    std::vector<std::string>* log = nullptr;
    std::function<void(Bus&, const EventView&)> action;

    RecordingListener(std::string tag_in, std::vector<std::string>& log_in)
        : tag(std::move(tag_in)), log(&log_in) {}

    void on_event(Bus& bus, const EventView& event) override {
        log->push_back(tag + ":" + std::string(event.event.view()));
        if (action)
            action(bus, event);
    }
};

// The pinned journal config (byte-stable across platforms, mirroring the
// journal fixtures' spelling).
inline journal::WriterConfig pinned_config() {
    journal::WriterConfig config;
    config.engine_version = "0.0.0-fixture";
    config.api_compat_hash = "0000000000000000";
    config.seed = 7;
    config.platform = "fixture-neutral";
    return config;
}

// Registry (with the built-in event vocabulary) + World + Writer + Bus
// wired the canonical way. Call finish() exactly once to close the journal
// and read every record back.
struct BusFixture {
    testkit::TempDir dir{"bus"};
    reflect::Registry registry;
    ecs::World world{registry};
    std::optional<journal::Writer> writer_slot;
    std::optional<Bus> bus_slot;

    BusFixture() {
        reflect::register_builtin_events(registry);
        auto opened = journal::Writer::create(dir.file("run.mrj"), pinned_config());
        REQUIRE_FALSE(opened.error.has_value());
        writer_slot.emplace(std::move(unwrap(opened.writer)));
        bus_slot.emplace(world, registry, unwrap(writer_slot));
    }

    // Checked accessors (the slots are always engaged after construction).
    [[nodiscard]] Bus& bus() { return unwrap(bus_slot); }

    [[nodiscard]] journal::Writer& writer() { return unwrap(writer_slot); }

    [[nodiscard]] std::string bundle_path() const { return dir.file("run.mrj"); }

    // Close the writer and stream the whole journal back.
    std::vector<journal::Record> finish() {
        REQUIRE_FALSE(writer().close().has_value());
        auto opened = journal::Reader::open(bundle_path());
        REQUIRE_FALSE(opened.error.has_value());
        journal::Reader& reader = unwrap(opened.reader);
        std::vector<journal::Record> records;
        while (true) {
            auto next = reader.next();
            REQUIRE_FALSE(next.error.has_value());
            if (!next.record.has_value())
                break;
            records.push_back(std::move(*next.record));
        }
        return records;
    }
};

// The error code carried by an optional<Error>, "<none>" when clear.
inline std::string code_of(const std::optional<base::Error>& error) {
    return error.has_value() ? error->code : std::string("<none>");
}

// Assert-and-access for a JSON object field (tests never deref a null find).
inline const base::Json& field(const base::Json& object, std::string_view key) {
    const base::Json* value = object.find(key);
    REQUIRE(value != nullptr);
    if (value == nullptr)
        std::abort(); // unreachable: REQUIRE threw
    return *value;
}

} // namespace midday::bus::test

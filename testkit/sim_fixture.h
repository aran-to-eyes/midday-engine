// testkit/sim_fixture.h — THE canonical sim composition for selftests:
// Registry (with the built-in event vocabulary) + World + Hierarchy + pinned
// journal Writer + Bus + TickLoop, under a testkit TempDir (selftest never
// does cwd-dependent file IO, D-BUILD-013). Hoisted from core/tick/
// test_support.h on its second consumer (core/physics), per the
// second-consumer rule (hex.h / temp_dir.h precedent).
//
// The journal config is PINNED (fixture version/hash/seed, platform
// "fixture-neutral") so two identically driven fixtures produce
// byte-identical bundles on every host — the dual-run determinism pattern.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/reader.h"
#include "core/journal/writer.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "core/tick/tick_loop.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::testkit {

// Call finish() exactly once to close the journal and read every record back.
struct SimFixture {
    TempDir dir{"sim"};
    reflect::Registry registry;
    ecs::World world{registry};
    hierarchy::Hierarchy hierarchy{world};
    std::optional<journal::Writer> writer_slot;
    std::optional<bus::Bus> bus_slot;
    std::optional<tick::TickLoop> loop_slot;

    explicit SimFixture(tick::TickLoopConfig config = {}) {
        reflect::register_builtin_events(registry);
        journal::WriterConfig pinned;
        pinned.engine_version = "0.0.0-fixture";
        pinned.api_compat_hash = "0000000000000000";
        pinned.seed = 7;
        pinned.platform = "fixture-neutral"; // info-only, pinned for byte-compares
        auto opened = journal::Writer::create(dir.file("run.mrj"), pinned);
        REQUIRE_FALSE(opened.error.has_value());
        writer_slot.emplace(std::move(unwrap(opened.writer)));
        bus_slot.emplace(world, registry, unwrap(writer_slot));
        loop_slot.emplace(world, hierarchy, unwrap(bus_slot), unwrap(writer_slot), config);
    }

    [[nodiscard]] tick::TickLoop& loop() { return unwrap(loop_slot); }

    [[nodiscard]] bus::Bus& bus() { return unwrap(bus_slot); }

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

// Assert-and-access for a JSON object field.
inline const base::Json& field(const base::Json& object, std::string_view key) {
    const base::Json* value = object.find(key);
    REQUIRE(value != nullptr);
    if (value == nullptr)
        std::abort(); // unreachable: REQUIRE threw
    return *value;
}

} // namespace midday::testkit

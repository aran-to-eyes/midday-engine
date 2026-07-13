// ts/runtime/component_instance_test_support.h — shared fixture for the
// script.component_instance selftests ONLY (component_instance_host_test.cpp
// and component_instance_loader_test.cpp — the prefab_test_support.h
// precedent for a cross-file test-support header). Never compiled into a
// library.

#pragma once

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"
#include "ts/runtime/component_host.h"
#include "ts/runtime/component_instance_host.h"
#include "ts/runtime/script_runtime.h"
#include "ts/toolchain/toolchain.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace midday::script::test {

// ChartFixture (registry/world/bus/journal/chart) + the two host seats +
// a fresh-cache toolchain, plus a scripts TempDir and a manifest builder.
struct HostFixture {
    statechart::test::ChartFixture fix;
    testkit::TempDir scripts{"component-instance"};
    std::optional<Toolchain> toolchain;
    ScriptRuntime runtime;
    std::optional<ComponentHost> primitives;
    std::optional<ComponentInstanceHost> host;

    explicit HostFixture(const std::string& name) {
        ToolchainConfig config;
        config.cache_dir = ".midday-cache/selftest/component_instance_" + name;
        std::filesystem::remove_all(config.cache_dir);
        toolchain.emplace(std::move(config));
        primitives.emplace(runtime, fix.world, fix.bus(), &fix.hierarchy);
        host.emplace(runtime,
                     testkit::unwrap(toolchain),
                     fix.world,
                     fix.bus(),
                     fix.writer(),
                     fix.registry,
                     testkit::unwrap(primitives));
        REQUIRE_FALSE(testkit::unwrap(host).first_error().has_value());
    }

    // Writes `source` as <stem>.ts. Returns the module path (generic form:
    // it lands in JSON string literals).
    std::string write_module(const std::string& stem, const std::string& source) {
        const std::string path = std::filesystem::path(scripts.file(stem + ".ts")).generic_string();
        REQUIRE_FALSE(base::write_file(path, source, "test.io").has_value());
        return path;
    }

    void load_manifest(const std::string& body) {
        const std::string path = scripts.file("schema_manifest.json");
        REQUIRE_FALSE(
            base::write_file(path, R"({"format_version": 2, "components": [)" + body + "]}\n", "t")
                .has_value());
        std::optional<base::Error> error = testkit::unwrap(host).load_manifest(path);
        REQUIRE_MESSAGE(!error.has_value(), (error ? error->message : std::string()));
    }
};

// The production run.cpp boot shape over an authored scene: vocab from the
// fixture's own manifest -> load_scene -> scene events -> spawn_scene with
// the host wired + deferred entry -> start_initial_entries. Asserts every
// stage; returns the spawn result (machines carry the host refs).
inline loader::SpawnResult spawn_component_scene(HostFixture& hf, const std::string& scene_path) {
    loader::ComponentVocabLoadResult vocab =
        loader::load_component_vocab(hf.scripts.file("schema_manifest.json"));
    REQUIRE_FALSE(vocab.error.has_value());
    loader::SceneLoadResult loaded =
        loader::load_scene(scene_path, hf.fix.registry, false, vocab.vocab);
    REQUIRE_MESSAGE(!loaded.error.has_value(),
                    (loaded.error ? loaded.error->message : std::string()));
    const loader::SceneFile& scene = testkit::unwrap(loaded.scene);
    REQUIRE_FALSE(loader::register_scene_events(scene, hf.fix.registry).has_value());
    loader::SpawnOptions options;
    options.scripts = &testkit::unwrap(hf.host);
    options.defer_initial_entry = true;
    loader::SpawnResult spawned = loader::spawn_scene(scene,
                                                      hf.fix.world,
                                                      hf.fix.hierarchy,
                                                      hf.fix.chart(),
                                                      nullptr,
                                                      hf.fix.writer(),
                                                      0,
                                                      options);
    REQUIRE_MESSAGE(!spawned.error.has_value(),
                    (spawned.error ? spawned.error->message : std::string()));
    REQUIRE_FALSE(loader::start_initial_entries(hf.fix.chart(), spawned).has_value());
    return spawned;
}

// All event.trigger records for one event name, journal order.
inline std::vector<journal::Record> triggers_of(const std::vector<journal::Record>& records,
                                                std::string_view event) {
    std::vector<journal::Record> out;
    for (const journal::Record& record : statechart::test::of_kind(records, "event.trigger"))
        if (statechart::test::field(record.payload, "event").as_string() == event)
            out.push_back(record);
    return out;
}

inline std::string manifest_entry(const std::string& name,
                                  const std::string& file,
                                  const std::string& fields,
                                  const std::string& events) {
    return R"({"name": ")" + name + R"(", "file": ")" + file + R"(", "fields": [)" + fields +
           R"(], "methods": [], "event_bindings": [)" + events + "]}";
}

} // namespace midday::script::test

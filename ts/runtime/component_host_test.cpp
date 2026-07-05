// script.component_host doctests (m1-ts-components exit test #3): `this`.
// emit` on a real `midday` `Component` instance routes to the component's
// OWN entity key (never a sibling entity's, never global), and a STALE
// EntityRef access (`.get()` on a despawned handle) reports the despawn
// tick and the access site through the SAME structured script.exception
// shape every other host-thrown script error already uses
// (toolchain_test.cpp's runtime_throw.ts is the precedent).

#include "core/base/file_io.h"
#include "core/bus/test_support.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"
#include "ts/runtime/component_host.h"
#include "ts/runtime/script_runtime.h"
#include "ts/toolchain/toolchain.h"

#include <filesystem>
#include <string>

using namespace midday;
using midday::base::Error;
using midday::base::Json;
using midday::bus::test::RecordingListener;
using midday::ecs::EntityRef;
using midday::script::ComponentHost;
using midday::script::ScriptRuntime;
using midday::script::Toolchain;
using midday::script::ToolchainConfig;
using midday::statechart::test::ChartFixture;

namespace {

ToolchainConfig fresh_toolchain(const std::string& name) {
    ToolchainConfig config;
    config.cache_dir = ".midday-cache/selftest/component_host_" + name;
    std::filesystem::remove_all(config.cache_dir);
    return config;
}

} // namespace

TEST_CASE("script.component_host: this.emit routes to the component's OWN entity key") {
    ChartFixture fix;
    const EntityRef pinged = fix.world.spawn();
    const EntityRef bystander = fix.world.spawn();
    REQUIRE_FALSE(pinged.is_null());
    REQUIRE_FALSE(bystander.is_null());

    std::vector<std::string> pinged_log;
    std::vector<std::string> bystander_log;
    RecordingListener pinged_listener("pinged_entity", pinged_log);
    RecordingListener bystander_listener("bystander_entity", bystander_log);
    REQUIRE_FALSE(fix.bus().subscribe(pinged_listener, bus::EventKey::entity(pinged)).has_value());
    REQUIRE_FALSE(
        fix.bus().subscribe(bystander_listener, bus::EventKey::entity(bystander)).has_value());

    Toolchain toolchain(fresh_toolchain("own_key"));
    ScriptRuntime runtime;
    ComponentHost host(runtime, fix.world, fix.bus(), &fix.hierarchy);

    testkit::TempDir scripts_dir{"component-host-own-key"};
    const std::string path = scripts_dir.file("pinger.ts");
    const std::string source =
        "import {Component, EntityRef, __attachComponent} from 'midday/component'\n"
        "export class Pinger extends Component {\n"
        "    ping(): void { this.emit('pinged', {from: 'pinger'}) }\n"
        "}\n"
        "const p = new Pinger()\n"
        "p.entity = new EntityRef(" +
        std::to_string(pinged.index) + ", " + std::to_string(pinged.generation) +
        ")\n"
        "__attachComponent(" +
        std::to_string(pinged.index) + ", " + std::to_string(pinged.generation) +
        ", 'Pinger', p)\n"
        "p.ping()\n";
    REQUIRE_FALSE(base::write_file(path, source, "test.io").has_value());

    Toolchain::LoadOutcome loaded = toolchain.load_module(runtime, path);
    REQUIRE_MESSAGE(!loaded.error.has_value(),
                    (loaded.error ? loaded.error->message : std::string()));

    CHECK(pinged_log == std::vector<std::string>{"pinged_entity:pinged"});
    CHECK(bystander_log.empty()); // never reaches a sibling entity's channel
}

TEST_CASE("script.component_host: a stale EntityRef access reports the despawn tick") {
    ChartFixture fix;
    const EntityRef ghost = fix.world.spawn();
    REQUIRE_FALSE(ghost.is_null());
    constexpr std::uint64_t kDespawnTick = 42;
    REQUIRE_FALSE(fix.world.despawn(ghost).has_value());

    Toolchain toolchain(fresh_toolchain("stale_ref"));
    ScriptRuntime runtime;
    ComponentHost host(runtime, fix.world, fix.bus(), &fix.hierarchy);
    host.note_despawn(ghost, kDespawnTick);

    testkit::TempDir scripts_dir{"component-host-stale-ref"};
    const std::string path = scripts_dir.file("access_ghost.ts");
    const std::string source =
        "import {Component, EntityRef} from 'midday/component'\n"
        "class Ghost extends Component {}\n"
        "const ref = new EntityRef(" +
        std::to_string(ghost.index) + ", " + std::to_string(ghost.generation) +
        ")\n"
        "ref.get(Ghost) // ACCESS_GHOST_MARKER: the stale access under test\n";
    REQUIRE_FALSE(base::write_file(path, source, "test.io").has_value());

    Toolchain::LoadOutcome loaded = toolchain.load_module(runtime, path);
    const Error& error = midday::testkit::unwrap(loaded.error);
    CHECK(error.code == "script.exception");
    CHECK(error.message.find("script.stale_ref") != std::string::npos);
    CHECK(error.message.find("despawn_tick=" + std::to_string(kDespawnTick)) != std::string::npos);
    CHECK(error.message.find("index=" + std::to_string(ghost.index)) != std::string::npos);
    CHECK(error.message.find("generation=" + std::to_string(ghost.generation)) !=
          std::string::npos);

    // Access site: the immediate throw checkpoint (ts/lib/component.ts) is
    // the located file:line (script_runtime.cpp's exception converter only
    // locates the innermost frame — see component_host.h's design note);
    // the full call chain, INCLUDING the fixture's own access line, is
    // captured in `stack` regardless.
    const Json* file = error.details.find("file");
    REQUIRE(file != nullptr);
    CHECK(file->as_string().find("ts/lib/component.ts") != std::string::npos);
    CHECK(error.details.find("line") != nullptr);
    const Json* stack = error.details.find("stack");
    REQUIRE(stack != nullptr);
    CHECK(stack->as_string().find("access_ghost.ts") != std::string::npos);
}

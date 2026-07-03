// reflect.init.* — init levels (core/reflect/init_levels.h).
//
// THE fixture of this node: symbols registered by a SERVERS-level hook do
// not exist while only CORE has initialized, and teardown unwinds levels in
// reverse (hooks within a level also reversed), leaving the registry empty.

#include "core/base/name.h"
#include "core/reflect/init_levels.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#include <functional>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using midday::base::Name;
using namespace midday::reflect;

namespace {

ClassDesc named_class(std::string_view name) {
    ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

EventDesc named_event(std::string_view name) {
    EventDesc event;
    event.name = Name(name);
    return event;
}

InitHooks init_only(std::function<void(Registry&)> initialize) {
    return InitHooks{.initialize = std::move(initialize), .teardown = {}};
}

} // namespace

TEST_CASE("reflect.init: SERVERS symbols are unavailable while only CORE is initialized") {
    Registry registry;
    Lifecycle boot(registry);
    boot.add_hooks(InitLevel::kCore, init_only([](Registry& r) {
                       r.add_class(named_class("CoreThing"));
                       r.add_event(named_event("core.ready"));
                   }));
    boot.add_hooks(InitLevel::kServers, init_only([](Registry& r) {
                       r.add_class(named_class("RenderServer"));
                       r.add_event(named_event("server.frame"));
                   }));

    boot.initialize_to(InitLevel::kCore);
    CHECK(boot.initialized(InitLevel::kCore));
    CHECK_FALSE(boot.initialized(InitLevel::kServers));
    CHECK(registry.find_class(Name("CoreThing")) != nullptr);
    CHECK(registry.find_event(Name("core.ready")) != nullptr);
    // The proof: the SERVERS hook has not run, so its symbols do not exist.
    CHECK(registry.find_class(Name("RenderServer")) == nullptr);
    CHECK(registry.find_event(Name("server.frame")) == nullptr);

    boot.initialize_to(InitLevel::kServers);
    const auto* server = registry.find_class(Name("RenderServer"));
    REQUIRE(server != nullptr);
    CHECK(server->level == InitLevel::kServers); // stamped by the driver

    // Teardown returns the registry to empty; SERVERS dies before CORE.
    boot.teardown_all();
    CHECK(registry.find_class(Name("RenderServer")) == nullptr);
    CHECK(registry.find_class(Name("CoreThing")) == nullptr);
    CHECK(registry.classes().empty());
    CHECK(registry.events().empty());
    CHECK_FALSE(boot.initialized(InitLevel::kCore));
}

TEST_CASE("reflect.init: hooks run in level order and tear down exactly mirrored") {
    Registry registry;
    Lifecycle boot(registry);
    std::vector<std::string> trace;
    auto tracer = [&trace](const char* label) {
        return InitHooks{
            .initialize = [&trace,
                           label](Registry&) { trace.push_back(std::string(label) + ".init"); },
            .teardown = [&trace,
                         label](Registry&) { trace.push_back(std::string(label) + ".down"); },
        };
    };
    // Deliberately contributed out of level order; two hooks share CORE.
    boot.add_hooks(InitLevel::kScene, tracer("scene"));
    boot.add_hooks(InitLevel::kCore, tracer("core_a"));
    boot.add_hooks(InitLevel::kTools, tracer("tools"));
    boot.add_hooks(InitLevel::kCore, tracer("core_b"));
    boot.add_hooks(InitLevel::kServers, tracer("servers"));

    boot.initialize_to(InitLevel::kTools);
    boot.teardown_all();

    const std::vector<std::string> expected{
        // up: CORE -> SERVERS -> SCENE -> TOOLS, add-order within a level
        "core_a.init",
        "core_b.init",
        "servers.init",
        "scene.init",
        "tools.init",
        // down: exact mirror
        "tools.down",
        "scene.down",
        "servers.down",
        "core_b.down",
        "core_a.down"};
    CHECK(trace == expected);
}

TEST_CASE("reflect.init: partial climbs are idempotent and resumable") {
    Registry registry;
    Lifecycle boot(registry);
    int core_runs = 0;
    boot.add_hooks(InitLevel::kCore, init_only([&core_runs](Registry&) { ++core_runs; }));

    boot.initialize_to(InitLevel::kCore);
    boot.initialize_to(InitLevel::kCore); // already there: no re-run
    CHECK(core_runs == 1);

    // Hooks for a NOT-yet-initialized level may still be contributed.
    bool scene_ran = false;
    boot.add_hooks(InitLevel::kScene, init_only([&scene_ran](Registry&) { scene_ran = true; }));
    boot.initialize_to(InitLevel::kScene); // climbs SERVERS (empty) + SCENE
    CHECK(scene_ran);
    CHECK(boot.initialized(InitLevel::kServers));
    CHECK(registry.active_level() == InitLevel::kScene);

    // After a full teardown the ladder is climbable again (editor restarts).
    boot.teardown_all();
    boot.initialize_to(InitLevel::kCore);
    CHECK(core_runs == 2);
}

TEST_CASE("reflect.init: hook registrations stamp the level being initialized") {
    Registry registry;
    Lifecycle boot(registry);
    boot.add_hooks(InitLevel::kServers, init_only([](Registry& r) {
                       CHECK(r.active_level() == InitLevel::kServers);
                       r.add_class(named_class("PhysicsServer"));
                   }));
    boot.initialize_to(InitLevel::kServers);
    CHECK(registry.find_class(Name("PhysicsServer"))->level == InitLevel::kServers);
}

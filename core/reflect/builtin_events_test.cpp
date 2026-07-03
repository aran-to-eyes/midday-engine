// reflect.events.* — the built-in event vocabulary (builtin_events.h).
//
// Under test: every spec-4.2/Appendix-A event registers at CORE with a
// typed payload schema queryable from the registry, and the API fixture —
// walking the registry produces the JSON description of trigger.entered
// including payload fields: the seed of engine_api.json (m0-api-json).

#include "core/base/hex.h"
#include "core/base/name.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/init_levels.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <set>
#include <string>
#include <string_view>
#include <vector>

using midday::base::hex64;
using midday::base::Name;
using namespace midday::reflect;

namespace {

constexpr std::string_view kVocabulary[] = {
    "trigger.entered",
    "trigger.exited",
    "contact.began",
    "contact.ended",
    "state.finished",
    "entity.spawned",
    "entity.despawned",
    "action.pressed",
    "action.released",
};

} // namespace

TEST_CASE("reflect.events: the whole vocabulary registers at CORE, typed and queryable") {
    Registry registry;
    register_builtin_events(registry);

    for (std::string_view name : kVocabulary) {
        const auto* event = registry.find_event(Name(name));
        REQUIRE_MESSAGE(event != nullptr, name);
        CHECK(event->level == InitLevel::kCore);
        CHECK_MESSAGE(!event->desc.payload.empty(), name);
        CHECK_MESSAGE(!event->desc.doc.empty(), name);
        CHECK(event->desc.compat_hash != 0);
    }
    // Exactly the vocabulary — nothing implicit, nothing extra.
    CHECK(registry.events().size() == std::size(kVocabulary));

    // Enumeration preserves the canonical declaration order.
    std::vector<std::string> listed;
    listed.reserve(registry.events().size());
    for (const auto* event : registry.events())
        listed.emplace_back(event->desc.name.view());
    CHECK(listed == std::vector<std::string>(std::begin(kVocabulary), std::end(kVocabulary)));
}

TEST_CASE("reflect.events: payload schemas carry the contracted field types") {
    Registry registry;
    register_builtin_events(registry);
    auto field_type = [&](std::string_view event, std::string_view field) -> std::string {
        const auto* entry = registry.find_event(Name(event));
        REQUIRE(entry != nullptr);
        for (const auto& f : entry->desc.payload)
            if (f.name == Name(field))
                return f.type.canonical();
        FAIL(event << " has no field " << field);
        return {};
    };

    CHECK(field_type("trigger.entered", "trigger") == "entity_ref");
    CHECK(field_type("trigger.entered", "other") == "entity_ref");
    CHECK(field_type("trigger.exited", "other") == "entity_ref");
    CHECK(field_type("contact.began", "self") == "entity_ref");
    CHECK(field_type("contact.began", "other") == "entity_ref");
    CHECK(field_type("contact.began", "position") == "vec3");
    CHECK(field_type("contact.began", "normal") == "vec3");
    CHECK(field_type("contact.began", "impulse") == "float");
    CHECK(field_type("contact.ended", "other") == "entity_ref");
    CHECK(field_type("state.finished", "entity") == "entity_ref");
    CHECK(field_type("state.finished", "region") == "name");
    CHECK(field_type("state.finished", "state") == "name");
    CHECK(field_type("entity.spawned", "entity") == "entity_ref");
    CHECK(field_type("entity.spawned", "parent") == "entity_ref");
    CHECK(field_type("entity.despawned", "entity") == "entity_ref");
    CHECK(field_type("action.pressed", "action") == "name");
    CHECK(field_type("action.pressed", "strength") == "float");
    CHECK(field_type("action.pressed", "device") == "int");
    CHECK(field_type("action.released", "action") == "name");
}

TEST_CASE("reflect.events: compat hashes are distinct and pinned") {
    Registry registry;
    register_builtin_events(registry);
    std::set<std::string> hashes;
    for (const auto* event : registry.events())
        hashes.insert(hex64(event->desc.compat_hash));
    CHECK(hashes.size() == std::size(kVocabulary)); // pairwise distinct

    // Known-answer pin: the drift-detection primitive for api diff. If this
    // moves, the vocabulary's signature changed — a deliberate API break.
    CHECK(hex64(registry.find_event(Name("trigger.entered"))->desc.compat_hash) ==
          "d9d4b0d4f4ce21a0");
}

TEST_CASE("reflect.events: API fixture — the registry generates TriggerEntered") {
    // The exit test of this node: walk the registry and produce the JSON
    // description of trigger.entered incl. payload fields — the exact seed
    // m0-api-json grows into engine_api.json, byte-pinned.
    Registry registry;
    register_builtin_events(registry);
    const auto* entry = registry.find_event(Name("trigger.entered"));
    REQUIRE(entry != nullptr);

    CHECK(event_payload_type_name(entry->desc.name.view()) == "TriggerEntered");
    CHECK(describe(*entry).dump() ==
          R"({"name":"trigger.entered","level":"core",)"
          R"("doc":"A body began overlapping a trigger volume. Key: the trigger entity.",)"
          R"("payload":[)"
          R"({"name":"trigger","type":"entity_ref","doc":"The trigger volume's entity."},)"
          R"({"name":"other","type":"entity_ref","doc":"The entity that entered."}],)"
          R"("compat_hash":"d9d4b0d4f4ce21a0"})");
}

TEST_CASE("reflect.events: vocabulary registers through the CORE init hook") {
    // Integration with the ladder: the canonical boot shape — builtins are
    // a CORE hook, so they exist before any SERVERS/SCENE symbol ever can.
    Registry registry;
    Lifecycle boot(registry);
    boot.add_hooks(
        InitLevel::kCore,
        InitHooks{.initialize = [](Registry& r) { register_builtin_events(r); }, .teardown = {}});
    CHECK(registry.find_event(Name("trigger.entered")) == nullptr); // not yet
    boot.initialize_to(InitLevel::kCore);
    CHECK(registry.find_event(Name("trigger.entered")) != nullptr);
    boot.teardown_all();
    CHECK(registry.events().empty());
}

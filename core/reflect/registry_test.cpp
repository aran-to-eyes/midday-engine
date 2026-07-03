// reflect.registry.* — the reflection registry (core/reflect/registry.h).
//
// Under test: registration/lookup, deterministic level-major enumeration,
// stable descriptor addresses, and the compat-hash contract — hashes cover
// the SIGNATURE (names, types, defaults, flags, level), never doc strings,
// and are pinned by known-answer vectors so drift cannot pass silently.

#include "core/base/hex.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/reflect/registry.h"
#include "doctest/doctest.h"

#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string>
#include <string_view>
#include <vector>

using midday::base::hex64;
using midday::base::Json;
using midday::base::Name;
using namespace midday::reflect;

namespace {

// The recurring fixture class: one property with default+flags, one method.
ClassDesc health_class() {
    ClassDesc cls;
    cls.name = Name("Health");
    cls.doc = "Hit points with clamped damage application.";
    cls.properties.push_back(PropertyDesc{
        .name = Name("value"),
        .type = TypeDesc::scalar(TypeKind::kFloat),
        .default_value = Json(100.0),
        .flags = kPropertySave,
        .doc = "Current hit points.",
    });
    MethodDesc damage;
    damage.name = Name("damage");
    damage.params.push_back(ParamDesc{
        .name = Name("amount"),
        .type = TypeDesc::scalar(TypeKind::kFloat),
        .default_value = Json(),
    });
    damage.returns = TypeDesc::scalar(TypeKind::kFloat);
    damage.doc = "Applies damage, returns remaining hit points.";
    cls.methods.push_back(std::move(damage));
    return cls;
}

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

} // namespace

TEST_CASE("reflect.registry: registration stores, stamps the level, and finds by name") {
    Registry registry;
    const auto& stored = registry.add_class(health_class());
    CHECK(stored.level == InitLevel::kCore); // default active level
    CHECK(stored.desc.name == Name("Health"));
    CHECK(stored.desc.compat_hash != 0);
    CHECK(stored.desc.methods[0].compat_hash != 0); // per-method hash (spec 4.3)

    const auto* found = registry.find_class(Name("Health"));
    REQUIRE(found != nullptr);
    CHECK(found == &stored); // stable address: lookup IS the stored entry
    CHECK(found->desc.properties[0].type.canonical() == "float");
    CHECK(registry.find_class(Name("Mana")) == nullptr);
    CHECK(registry.find_event(Name("Health")) == nullptr); // separate namespaces
}

TEST_CASE("reflect.registry: enumeration is level-major, registration-order minor") {
    Registry registry;
    registry.set_active_level(InitLevel::kServers);
    registry.add_class(named_class("RenderServer"));
    registry.set_active_level(InitLevel::kCore);
    registry.add_class(named_class("Zebra"));
    registry.add_class(named_class("Aardvark"));

    // CORE entries precede SERVERS regardless of call interleaving; within
    // a level, registration order — never name or pointer order.
    const auto classes = registry.classes();
    REQUIRE(classes.size() == 3);
    CHECK(classes[0]->desc.name == Name("Zebra"));
    CHECK(classes[1]->desc.name == Name("Aardvark"));
    CHECK(classes[2]->desc.name == Name("RenderServer"));
}

TEST_CASE("reflect.registry: compat hash covers the signature, never the docs") {
    Registry a;
    Registry b;
    ClassDesc original = health_class();
    ClassDesc reworded = health_class();
    reworded.doc = "Completely different prose.";
    reworded.properties[0].doc = "Also different.";
    reworded.methods[0].doc = "Different again.";
    CHECK(a.add_class(original).desc.compat_hash == b.add_class(reworded).desc.compat_hash);

    // Any signature change moves the hash: type, default, flags, params.
    Registry c;
    ClassDesc retyped = health_class();
    retyped.properties[0].default_value = Json(50.0);
    CHECK(c.add_class(retyped).desc.compat_hash != a.find_class(Name("Health"))->desc.compat_hash);

    Registry d;
    ClassDesc reparammed = health_class();
    reparammed.methods[0].params.push_back(ParamDesc{
        .name = Name("source"),
        .type = TypeDesc::scalar(TypeKind::kEntityRef),
        .default_value = Json("self"),
    });
    const auto& stored = d.add_class(reparammed);
    CHECK(stored.desc.compat_hash != a.find_class(Name("Health"))->desc.compat_hash);
    CHECK(stored.desc.methods[0].compat_hash !=
          a.find_class(Name("Health"))->desc.methods[0].compat_hash);
}

TEST_CASE("reflect.registry: compat hashes are pinned known answers") {
    // Cross-platform drift anchors, exactly like the core.name vectors: if
    // these move, every committed engine_api.json compat hash rots with them.
    Registry registry;
    const auto& cls = registry.add_class(health_class());
    CHECK(hex64(cls.desc.compat_hash) == "f6bad6a2b8e97082");
    CHECK(hex64(cls.desc.methods[0].compat_hash) == "3040ea97ccf396d2");

    MethodDesc clampf;
    clampf.name = Name("clamp");
    clampf.params.push_back(ParamDesc{
        .name = Name("x"), .type = TypeDesc::scalar(TypeKind::kFloat), .default_value = Json()});
    clampf.params.push_back(ParamDesc{
        .name = Name("lo"), .type = TypeDesc::scalar(TypeKind::kFloat), .default_value = Json()});
    clampf.params.push_back(ParamDesc{
        .name = Name("hi"), .type = TypeDesc::scalar(TypeKind::kFloat), .default_value = Json()});
    clampf.returns = TypeDesc::scalar(TypeKind::kFloat);
    CHECK(hex64(registry.add_function(std::move(clampf)).desc.compat_hash) == "d10ee01d61c3a636");
}

TEST_CASE("reflect.registry: free functions register and describe (expr-lang seat)") {
    Registry registry;
    MethodDesc fn;
    fn.name = Name("min");
    fn.params.push_back(ParamDesc{
        .name = Name("a"), .type = TypeDesc::scalar(TypeKind::kFloat), .default_value = Json()});
    fn.params.push_back(ParamDesc{
        .name = Name("b"), .type = TypeDesc::scalar(TypeKind::kFloat), .default_value = Json()});
    fn.returns = TypeDesc::scalar(TypeKind::kFloat);
    fn.doc = "Smaller of two numbers.";
    const auto& stored = registry.add_function(std::move(fn));

    CHECK(registry.find_function(Name("min")) == &stored);
    const Json json = describe(stored);
    CHECK(json.find("name")->as_string() == "min");
    CHECK(json.find("level")->as_string() == "core");
    CHECK(json.find("returns")->as_string() == "float");
    CHECK(json.find("params")->elements().size() == 2);
    CHECK(json.find("compat_hash")->as_string() == hex64(stored.desc.compat_hash));
}

TEST_CASE("reflect.registry: remove_level drops exactly that level's entries") {
    Registry registry;
    registry.add_class(named_class("CoreThing"));
    registry.add_event(named_event("core.ready"));
    registry.set_active_level(InitLevel::kServers);
    registry.add_class(named_class("ServerThing"));
    registry.add_event(named_event("server.ready"));

    registry.remove_level(InitLevel::kServers);
    CHECK(registry.find_class(Name("ServerThing")) == nullptr);
    CHECK(registry.find_event(Name("server.ready")) == nullptr);
    CHECK(registry.find_class(Name("CoreThing")) != nullptr);
    CHECK(registry.find_event(Name("core.ready")) != nullptr);
    CHECK(registry.classes().size() == 1);
    CHECK(registry.events().size() == 1);
}

TEST_CASE("reflect.registry: to_json enumerates all buckets deterministically") {
    Registry registry;
    registry.add_class(health_class());
    registry.add_event(named_event("core.ready"));
    const Json api = registry.to_json();
    CHECK(api.find("classes")->elements().size() == 1);
    CHECK(api.find("events")->elements().size() == 1);
    CHECK(api.find("functions")->elements().size() == 0);
    // Byte-determinism across two walks of the same registry.
    CHECK(api.dump() == registry.to_json().dump());
}

TEST_CASE("reflect.registry: event payload type names are fixed derivations") {
    CHECK(event_payload_type_name("trigger.entered") == "TriggerEntered");
    CHECK(event_payload_type_name("contact.began") == "ContactBegan");
    CHECK(event_payload_type_name("entity.spawned") == "EntitySpawned");
    CHECK(event_payload_type_name("action.pressed") == "ActionPressed");
    CHECK(event_payload_type_name("state.finished") == "StateFinished");
    CHECK(event_payload_type_name("boss_fight.phase_two") == "BossFightPhaseTwo");
}

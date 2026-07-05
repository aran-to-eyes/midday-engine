// ts/runtime/component_host.h — the script-facing entity/component access
// seat (m1-ts-components): `entity.get/tryGet/has/root` and `this.emit`'s
// own-key sugar are TS-side (ts/lib/component.ts); this class supplies the
// small set of NATIVE primitives that side cannot provide itself.
//
// Two collaborators:
//   * core::ecs::World — the ONLY source of truth for aliveness (generational
//     EntityRef, core/ecs/entity.h). TS-authored (@component) instances have
//     NO C++ type, so they can never live in a typed ecs::Pool<T>
//     (World::pool_ref<T> aborts on an unregistered type by design,
//     core/ecs/world.h) — their data lives entirely as live QuickJS objects,
//     directoried by ts/lib/component.ts itself (module-local state, never a
//     global): this seat never sees a component VALUE, only entity handles.
//   * core::hierarchy::Hierarchy (optional) — entity.root() reuses
//     Hierarchy::owner_of(), "the nearest ancestor-or-self marked owner"
//     (core/hierarchy/hierarchy.h) — precisely spec 4.2's "the owning entity
//     from any state-subtree child". A caller with no hierarchy (bare ECS,
//     e.g. a unit test) gets identity root(): a leaf is its own root.
//
// Stale access (despawn tick + access site): entity.get/tryGet/root throw a
// PLAIN JS Error from ts/lib/component.ts — deliberately never a native
// JS_ThrowTypeError. Reading script_runtime.cpp's exception converter
// (error_from_value/parse_top_frame) shows it locates a callsite ONLY for a
// pure-JS throw; a throw raised from INSIDE a native host function sits
// behind an extra "(native)" stack frame the parser does not attempt to
// look past, so a native-origin error would carry no file:line at all. The
// two primitives here (status/root) therefore never throw — they are plain,
// always-succeeding queries — and ts/lib/component.ts is the one deciding
// whether to raise, so the resulting exception is captured exactly like
// runtime_throw.ts / state_script_test.cpp's "boom.ts" already are. The
// despawn tick and entity identity travel as a stable, greppable message
// substring: the SAME convention m1-events-format's bus.payload_invalid
// halt already established (structured `details` never survive the host ->
// JS throw boundary — script_runtime.cpp's host_call always reduces a
// HostResult::error to "code: message" text, proven by reading it).
//
// The despawn-tick record is intentionally minimal: one (generation, tick)
// pair per slot INDEX (the latest despawn only — a stale ref by definition
// names one specific dead generation, so the latest recorded despawn for
// that generation is always the right one; older history is not needed).
// note_despawn() is a recording seam, not a mutator: nothing here calls
// World::despawn — the entity-API scanner (scripts/check_entity_api.py)
// rightly forbids that outside core/ or a _test.cpp. Today only this node's
// own tests call it; the real structural-apply despawn path picks it up
// once components are wired into a live scene (m1-prefab-spawn territory,
// out of this node's scope).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "ts/runtime/script_runtime.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace midday::script {

class ComponentHost {
public:
    // The seam contract (mirrors state_script.h's kRegisterFn-style pins):
    // ts/lib/component.ts calls these by exactly these names.
    static constexpr std::string_view kStatusFn = "__midday_entity_status";
    static constexpr std::string_view kRootFn = "__midday_entity_root";
    static constexpr std::string_view kTriggerEntityFn = "__midday_trigger_entity";
    static constexpr std::string_view kTriggerNamedFn = "__midday_trigger_named";

    // `hierarchy` may be null: entity.root() then degrades to identity
    // (every entity is its own root). `world`/`bus` must outlive this seat.
    ComponentHost(ScriptRuntime& runtime,
                  ecs::World& world,
                  bus::Bus& bus,
                  hierarchy::Hierarchy* hierarchy = nullptr);

    ComponentHost(const ComponentHost&) = delete;
    ComponentHost& operator=(const ComponentHost&) = delete;
    ComponentHost(ComponentHost&&) = delete;
    ComponentHost& operator=(ComponentHost&&) = delete;
    ~ComponentHost() = default;

    // Record that `ref` despawned at `tick` (see header note: latest-only,
    // keyed by slot index). A later status() query for exactly this
    // (index, generation) reports the tick back; any OTHER incarnation of
    // the slot reports despawn_tick: null (unknown — honestly, not "never").
    void note_despawn(ecs::EntityRef ref, std::uint64_t tick);

private:
    [[nodiscard]] HostResult status(const base::Json::Array& args) const;
    [[nodiscard]] HostResult root(const base::Json::Array& args) const;
    [[nodiscard]] HostResult trigger_entity(const base::Json::Array& args);
    [[nodiscard]] HostResult trigger_named(const base::Json::Array& args);
    // The shared tail of both trigger seats: fire `event` at `key`, and hand
    // back the journal record id — or the bus's structured refusal.
    [[nodiscard]] HostResult
    fire(bus::EventKey key, std::string_view event, const base::Json& payload) const;

    ecs::World* world_;
    bus::Bus* bus_;
    hierarchy::Hierarchy* hierarchy_;
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint64_t>> despawn_ticks_;
};

} // namespace midday::script

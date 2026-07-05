// core/loader/loader.h — the PERMANENT strict-YAML loader subset
// (m0-yaml-loader-run): authored scene / machine / events text -> the
// EXISTING desc aggregates (statechart::MachineDesc and friends) -> live
// World/Hierarchy instantiation. There is no other loader and never will
// be: m1-scene-format extends THIS surface in place (schema-manifest
// validation, uid dual-write, prefab overrides), and spec section 7's "no
// code-assembled entities in public paths" holds from the first golden.
//
// M0 grammar (formats/loader_yaml.md is the format contract; D-BUILD-077):
//   * scene:   format, scene, events: [paths], entities: [{entity,
//              components: [{Transform|Collider|RigidBody: {...}}],
//              machines: [{instance: {path}}]}]
//   * machine: format, machine, vars: {name: type}, regions: {<name>:
//              {initial, history, anystate: [pair...], states: {<State>:
//              {script, on: [pair...], sequence, states, initial, history,
//              children: [{entity, at}]}}}}
//              pair = {event, key, goto, priority, if}; `on:` is sugar that
//              canonicalizes to TransitionDesc pairs at load; `self.finished`
//              canonicalizes to "<owning state>.finished"; sequence `then: S`
//              canonicalizes to the {<state>.finished -> S} pair (spec 4.1).
//   * events:  format, events: {<name>: {payload: {field: type}, doc}},
//              keys: [group names] — canonical reflect TypeDesc spellings.
//
// STRICT: unknown keys, bad state refs, bad event refs, undeclared group
// keys, type mismatches, and duplicate names all refuse with a structured
// error carrying file:line:col (exit 3 at the CLI). Event names referenced
// by pairs/triggers must be declared (events files), built-in (reflect
// vocabulary), or derived ("<state>.finished", "<span>.opened"/".closed").
//
// SYMBOLIC KEYS (spec 4.2 vocabulary): `self`/`root` are the host entity's
// private channel (the machine's always-on subscription; resolved at
// spawn), `global` and declared <group> names are shared named channels —
// pairs listening on them add the channel to MachineDesc::channels. NOTE
// (M0 core semantics, D-BUILD-046): transition tables match by EVENT NAME
// across every subscribed channel of the machine.

#pragma once

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/math/xform.h"
#include "core/reflect/registry.h"
#include "core/statechart/machine_desc.h"
#include "core/statechart/statechart.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::ecs {
class World;
}

namespace midday::hierarchy {
class Hierarchy;
}

namespace midday::journal {
class Writer;
}

namespace midday::physics {
class PhysicsServer;
}

namespace midday::loader {

inline constexpr std::int64_t kFormatVersion = 1; // every text format carries format: N

// ---- events files -----------------------------------------------------------
struct EventFieldDecl {
    std::string name = {};
    reflect::TypeDesc type = reflect::TypeDesc::scalar(reflect::TypeKind::kFloat);
};

struct EventDecl {
    std::string name = {}; // dotted vocabulary name
    std::string doc = {};
    std::vector<EventFieldDecl> payload = {};
};

// The merged project vocabulary: declared events + declared group keys.
struct EventsDecl {
    std::vector<EventDecl> events = {};
    std::vector<std::string> group_keys = {}; // named channels usable as `key:`

    [[nodiscard]] bool has_event(std::string_view name) const;
    [[nodiscard]] bool has_group(std::string_view name) const;
};

struct EventsLoadResult {
    EventsDecl decl;
    std::optional<base::Error> error;
};

// Parse ONE events file, appending into `decl` (cross-file duplicates
// refuse). `registry` guards collisions with the built-in vocabulary.
std::optional<base::Error>
load_events_file(const std::string& path, const reflect::Registry& registry, EventsDecl& decl);

// The PROJECT-WIDE namespace (m1-events-format): every `*.events.yaml`
// found under `root_dir` (recursive, lexicographically sorted for
// determinism), merged into ONE EventsDecl via repeated load_events_file
// calls on the SAME accumulator — so a name declared in two different
// files under the root refuses (loader.duplicate, file:line:col) even when
// no single scene lists both files. `root_dir` IS the project boundary for
// this purpose: until m1-project-new defines a real project root, the
// caller supplies one (`midday validate` uses the target file's own
// directory, mirroring the scene loader's "project root = the file's
// directory" convention, loader_yaml.md); once a real root exists, callers
// simply point it there instead — the walk/merge logic is unchanged.
struct ProjectEventsResult {
    EventsDecl decl;
    std::vector<std::string> files = {}; // discovered *.events.yaml paths, sorted
    std::optional<base::Error> error;
};

ProjectEventsResult load_project_events(const std::string& root_dir,
                                        const reflect::Registry& registry);

// ---- input action maps (m1-input-actions) -----------------------------------
// `*.input.yaml`: named actions -> raw device bindings, plus named virtual-
// stick composites (spec section 4.3 "Input": event hierarchy + named action
// maps; action maps abstract touch). SPEC-GAP: the spec fixes no file
// convention for action maps (exactly like `*.events.yaml` before it) —
// `*.input.yaml` / `*.input_profile.yaml` (the runtime rebinding overlay,
// spec section 13 "input rebinding is data ... a user-profile overlay") are
// this node's call: project-config documents merged project-wide exactly
// like events (load_project_events precedent) — one namespace of action
// names across every *.input.yaml a project owns.
//
// Raw controls are opaque device-specific strings (a real OS/device backend
// is m7-platform territory; this node is the DATA plus the synthetic
// injection seam, core/input/inject.h) — device KINDS are the only closed
// vocabulary.
enum class DeviceKind : std::uint8_t {
    kKeyboard = 0,
    kMouse = 1,
    kGamepad = 2,
    kTouch = 3,
};

std::string_view to_string(DeviceKind kind);
// nullopt for anything outside {keyboard, mouse, gamepad, touch}.
std::optional<DeviceKind> device_kind_from_string(std::string_view text);

struct BindingDesc {
    DeviceKind device = DeviceKind::kKeyboard;
    std::string control; // opaque device-specific control id, e.g. "space", "button_south"

    friend bool operator==(const BindingDesc&, const BindingDesc&) = default;
};

struct ActionDesc {
    std::string name;
    std::vector<BindingDesc> bindings;
};

// A named 2D composite over four DECLARED actions (the Godot InputMap::
// get_vector precedent, core/input/action_state.h): up/down/left/right name
// actions already known when the stick is parsed (this file, or an earlier-
// sorted *.input.yaml under the same project root — single-pass discipline,
// no forward refs across files).
struct StickDesc {
    std::string name;
    std::string up;
    std::string down;
    std::string left;
    std::string right;
    float deadzone = 0.2f; // Godot's InputMap default; valid range [0, 1)
};

struct ActionMapDecl {
    std::vector<ActionDesc> actions;
    std::vector<StickDesc> sticks;

    [[nodiscard]] const ActionDesc* find_action(std::string_view name) const;

    [[nodiscard]] bool has_action(std::string_view name) const {
        return find_action(name) != nullptr;
    }

    [[nodiscard]] bool has_stick(std::string_view name) const;
};

// Parse ONE `*.input.yaml` file, appending into `decl` (cross-file duplicate
// action/stick names refuse, exactly like load_events_file). Two DIFFERENT
// actions claiming the same (device, control) binding anywhere under the
// merged project refuse "input.conflict" at the offending (second)
// binding's file:line:col — the validator refusal exit-test rides this
// (`midday validate <f>.input.yaml`, extension dispatch, cli/verbs/
// validate.cpp).
std::optional<base::Error> load_input_file(const std::string& path, ActionMapDecl& decl);

struct ProjectInputResult {
    ActionMapDecl decl;
    std::vector<std::string> files = {}; // discovered *.input.yaml paths, sorted
    std::optional<base::Error> error;
};

// The project-wide namespace (load_project_events precedent): every
// *.input.yaml under `root_dir` (recursive, lexicographically sorted for
// determinism), merged via repeated load_input_file calls on the SAME
// accumulator.
ProjectInputResult load_project_input(const std::string& root_dir);

// ---- the runtime rebinding overlay (spec section 13: "a user-profile
// overlay") -------------------------------------------------------------------
// `*.input_profile.yaml`: `overlay: {<action>: {bindings: [...]}}` — the
// SAME action/binding grammar a base map's `actions:` uses (no `sticks:`: a
// rebind changes which raw control drives an action, never which actions
// feed a stick). One file, one profile — no project-wide merge (a player's
// profile is not a project-config namespace).
std::optional<base::Error> load_input_profile_file(const std::string& path, ActionMapDecl& overlay);

struct ApplyOverlayResult {
    ActionMapDecl map;
    std::optional<base::Error> error; // loader.bad_ref (rebinds an undeclared
                                      // action) | input.conflict (the rebind
                                      // collides with another action's binding)
};

// `base` with every action the overlay mentions REPLACED wholesale by the
// overlay's binding list (full replacement — the simplest, least-surprising
// rebind semantics; partial add/remove is a game-UI concern, spec section
// 13). The result is re-validated for conflicts: a rebind can introduce a
// collision the original authoring never had.
ApplyOverlayResult apply_overlay(const ActionMapDecl& base, const ActionMapDecl& overlay);

// The post-merge conflict scan `apply_overlay` (and the `.input_profile.yaml`
// CLI self-check) reuse: two DIFFERENT actions in `actions` sharing a
// (device, control) binding. No file:line:col (not tied to a parse site) —
// load_input_file's own incremental, file:line:col-carrying check is the
// authoring-time path.
std::optional<base::Error> find_conflict(const std::vector<ActionDesc>& actions);

// ---- the M0 component vocabulary (scene entities) ---------------------------
// Exactly the base components that EXIST in the runtime today: Transform
// (hierarchy local TRS), Collider + RigidBody (the M0 physics surface:
// dynamic box needs both; a plane collider alone is the static ground).
// m1-ts-components / m4-physics-full extend this vocabulary in place.
struct ColliderDesc {
    bool box = false;  // false = ground plane
    math::Vec3 size{}; // box full extents (authored size, halved for Jolt)
};

struct ComponentSet {
    std::optional<math::Transform> transform;
    std::optional<ColliderDesc> collider;
    bool rigid_body = false;
};

// ---- machine files -----------------------------------------------------------
// A child entity authored under a STATE node (A.3's Hurtbox-under-
// HitboxLive): spawned at scene spawn, attached beneath the state's
// hierarchy entity, dormant exactly while the state is inactive.
struct StateChildDesc {
    base::Name entity;
    math::Transform at; // `at:` sugar — the child's local translation
};

struct StateScriptRef {
    base::Name region;
    base::Name state;
    std::string ref;  // as authored (project-root-relative)
    std::string path; // resolved against the project root
    int line = 0;     // authoring location (diagnostics)
};

struct StateChildren {
    base::Name region;
    base::Name state;
    std::vector<StateChildDesc> children;
};

struct MachineFile {
    statechart::MachineDesc desc; // the EXISTING aggregate, load-ready
    std::vector<StateScriptRef> scripts;
    std::vector<StateChildren> children;
    std::string path; // resolved file path (identity for dedup)
};

struct MachineLoadResult {
    std::optional<MachineFile> machine;
    std::optional<base::Error> error;
};

// Parse + validate one machine file. `vocab` is the merged project events
// declaration; `root_dir` resolves script refs.
MachineLoadResult load_machine_file(const std::string& path,
                                    const std::string& root_dir,
                                    const reflect::Registry& registry,
                                    const EventsDecl& vocab);

// ---- scene files --------------------------------------------------------------
struct SceneEntityDesc {
    base::Name name;
    ComponentSet components;
    std::vector<std::uint32_t> machines; // indexes into SceneFile::machines
    int line = 0;
};

struct SceneFile {
    base::Name name;
    std::string path;                  // the scene file as given
    std::string root_dir;              // project root = the scene file's directory
    EventsDecl events;                 // merged from every listed events file
    std::vector<MachineFile> machines; // deduplicated by resolved path
    std::vector<SceneEntityDesc> entities;
};

struct SceneLoadResult {
    std::optional<SceneFile> scene;
    std::optional<base::Error> error;
};

// Parse the scene and everything it references (events files first, then
// machine files). One structured error, first failure wins.
SceneLoadResult load_scene(const std::string& scene_path, const reflect::Registry& registry);

// ---- instantiation -------------------------------------------------------------
// The authored-name marker component every spawned entity carries
// (registered by spawn_scene as "SceneEntity"): name queries, diagnostics,
// and the golden suite's activity probes hang off this row.
struct SceneEntity {
    base::Name name;
};

struct SpawnStats {
    std::uint32_t entities = 0;       // scene entities spawned
    std::uint32_t machines = 0;       // machine instances
    std::uint32_t bodies = 0;         // physics bodies created
    std::uint32_t state_children = 0; // child entities under states
};

// One state-script seat to bind (the run verb loads these through the TS
// toolchain; full onX-hook parity is proven at m0-appendix-a-determinism).
struct ScriptSeat {
    statechart::MachineId machine = statechart::kInvalidMachine;
    base::Name region;
    base::Name state;
    std::string ref;
    std::string path;
};

// One instantiated machine: id + host, for introspection (in_state
// assertions, the run verb's envelope, next node's golden suite).
struct MachineSeat {
    statechart::MachineId id = statechart::kInvalidMachine;
    base::Name machine;
    base::Name entity;
    ecs::EntityRef host;
};

struct SpawnResult {
    SpawnStats stats;
    std::vector<MachineSeat> machines;
    std::vector<ScriptSeat> scripts;
    std::optional<base::Error> error;
};

[[nodiscard]] bool scene_uses_physics(const SceneFile& scene);

// Registers every declared event into the registry (typed payload schemas —
// the bus validates against them from then on). Collisions with already-
// registered names refuse structurally; call BEFORE the first trigger.
std::optional<base::Error> register_scene_events(const SceneFile& scene,
                                                 reflect::Registry& registry);

// Boot-phase instantiation (one scene per World in M0): registers the
// SceneEntity component, spawns entities in document order (adopt +
// transforms + physics bodies), instantiates machines through the EXISTING
// Statechart::instantiate, spawns children under their state entities, and
// flushes the structural queue. Every spawn journals a FLIGHT "scene.spawn"
// record citing `cause_id`. `physics` may be null iff !scene_uses_physics.
SpawnResult spawn_scene(const SceneFile& scene,
                        ecs::World& world,
                        hierarchy::Hierarchy& hierarchy,
                        statechart::Statechart& chart,
                        physics::PhysicsServer* physics,
                        journal::Writer& journal,
                        std::uint64_t cause_id);

// The spec 4.2 symbolic key vocabulary, resolved at spawn: `self`/`root` ->
// the host's private channel, `global`/<group> -> the shared named channel.
[[nodiscard]] bus::EventKey resolve_key(std::string_view spelling, ecs::EntityRef host);

} // namespace midday::loader

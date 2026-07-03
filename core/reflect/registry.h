// core/reflect/registry.h — the ClassDB-equivalent for an ECS + statechart
// engine (spec section 4.3). Deviation from Godot is deliberate: Midday
// reflects COMPONENT CLASSES, EVENTS with typed payload schemas, and free
// FUNCTIONS (the expression-language inventory) — not an Object hierarchy.
//
// This registry is THE source for engine_api.json (m0-api-json) and every
// generated artifact downstream (engine.d.ts, schema manifest, agent docs).
// Later nodes ONLY ADD entries — the data model here is the contract.
//
// Guarantees:
//   * Deterministic enumeration: init-level-major, then registration order
//     within the level — never pointer, hash, or name order.
//   * Per-entry compat hashes: XXH3-64 over the dump() bytes of a canonical
//     signature-only JSON (docs excluded — doc edits are not API drift).
//     This is the drift-detection primitive `midday api diff` compares.
//   * Descriptor addresses are stable from registration until their init
//     level is torn down (per-level unique_ptr storage) — consumers may
//     cache pointers across the frame, not across teardown.
//   * No exceptions on any path. Duplicate names, malformed descriptors
//     (default not inhabiting its declared type, unknown flag bits, empty
//     or repeated member names, required params after optional ones) abort
//     loudly at registration — like Name collisions (D-BUILD-011).
//   * Registration is boot-path, single-threaded by contract; queries after
//     boot are const and freely shared.

#pragma once

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/reflect/init_levels.h"
#include "core/reflect/type_model.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace midday::reflect {

// Property flags: bits reserved now, semantics land with their subsystems
// (save -> m2 save games, replicated -> networking, read_only -> bindings).
inline constexpr std::uint32_t kPropertySave = 1u << 0;
inline constexpr std::uint32_t kPropertyReplicated = 1u << 1;
inline constexpr std::uint32_t kPropertyReadOnly = 1u << 2;
inline constexpr std::uint32_t kPropertyFlagsMask =
    kPropertySave | kPropertyReplicated | kPropertyReadOnly;

struct PropertyDesc {
    base::Name name;
    TypeDesc type;
    base::Json default_value; // null = no default
    std::uint32_t flags = 0;  // kProperty* bits
    std::string doc;
};

struct ParamDesc {
    base::Name name;
    TypeDesc type;
    base::Json default_value; // null = required parameter
};

// A callable signature: class methods AND free functions (the expression-
// language inventory registers these — one shape, no parallel model).
struct MethodDesc {
    base::Name name;
    std::vector<ParamDesc> params;
    std::optional<TypeDesc> returns; // nullopt = void
    std::string doc;
    std::uint64_t compat_hash = 0; // set at registration (per-method, spec 4.3)
};

struct ClassDesc {
    base::Name name;
    base::Name base; // empty = no base class
    std::string doc;
    std::vector<PropertyDesc> properties;
    std::vector<MethodDesc> methods;
    std::uint64_t compat_hash = 0; // set at registration
};

struct EventFieldDesc {
    base::Name name;
    TypeDesc type;
    std::string doc;
};

// A first-class event definition (spec section 4.2): named, with a TYPED
// payload schema — schema-visible to the validator, agents, and codegen.
struct EventDesc {
    base::Name name; // dotted vocabulary name, e.g. "trigger.entered"
    std::string doc;
    std::vector<EventFieldDesc> payload;
    std::uint64_t compat_hash = 0; // set at registration
};

// A registered entry: the descriptor plus the init level whose lifetime
// owns it (stamped from the registry's active level at registration).
template <typename Desc> struct Registered {
    Desc desc;
    InitLevel level;
};

class Registry {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;
    ~Registry() = default;

    // ---- registration (boot path, single-threaded by contract) ----------
    // Each returns the stored entry (stable address until the active
    // level's teardown). Compat hashes are computed here; `desc.compat_hash`
    // on input is ignored. Aborts loudly on duplicates and malformed
    // descriptors (see file header). Classes, events, and functions are
    // separate namespaces.
    const Registered<ClassDesc>& add_class(ClassDesc desc);
    const Registered<EventDesc>& add_event(EventDesc desc);
    const Registered<MethodDesc>& add_function(MethodDesc desc);

    // ---- lookup ----------------------------------------------------------
    // nullptr when absent (including "not registered YET / no longer" —
    // init-level visibility is exactly registry presence).
    [[nodiscard]] const Registered<ClassDesc>* find_class(base::Name name) const;
    [[nodiscard]] const Registered<EventDesc>* find_event(base::Name name) const;
    [[nodiscard]] const Registered<MethodDesc>* find_function(base::Name name) const;

    // ---- deterministic enumeration --------------------------------------
    // Init-level-major (CORE first), registration order within a level.
    [[nodiscard]] std::vector<const Registered<ClassDesc>*> classes() const;
    [[nodiscard]] std::vector<const Registered<EventDesc>*> events() const;
    [[nodiscard]] std::vector<const Registered<MethodDesc>*> functions() const;

    // The full API description in enumeration order — the seed m0-api-json
    // grows into engine_api.json: {"classes":[...],"events":[...],
    // "functions":[...]}, each entry per describe() below.
    [[nodiscard]] base::Json to_json() const;

    // ---- init-level plumbing (normally driven by Lifecycle) -------------
    void set_active_level(InitLevel level) { active_level_ = level; }

    [[nodiscard]] InitLevel active_level() const { return active_level_; }

    // Drops every entry registered at `level` (their addresses die here);
    // other levels are untouched.
    void remove_level(InitLevel level);

private:
    template <typename Desc> struct Bucket {
        // Per-level storage: enumeration order is structural, teardown is
        // surgical. unique_ptr = stable addresses across later pushes.
        std::array<std::vector<std::unique_ptr<Registered<Desc>>>, kInitLevelCount> by_level;
        // Lookup only — NEVER iterated (unordered order must not leak).
        std::unordered_map<std::uint64_t, const Registered<Desc>*> by_name;
    };

    template <typename Desc>
    const Registered<Desc>& store(Bucket<Desc>& bucket, Desc desc, std::string_view kind_word);

    Bucket<ClassDesc> classes_;
    Bucket<EventDesc> events_;
    Bucket<MethodDesc> functions_;
    InitLevel active_level_ = InitLevel::kCore;
};

// Full JSON description of one registered entry: signature fields plus
// docs plus "compat_hash" (16-digit lowercase hex). Key order is fixed;
// bytes are deterministic (core JSON dump contract).
base::Json describe(const Registered<ClassDesc>& entry);
base::Json describe(const Registered<EventDesc>& entry);
base::Json describe(const Registered<MethodDesc>& entry); // free function

// The generated payload type name codegen derives for an event:
// "trigger.entered" -> "TriggerEntered" (segments split on '.'/'_',
// each capitalized). Pinned here so engine.d.ts and docs agree forever.
std::string event_payload_type_name(std::string_view event_name);

} // namespace midday::reflect

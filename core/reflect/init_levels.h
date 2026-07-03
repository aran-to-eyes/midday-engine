// core/reflect/init_levels.h — module initialization levels (spec section
// 4.3): CORE -> SERVERS -> SCENE -> TOOLS, torn down in reverse. The proven
// Godot shape (register_core_types / MODULE_INITIALIZATION_LEVEL_*), reduced
// to data: subsystems contribute hooks per level; the Lifecycle driver runs
// them in order against one Registry and unwinds them exactly mirrored.
//
// Visibility IS the contract: a symbol registered by a SERVERS hook does not
// exist while only CORE has initialized — the registry holds it only between
// its level's initialize and teardown (pinned by the reflect.init fixtures).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace midday::reflect {

class Registry;

enum class InitLevel : std::uint8_t {
    kCore = 0,    // primitives, built-in event vocabulary, expression functions
    kServers = 1, // rendering/physics/audio/navigation server surfaces
    kScene = 2,   // entity/component/statechart surfaces
    kTools = 3,   // editor & tooling-only surfaces
};

inline constexpr int kInitLevelCount = 4;

// Canonical spelling ("core", "servers", "scene", "tools") — used by
// engine_api.json and the compat-hash signatures.
std::string_view to_string(InitLevel level);

// A subsystem's contribution to one level. `initialize` is required and
// registers that level's symbols; `teardown` is optional (registry entries
// are removed by the driver regardless — teardown is for side state).
struct InitHooks {
    std::function<void(Registry&)> initialize;
    std::function<void(Registry&)> teardown;
};

// Drives one Registry through the level ladder. Boot is single-threaded by
// contract; misuse (hooks added to an already-initialized level, missing
// initialize fn) aborts loudly — never throws.
class Lifecycle {
public:
    explicit Lifecycle(Registry& registry) : registry_(&registry) {}

    // Contribute hooks. Per level, hooks run in the order they were added
    // (and tear down in exactly the reverse order). Pre: `level` is not
    // yet initialized.
    void add_hooks(InitLevel level, InitHooks hooks);

    // Initializes every not-yet-initialized level from CORE through
    // `target`, in order; a level at or below the current one is a no-op.
    // Each level's hooks see the registry's active level already set.
    void initialize_to(InitLevel target);

    // Full reverse unwind: for each initialized level, highest first, run
    // its teardown hooks in reverse order, then drop every registry entry
    // the level registered. Afterwards the ladder may be climbed again.
    void teardown_all();

    [[nodiscard]] bool initialized(InitLevel level) const {
        return static_cast<int>(level) <= highest_initialized_;
    }

private:
    Registry* registry_;
    std::array<std::vector<InitHooks>, kInitLevelCount> hooks_;
    int highest_initialized_ = -1;
};

} // namespace midday::reflect

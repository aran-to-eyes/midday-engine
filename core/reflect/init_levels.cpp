#include "core/reflect/init_levels.h"

#include "core/reflect/fatal.h"
#include "core/reflect/registry.h"

#include <cstddef>
#include <string>
#include <utility>

namespace midday::reflect {

std::string_view to_string(InitLevel level) {
    constexpr std::string_view kNames[] = {"core", "servers", "scene", "tools"};
    return kNames[static_cast<std::size_t>(level)];
}

void Lifecycle::add_hooks(InitLevel level, InitHooks hooks) {
    if (!hooks.initialize)
        detail::fatal("add_hooks(" + std::string(to_string(level)) +
                      "): initialize hook is required");
    if (initialized(level))
        detail::fatal("add_hooks(" + std::string(to_string(level)) +
                      "): level already initialized — the hook would never run");
    hooks_[static_cast<std::size_t>(level)].push_back(std::move(hooks));
}

void Lifecycle::initialize_to(InitLevel target) {
    for (int level = highest_initialized_ + 1; level <= static_cast<int>(target); ++level) {
        registry_->set_active_level(static_cast<InitLevel>(level));
        for (InitHooks& hooks : hooks_[static_cast<std::size_t>(level)])
            hooks.initialize(*registry_);
        highest_initialized_ = level;
    }
}

void Lifecycle::teardown_all() {
    for (int level = highest_initialized_; level >= 0; --level) {
        registry_->set_active_level(static_cast<InitLevel>(level));
        auto& level_hooks = hooks_[static_cast<std::size_t>(level)];
        for (std::size_t i = level_hooks.size(); i-- > 0;)
            if (level_hooks[i].teardown)
                level_hooks[i].teardown(*registry_);
        registry_->remove_level(static_cast<InitLevel>(level));
        highest_initialized_ = level - 1;
    }
    registry_->set_active_level(InitLevel::kCore);
}

} // namespace midday::reflect

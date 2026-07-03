#include "core/expr/env.h"

#include <cstdio>
#include <cstdlib>

namespace midday::expr {

std::uint16_t EnvSpec::declare(std::string_view name, ValueType type) {
    if (name.empty()) {
        std::fprintf(stderr, "midday: fatal: expr: unnamed variable declaration\n");
        std::abort();
    }
    if (find(name) >= 0) {
        std::fprintf(stderr,
                     "midday: fatal: expr: duplicate variable declaration '%.*s'\n",
                     static_cast<int>(name.size()),
                     name.data());
        std::abort();
    }
    vars_.push_back(Var{std::string(name), type});
    return static_cast<std::uint16_t>(vars_.size() - 1);
}

int EnvSpec::find(std::string_view name) const {
    for (std::size_t i = 0; i < vars_.size(); ++i)
        if (vars_[i].name == name)
            return static_cast<int>(i);
    return -1;
}

} // namespace midday::expr

#include "core/input/inject.h"

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"

#include <string>
#include <utility>

namespace midday::input {
namespace {

base::Error tick_mismatch_error(std::uint64_t target, std::uint64_t next) {
    base::Error error;
    error.code = "input.tick_mismatch";
    error.message = "synthetic injection targets tick " + std::to_string(target) +
                    " but the next input phase lands on tick " + std::to_string(next);
    error.details.set("target_tick", static_cast<std::int64_t>(target));
    error.details.set("next_tick", static_cast<std::int64_t>(next));
    return error;
}

} // namespace

std::optional<base::Error> ActionInjector::inject(const RawInput& raw) {
    const bus::EventKey key = bus::EventKey::named(base::Name("global"));
    for (const loader::ActionDesc& action : map_->actions) {
        bool bound = false;
        for (const loader::BindingDesc& binding : action.bindings) {
            if (binding.device == raw.device && binding.control == raw.control) {
                bound = true;
                break;
            }
        }
        if (!bound)
            continue;

        base::Json payload = base::Json::object();
        payload.set("action", action.name);
        payload.set("device", raw.device_index);
        if (raw.edge == RawEdge::kPressed) {
            payload.set("strength", static_cast<double>(raw.strength));
            if (auto error = loop_->inject_input(key, base::Name("action.pressed"), payload))
                return error;
        } else {
            if (auto error = loop_->inject_input(key, base::Name("action.released"), payload))
                return error;
        }
    }
    return std::nullopt;
}

std::optional<base::Error> ActionInjector::inject_at(std::uint64_t target_tick,
                                                     const RawInput& raw) {
    const std::uint64_t next = loop_->current_tick() + 1;
    if (target_tick != next)
        return tick_mismatch_error(target_tick, next);
    return inject(raw);
}

} // namespace midday::input

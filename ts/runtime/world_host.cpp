// ts/runtime/world_host.cpp — see world_host.h for the seam contract this
// implements.

#include "ts/runtime/world_host.h"

#include "core/math/vec.h"
#include "ts/runtime/host_json.h"

#include <cstdint>
#include <string>
#include <utility>

namespace midday::script {

using base::Error;
using base::Json;

namespace {

math::Vec3 vec3_of(const Json& value) {
    math::Vec3 out;
    if (const Json* x = value.find("x"); x != nullptr)
        out.x = static_cast<float>(x->as_double());
    if (const Json* y = value.find("y"); y != nullptr)
        out.y = static_cast<float>(y->as_double());
    if (const Json* z = value.find("z"); z != nullptr)
        out.z = static_cast<float>(z->as_double());
    return out;
}

// The `Record<string, Record<string, unknown>>` override map (path -> a
// property-diff mapping, spec 4.2) crosses the host boundary as a plain JSON
// object already — one OverrideEntry per top-level key, no line/col (a
// runtime override has no authoring file:line to report).
std::vector<loader::OverrideEntry> overrides_of(const Json& value) {
    std::vector<loader::OverrideEntry> out;
    if (!value.is_object())
        return out;
    for (const auto& [path, diff] : value.items())
        out.push_back(loader::OverrideEntry{.path = path, .diff = diff});
    return out;
}

} // namespace

WorldHost::WorldHost(ScriptRuntime& runtime, loader::PrefabSpawner& spawner) : spawner_(&spawner) {
    runtime.register_host_fn(std::string(kSpawnFn),
                             [this](const Json::Array& args) { return spawn(args); });
    runtime.register_host_fn(std::string(kDespawnFn),
                             [this](const Json::Array& args) { return despawn(args); });
}

HostResult WorldHost::spawn(const Json::Array& args) {
    HostResult result;
    if (args.size() != 3 || !args[0].is_string()) {
        result.error = host_bad_args(kSpawnFn, "prefab: string, at, overrides");
        return result;
    }
    const math::Vec3 at = args[1].is_object() ? vec3_of(args[1]) : math::Vec3{};
    const std::vector<loader::OverrideEntry> overrides = overrides_of(args[2]);
    loader::PrefabSpawnResult spawned = spawner_->spawn_prefab(args[0].as_string(), at, overrides);
    if (spawned.error.has_value()) {
        result.error = std::move(spawned.error);
        return result;
    }
    result.value = host_ref_json(spawned.ref);
    return result;
}

HostResult WorldHost::despawn(const Json::Array& args) {
    HostResult result;
    // 2-arg (immediate) or 3-arg (despawn linger, M2 0B track D — ts/lib/
    // component.ts passes `after` ONLY when the caller supplied opts.after,
    // so the legacy arity stays the common path). Validation here is shape
    // only; the VALUE contract (finite, non-negative, representable) is the
    // spawner's own structured refusal — never duplicated at the seam.
    const bool shape_ok = (args.size() == 2 || args.size() == 3) && args[0].is_number() &&
                          args[1].is_number() && (args.size() == 2 || args[2].is_number());
    if (!shape_ok) {
        result.error =
            host_bad_args(kDespawnFn, "index: number, generation: number, after?: number");
        return result;
    }
    const ecs::EntityRef ref = host_entity_arg(args, 0, 1);
    loader::DespawnOptions opts;
    if (args.size() == 3)
        opts.after = args[2].as_double();
    if (auto error = spawner_->despawn(ref, opts))
        result.error = std::move(*error);
    return result;
}

} // namespace midday::script

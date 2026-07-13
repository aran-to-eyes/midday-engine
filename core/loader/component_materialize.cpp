// core/loader/component_materialize.cpp — see component_materialize.h for
// the dispatcher contract this implements.

#include "core/loader/component_materialize.h"

#include "core/hierarchy/hierarchy.h"
#include "core/loader/component_vocab.h"

#include <string>
#include <utility>

namespace midday::loader {

namespace {

base::Error component_error(std::string_view code,
                            std::string_view message,
                            base::Name component,
                            std::string_view where) {
    base::Error error;
    error.code = std::string(code);
    error.message = std::string(message);
    error.details.set("component", component.view());
    if (!where.empty())
        error.details.set("where", where);
    return error;
}

// The generic native-Transform grammar: exactly the scene grammar's
// `Transform: {at: [x, y, z]}` (core/loader/scene_components.cpp), read
// back from the entry's opaque JSON. Strict: only `at`, only a 3-number
// list — anything else refuses (agents deserve refusal over silent
// tolerance, D-BUILD-012).
struct ParsedTransform {
    math::Transform value;
    std::optional<base::Error> error;
};

ParsedTransform parse_generic_transform(const GenericComponentEntry& entry,
                                        std::string_view where) {
    ParsedTransform out;
    for (const auto& [key, value] : entry.fields.items()) {
        if (key != "at") {
            out.error = component_error("component.bad_fields",
                                        "Transform accepts only {at: [x, y, z]} here",
                                        entry.type,
                                        where);
            out.error->details.set("field", key);
            return out;
        }
        const bool tuple = value.is_array() && value.elements().size() == 3 &&
                           value.elements()[0].is_number() && value.elements()[1].is_number() &&
                           value.elements()[2].is_number();
        if (!tuple) {
            out.error = component_error("component.bad_fields",
                                        "Transform `at` must be a [x, y, z] number list",
                                        entry.type,
                                        where);
            return out;
        }
        out.value.translation = math::Vec3{static_cast<float>(value.elements()[0].as_double()),
                                           static_cast<float>(value.elements()[1].as_double()),
                                           static_cast<float>(value.elements()[2].as_double())};
    }
    return out;
}

} // namespace

std::optional<base::Error> materialize_base_component(hierarchy::Hierarchy& hierarchy,
                                                      const SpawnOptions& options,
                                                      ecs::EntityRef entity,
                                                      const GenericComponentEntry& entry,
                                                      std::uint64_t cause_id,
                                                      bool allow_native_transform) {
    if (is_native_component(entry.type.view())) {
        if (entry.type.view() != "Transform" || !allow_native_transform)
            return component_error(
                "component.native_unsupported",
                "this native component has no materializer on the generic path yet "
                "(scene inline `components:` keeps full native coverage)",
                entry.type,
                {});
        ParsedTransform parsed = parse_generic_transform(entry, {});
        if (parsed.error.has_value())
            return parsed.error;
        if (auto error = hierarchy.set_local(entity, parsed.value))
            return error;
        // entity.get(Transform) must read the placed value (D6's base
        // Transform read): mirror into the script directory when one exists.
        if (options.scripts != nullptr)
            return options.scripts->mirror_native_transform(entity, parsed.value, cause_id);
        return std::nullopt;
    }
    if (options.scripts == nullptr)
        return component_error("component.no_materializer",
                               "a script component was authored but no script component "
                               "materializer is wired (run needs --components / a component host)",
                               entry.type,
                               {});
    return options.scripts->materialize_base(entity, entry, cause_id);
}

std::optional<base::Error>
materialize_entity_base(hierarchy::Hierarchy& hierarchy,
                        const SpawnOptions& options,
                        ecs::EntityRef entity,
                        const std::vector<GenericComponentEntry>& components,
                        std::uint64_t cause_id,
                        bool allow_native_transform) {
    for (const GenericComponentEntry& entry : components)
        if (auto error = materialize_base_component(
                hierarchy, options, entity, entry, cause_id, allow_native_transform))
            return error;
    return std::nullopt;
}

namespace {

// Depth-first over a state and its substates, document order — the flatten
// order instantiate.cpp uses, so attach order matches state-table order.
std::optional<base::Error> materialize_state_recursive(statechart::Statechart& chart,
                                                       const SpawnOptions& options,
                                                       statechart::MachineId machine,
                                                       base::Name region,
                                                       const statechart::StateDesc& state,
                                                       ecs::EntityRef host,
                                                       std::uint64_t cause_id) {
    for (const statechart::StateComponentDesc& desc : state.components) {
        const std::string where = std::string(region.view()) + "/" + std::string(state.name.view());
        if (is_native_component(desc.type.view()))
            return component_error("component.native_unsupported",
                                   "native components cannot be state-scoped in 0B",
                                   desc.type,
                                   where);
        if (auto error = options.scripts->materialize_state(
                chart, machine, region, state.name, host, desc, cause_id))
            return error;
    }
    for (const statechart::StateDesc& substate : state.substates)
        if (auto error = materialize_state_recursive(
                chart, options, machine, region, substate, host, cause_id))
            return error;
    return std::nullopt;
}

bool any_state_components(const statechart::StateDesc& state) {
    if (!state.components.empty())
        return true;
    for (const statechart::StateDesc& substate : state.substates)
        if (any_state_components(substate))
            return true;
    return false;
}

} // namespace

std::optional<base::Error> materialize_machine_components(statechart::Statechart& chart,
                                                          const SpawnOptions& options,
                                                          statechart::MachineId machine,
                                                          const statechart::MachineDesc& desc,
                                                          ecs::EntityRef host,
                                                          std::uint64_t cause_id) {
    if (!machine_has_state_components(desc))
        return std::nullopt; // component-free machines pay one scan
    if (options.scripts == nullptr)
        return component_error("component.no_materializer",
                               "this machine's states own components but no script component "
                               "materializer is wired",
                               desc.name,
                               {});
    if (!options.defer_initial_entry)
        return component_error("component.requires_deferred_entry",
                               "state components must seat BEFORE initial entry: instantiate with "
                               "defer_initial_entry and start entries after seating (the D2 split)",
                               desc.name,
                               {});
    for (const statechart::RegionDesc& region : desc.regions)
        for (const statechart::StateDesc& state : region.states)
            if (auto error = materialize_state_recursive(
                    chart, options, machine, region.name, state, host, cause_id))
                return error;
    return std::nullopt;
}

bool machine_has_state_components(const statechart::MachineDesc& desc) {
    for (const statechart::RegionDesc& region : desc.regions)
        for (const statechart::StateDesc& state : region.states)
            if (any_state_components(state))
                return true;
    return false;
}

std::optional<base::Error>
mirror_seeded_transform(const SpawnOptions& options,
                        ecs::EntityRef entity,
                        const math::Transform& value,
                        const std::vector<GenericComponentEntry>& components,
                        bool state_components,
                        std::uint64_t cause_id) {
    if (options.scripts == nullptr)
        return std::nullopt;
    bool hosts_scripts = state_components;
    for (const GenericComponentEntry& entry : components) {
        if (entry.type.view() == "Transform")
            return std::nullopt; // the generic entry re-places and mirrors itself
        hosts_scripts = hosts_scripts || !is_native_component(entry.type.view());
    }
    if (!hosts_scripts)
        return std::nullopt; // no script component ever reads it: no record
    return options.scripts->mirror_native_transform(entity, value, cause_id);
}

} // namespace midday::loader

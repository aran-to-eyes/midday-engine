// core/loader/prefab_instantiate.cpp — see prefab_instantiate.h for the
// header contract this implements.

#include "core/loader/prefab_instantiate.h"

#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"

#include <string>
#include <utility>

namespace midday::loader {

namespace {

std::string entity_form(ecs::EntityRef ref) {
    return "entity:" + std::to_string(ref.index) + "#" + std::to_string(ref.generation);
}

base::Error journal_refused_error(const journal::Writer& journal) {
    base::Error error{.code = "prefab.journal_refused",
                      .message = "the journal refused the prefab.spawn record"};
    const std::optional<base::Error>& status = journal.status();
    if (status.has_value())
        error.details.set("journal", status->to_json());
    return error;
}

} // namespace

ResolvedMachinesResult resolve_prefab_machines(const EntityFile& entity_file,
                                               const std::vector<OverrideEntry>& overrides,
                                               std::string_view origin_file) {
    ResolvedMachinesResult out;
    out.machines.reserve(entity_file.machines.size());
    for (const EntityMachineInstance& instance : entity_file.machines) {
        const MachineFile& base = entity_file.machine_files.at(instance.machine_index);
        const SplitOverrides split = split_overrides_for_machine(overrides, base.desc.name.view());
        std::vector<OverrideEntry> all = instance.overrides;
        all.insert(all.end(), split.matched.begin(), split.matched.end());
        if (all.empty()) {
            out.machines.push_back(base);
            continue;
        }
        ApplyOverridesResult applied = apply_overrides(base, all, origin_file);
        if (applied.error.has_value()) {
            out.error = std::move(applied.error);
            out.machines.clear();
            return out;
        }
        out.machines.push_back(std::move(applied.machine));
    }
    return out;
}

MaterializeResult materialize_prefab(ecs::World& world,
                                     hierarchy::Hierarchy& hierarchy,
                                     statechart::Statechart& chart,
                                     journal::Writer& journal,
                                     std::uint64_t tick,
                                     const std::vector<MachineFile>& machines,
                                     ecs::EntityRef root,
                                     std::string_view prefab_path,
                                     std::uint64_t cause_id) {
    MaterializeResult out;

    base::Json root_payload = base::Json::object();
    root_payload.set("prefab", std::string(prefab_path));
    root_payload.set("entity", entity_form(root));
    const std::uint64_t record_id = journal.record(
        tick, journal::Tier::Flight, "prefab.spawn", cause_id, std::move(root_payload));
    if (record_id == 0) {
        out.error = journal_refused_error(journal);
        return out;
    }

    for (const MachineFile& machine : machines) {
        const statechart::InstantiateResult instantiated =
            chart.instantiate(machine.desc, root, record_id);
        if (instantiated.error.has_value()) {
            out.error = instantiated.error;
            return out;
        }
        out.machines.push_back(MaterializedMachine{
            .id = instantiated.machine, .machine = machine.desc.name, .scripts = machine.scripts});

        for (const StateChildren& children : machine.children) {
            const ecs::EntityRef state_entity =
                chart.state_entity(instantiated.machine, children.region, children.state);
            if (state_entity.is_null())
                continue; // defensive: the loader already guarantees this resolves

            for (const StateChildDesc& child : children.children) {
                base::Error child_error;
                const ecs::EntityRef child_ref = world.spawn(&child_error);
                if (child_ref.is_null()) {
                    out.error = child_error;
                    return out;
                }
                if (auto error = hierarchy.queue_attach(child_ref, state_entity)) {
                    out.error = std::move(error);
                    return out;
                }
                base::Json child_payload = base::Json::object();
                child_payload.set("prefab", std::string(prefab_path));
                child_payload.set("entity", entity_form(child_ref));
                child_payload.set("under_state", children.state.view());
                journal.record(tick,
                               journal::Tier::Flight,
                               "prefab.spawn",
                               record_id,
                               std::move(child_payload));
                out.children.push_back(PrefabChild{child_ref, child.at});
            }
        }
    }
    return out;
}

} // namespace midday::loader

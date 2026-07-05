// `midday scene print <file> [--full] [--components <manifest>]` —
// m1-scene-format's inspection verb: extension-dispatch over `*.scene.yaml`
// / `*.machine.yaml` / `*.entity.yaml` (the validate.cpp precedent), always
// LENIENT (core/loader/gaps.h) — a component type or asset this engine does
// not implement yet is REPORTED, never a reason to refuse or pretend the
// file is complete (spec's honesty contract, exit-test #5). Grammar/
// override-path mistakes still refuse loudly (exit 3): leniency is reserved
// for content-not-implemented-yet, never for authoring bugs.
//
// Default (no --full): the canonical strict-YAML text (`midday fmt`'s own
// mechanism, schema-agnostic) — proves the file parses through the SCENE-
// AWARE loader (catching semantic errors `fmt` alone would not) before
// printing.
// `--full`: the EFFECTIVE view (spec 369 "full-state visibility").
//   * `*.machine.yaml`: the canonical MachineFile re-serialized
//     (core/loader/machine_emit.h) — `on:` desugars to `Transition:`,
//     `history:`/`priority:` render explicitly. Round-trip-stable
//     (exit-test #1): re-running `scene print --full` against THIS output
//     reproduces it byte-for-byte (the loader accepts `Transition:` as
//     `on:`'s exact equivalent).
//   * `*.entity.yaml` / `*.scene.yaml`: the raw canonical text, PLUS an
//     `overrides` report per machine instance — each override path
//     resolved BY NAME (exit-test #2) and, when it resolves cleanly, the
//     resulting machine's OWN canonical form (`effective_yaml`, the same
//     machine_emit.h mechanism) so the overridden field is visible
//     verbatim (`duration: 1`, `amount: 55`, ...).
// `gaps`: every unresolved component type / script / prefab / model /
// attachment reference found anywhere in the file (and, for scene/entity
// files, everything their resolved prefabs/machines reference too).

#include "cli/verb.h"
#include "core/loader/component_vocab.h"
#include "core/loader/loader.h"
#include "core/loader/machine_emit.h"
#include "core/loader/override.h"
#include "core/loader/yaml.h"
#include "core/loader/yaml_emit.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::cli {
namespace {

constexpr std::string_view kSceneSuffix = ".scene.yaml";
constexpr std::string_view kMachineSuffix = ".machine.yaml";
constexpr std::string_view kEntitySuffix = ".entity.yaml";

VerbOutcome usage(std::string code, std::string message) {
    VerbOutcome out;
    out.exit = Exit::Usage;
    out.error = Error{.code = std::move(code), .message = std::move(message)};
    return out;
}

VerbOutcome refuse(Error error) {
    VerbOutcome out;
    out.exit = Exit::Validation;
    out.error = std::move(error);
    return out;
}

std::string root_dir_of(const std::string& path) {
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    return parent.empty() ? std::string(".") : parent.generic_string();
}

Json gap_json(const loader::Gap& gap) {
    return gap.to_json();
}

Json gaps_json(const std::vector<loader::Gap>& gaps) {
    Json out = Json::array();
    for (const loader::Gap& gap : gaps)
        out.push(gap_json(gap));
    return out;
}

// The raw canonical text (`midday fmt`'s mechanism) — used for every
// non-full print, and as the base text for a full scene/entity print.
Json read_raw_canonical(const std::string& path, std::optional<VerbOutcome>& failure) {
    loader::YamlParseResult parsed = loader::parse_yaml_file(path);
    if (parsed.error.has_value()) {
        failure = refuse(std::move(*parsed.error));
        return {};
    }
    return {loader::emit_yaml(parsed.root)};
}

Json asset_ref_json(const loader::AssetRefDesc& ref) {
    Json out = Json::object();
    if (ref.has_uid)
        out.set("uid", ref.uid);
    out.set("path", ref.path_authored);
    out.set("exists", ref.exists);
    return out;
}

// Applies `overrides` onto `entity.machine_files[instance.machine_index]`
// and reports the outcome: `overrides` (the raw path list), and — when
// they resolve cleanly — `effective_yaml`, the resulting machine's own
// canonical form (machine_emit.h), so an overridden field (duration,
// amount, ...) is visible verbatim. A bad override path stays a hard
// refusal (never silently dropped from the report).
struct MachineReportResult {
    Json json;
    std::optional<Error> error;
};

MachineReportResult
report_machine_instance(const loader::EntityFile& entity,
                        const loader::EntityMachineInstance& instance,
                        const std::vector<loader::OverrideEntry>& scene_overrides,
                        std::string_view origin_file) {
    MachineReportResult out;
    const loader::MachineFile& base = entity.machine_files.at(instance.machine_index);
    Json entry = Json::object();
    entry.set("instance", asset_ref_json(instance.instance_ref));
    entry.set("machine", base.desc.name.view());

    std::vector<loader::OverrideEntry> all_overrides = instance.overrides;
    all_overrides.insert(all_overrides.end(), scene_overrides.begin(), scene_overrides.end());
    Json override_paths = Json::array();
    for (const loader::OverrideEntry& entry_override : all_overrides)
        override_paths.push(entry_override.path);
    entry.set("override_paths", std::move(override_paths));

    if (!all_overrides.empty()) {
        loader::ApplyOverridesResult applied =
            loader::apply_overrides(base, all_overrides, origin_file);
        if (applied.error.has_value()) {
            out.error = std::move(applied.error);
            return out;
        }
        entry.set("effective_yaml", loader::emit_yaml(loader::machine_to_yaml(applied.machine)));
    }
    out.json = std::move(entry);
    return out;
}

// Every machine instance an entity/prefab file owns, plus `extra_overrides`
// scoped to that SAME entity by a scene-level `prefab: ... override:` block
// (empty for a standalone `*.entity.yaml` print).
struct EntityReportResult {
    Json machines = Json::array();
    std::optional<Error> error;
};

EntityReportResult
report_entity_machines(const loader::EntityFile& entity,
                       const std::vector<loader::OverrideEntry>& extra_overrides) {
    EntityReportResult out;
    for (const loader::EntityMachineInstance& instance : entity.machines) {
        const loader::MachineFile& base = entity.machine_files.at(instance.machine_index);
        loader::SplitOverrides split =
            loader::split_overrides_for_machine(extra_overrides, base.desc.name.view());
        MachineReportResult reported =
            report_machine_instance(entity, instance, split.matched, entity.path);
        if (reported.error.has_value()) {
            out.error = std::move(reported.error);
            return out;
        }
        out.machines.push(std::move(reported.json));
    }
    return out;
}

VerbOutcome print_machine(const std::string& path, bool full, const loader::ComponentVocab& vocab) {
    const std::string root = root_dir_of(path);
    reflect::Registry registry;
    reflect::register_builtin_events(registry);
    loader::ProjectEventsResult project = loader::load_project_events(root, registry);
    if (project.error.has_value())
        return refuse(std::move(*project.error));

    loader::MachineLoadResult loaded =
        loader::load_machine_file(path, root, registry, project.decl, vocab, /*lenient=*/true);
    if (loaded.error.has_value())
        return refuse(std::move(*loaded.error));
    if (!loaded.machine.has_value()) // defensive: loaders return one or the other
        return refuse(Error{.code = "loader.bad_ref", .message = "machine load failed"});
    const loader::MachineFile& machine = *loaded.machine;

    VerbOutcome out;
    out.payload.set("file", path);
    out.payload.set("kind", "machine");
    out.payload.set("gaps", gaps_json(machine.gaps));
    if (full) {
        const std::string canonical = loader::emit_yaml(loader::machine_to_yaml(machine));
        out.payload.set("yaml", canonical);
        out.human = canonical;
    } else {
        std::optional<VerbOutcome> failure;
        Json canonical = read_raw_canonical(path, failure);
        if (failure.has_value())
            return std::move(*failure);
        out.payload.set("yaml", canonical);
        out.human = canonical.as_string();
    }
    return out;
}

VerbOutcome print_entity(const std::string& path, bool full, const loader::ComponentVocab& vocab) {
    const std::string root = root_dir_of(path);
    reflect::Registry registry;
    reflect::register_builtin_events(registry);
    loader::ProjectEventsResult project = loader::load_project_events(root, registry);
    if (project.error.has_value())
        return refuse(std::move(*project.error));

    loader::EntityLoadResult loaded =
        loader::load_entity_file(path, registry, project.decl, vocab, /*lenient=*/true);
    if (loaded.error.has_value())
        return refuse(std::move(*loaded.error));
    if (!loaded.entity.has_value()) // defensive: loaders return one or the other
        return refuse(Error{.code = "loader.bad_ref", .message = "entity load failed"});
    const loader::EntityFile& entity_file = *loaded.entity;

    VerbOutcome out;
    out.payload.set("file", path);
    out.payload.set("kind", "entity");
    out.payload.set("gaps", gaps_json(entity_file.gaps));
    std::optional<VerbOutcome> failure;
    Json canonical = read_raw_canonical(path, failure);
    if (failure.has_value())
        return std::move(*failure);
    out.payload.set("yaml", canonical);
    out.human = canonical.as_string();
    if (full) {
        EntityReportResult report = report_entity_machines(entity_file, {});
        if (report.error.has_value())
            return refuse(std::move(*report.error));
        out.payload.set("machines", std::move(report.machines));
    }
    return out;
}

VerbOutcome print_scene(const std::string& path, bool full, const loader::ComponentVocab& vocab) {
    reflect::Registry registry;
    reflect::register_builtin_events(registry);
    loader::SceneLoadResult loaded = loader::load_scene(path, registry, /*lenient=*/true, vocab);
    if (loaded.error.has_value())
        return refuse(std::move(*loaded.error));
    if (!loaded.scene.has_value()) // defensive: loaders return one or the other
        return refuse(Error{.code = "loader.bad_ref", .message = "scene load failed"});
    const loader::SceneFile& scene = *loaded.scene;

    VerbOutcome out;
    out.payload.set("file", path);
    out.payload.set("kind", "scene");
    out.payload.set("gaps", gaps_json(scene.gaps));
    std::optional<VerbOutcome> failure;
    Json canonical = read_raw_canonical(path, failure);
    if (failure.has_value())
        return std::move(*failure);
    out.payload.set("yaml", canonical);
    out.human = canonical.as_string();

    if (full) {
        Json entities = Json::array();
        for (const loader::SceneEntityDesc& entity : scene.entities) {
            if (entity.kind != loader::SceneEntityKind::kPrefab || !entity.prefab.has_value())
                continue;
            const loader::PrefabInstanceDesc& prefab = *entity.prefab;
            Json entry = Json::object();
            entry.set("entity", entity.name.view());
            entry.set("prefab", asset_ref_json(prefab.prefab_ref));
            entry.set("resolved", prefab.resolved);
            if (prefab.resolved) {
                const loader::EntityFile& resolved_entity =
                    scene.entity_prefabs.at(prefab.entity_index);
                EntityReportResult report =
                    report_entity_machines(resolved_entity, prefab.overrides);
                if (report.error.has_value())
                    return refuse(std::move(*report.error));
                entry.set("machines", std::move(report.machines));
            }
            entities.push(std::move(entry));
        }
        out.payload.set("prefab_entities", std::move(entities));
    }
    return out;
}

VerbOutcome scene_verb(const VerbArgs& args) {
    const std::string& op = args.get_string("op");
    if (op != "print") {
        return usage("usage.unknown_op", "unknown scene operation '" + op + "' (available: print)");
    }
    const std::string& path = args.get_string("file");
    const bool full = args.get_bool("full");

    loader::ComponentVocab vocab;
    if (args.present("components")) {
        loader::ComponentVocabLoadResult loaded_vocab =
            loader::load_component_vocab(args.get_string("components"));
        if (loaded_vocab.error.has_value())
            return refuse(std::move(*loaded_vocab.error));
        vocab = std::move(loaded_vocab.vocab);
    }

    if (path.ends_with(kMachineSuffix))
        return print_machine(path, full, vocab);
    if (path.ends_with(kEntitySuffix))
        return print_entity(path, full, vocab);
    if (path.ends_with(kSceneSuffix))
        return print_scene(path, full, vocab);
    return usage("usage.unknown_extension",
                 path + ": expected a *.scene.yaml / *.machine.yaml / *.entity.yaml file");
}

constexpr FlagSpec kFlags[] = {
    {.name = "full",
     .type = "bool",
     .doc = "print the EFFECTIVE state: on: desugars to Transition:, defaults filled in, "
            "overrides resolved and applied"},
    {.name = "components",
     .type = "string",
     .doc = "a project component-schema manifest (`midday script extract --out`) — extends the "
            "native vocabulary a component name is checked against"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "op", .type = "string", .doc = "operation: print"},
    {.name = "file",
     .type = "string",
     .doc = "a *.scene.yaml / *.machine.yaml / *.entity.yaml file"},
};

} // namespace

const VerbSpec& scene_spec() {
    static const VerbSpec spec{
        .name = "scene",
        .summary = "inspect a scene/machine/entity file: canonical text, effective state, gaps",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &scene_verb,
    };
    return spec;
}

} // namespace midday::cli

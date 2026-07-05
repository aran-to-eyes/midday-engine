// `midday new <dir> [--name <string>]` — project scaffolding (m1-project-
// new, spec section 9's CLI lifecycle line 394: `midday new|run|build|
// import|export`). Creates a fresh, CI-ready project on disk:
//
//   <dir>/midday.project.yaml   project config (core/loader/project_schema.h)
//   <dir>/midday.import.yaml    the asset import policy (glob-scoped defaults)
//   <dir>/default.input.yaml    a minimal, conflict-free action map (m1-input-actions)
//   <dir>/scenes/main.scene.yaml   the first scene: `format: 1` + `scene: main`,
//                                  no entities — the M0 loader's smallest legal document
//   <dir>/scripts/, <dir>/assets/  empty, ready for TS state scripts / imported content
//   <dir>/.gitignore             ignores .midday-cache/ — nothing regenerable is ever
//                                 committed (spec line 371's "no editor-bookkeeping
//                                 fields in agent files; anything regenerable lives in
//                                 a cache dir" — this scaffold WRITES no cache at all)
//
// Every generated strict-YAML file is built as a YamlNode tree
// (core/loader/yaml_build.h) and serialized through the canonical emitter
// (core/loader/yaml_emit.h) — born canonical, never a hand-indented string
// template. Before anything touches disk, the project config and import
// policy trees are validated in memory against their schemas
// (core/loader/project_schema.h, the SAME generic engine `midday validate`
// runs); after the input map and scene land on disk, each is re-loaded
// through its OWN real loader (core/loader/loader.h's load_input_file +
// find_conflict; core/loader/loader.h's load_scene, the M0 subset — never
// scene-format's schema, which does not exist yet). A self-check failure
// here can only be this generator's own bug (every byte is code-generated,
// never a function of untrusted input), so it is reported structurally
// (Exit::Failure) rather than silently accepted.
//
// PROJECT ROOT, defined for the first time (loader.h and formats/
// loader_yaml.md both flagged their "project root = the file's own
// directory" convention as provisional "until m1-project-new defines a
// real one"): the project root IS the directory containing
// `midday.project.yaml`. The input map is scaffolded directly AT that
// root, not under a subdirectory — every existing *.input.yaml/
// *.events.yaml consumer (validate.cpp's extension dispatch, load_project_
// input/events) already treats "the target file's own directory" as its
// scan root, so this placement makes the existing convention and the real
// project root COINCIDE for free; no consumer changes were needed. The
// scaffolded scene keeps the UNRELATED M0 scene convention (project root =
// the scene file's own directory, D-BUILD-077) — irrelevant here since the
// scaffolded scene carries no events/machine refs to resolve.
//
// CI-friendly defaults: every path is project-root-relative (never
// absolute), `seed: 0` and `physics_fixed_hz: 60` pin the same values the
// engine already defaults to (core/tick/tick_loop.h, core/physics/
// physics_server.h) so nothing here contradicts runtime behavior, and the
// scene is headless-runnable the moment it exists. No verb yet READS
// `midday.project.yaml` at runtime (`midday run` still takes a scene path
// directly) — wiring a project-aware `run`/`build` is future work this
// node does not claim to do; the file exists so that work has real data to
// consume, per spec lines 306/508/606 ("visible in the project file").

#include "cli/verb.h"
#include "core/base/file_io.h"
#include "core/loader/format_schema.h"
#include "core/loader/loader.h"
#include "core/loader/project_schema.h"
#include "core/loader/yaml_build.h"
#include "core/loader/yaml_emit.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::cli {
namespace {

constexpr std::string_view kProjectConfigName = "midday.project.yaml";
constexpr std::string_view kImportPolicyName = "midday.import.yaml";
constexpr std::string_view kInputMapName = "default.input.yaml";
constexpr std::string_view kScenesDir = "scenes";
constexpr std::string_view kMainSceneName = "main.scene.yaml";
constexpr std::string_view kScriptsDir = "scripts";
constexpr std::string_view kAssetsDir = "assets";
constexpr std::string_view kGitignoreName = ".gitignore";
constexpr std::string_view kGitignoreBody = ".midday-cache/\n";

VerbOutcome fail(Exit exit, Error error) {
    VerbOutcome out;
    out.exit = exit;
    out.error = std::move(error);
    return out;
}

// Every existing tree-scanning consumer normalizes to forward-slash,
// lexically-normal paths (mv.cpp's absolute_generic precedent) — the
// scaffold's own payload and the paths it writes INTO generated files hold
// to the same rule (D-BUILD-113: native-vs-generic broke Windows once).
std::string generic(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

std::string default_project_name(const std::filesystem::path& dir) {
    const std::string name = dir.lexically_normal().filename().generic_string();
    return (name.empty() || name == ".") ? std::string("project") : name;
}

// ---- scaffold content, built as YamlNode trees (never a text template) ----

using loader::make_entry;
using loader::make_map;
using loader::make_scalar;
using loader::make_seq;
using loader::YamlEntry;
using loader::YamlNode;

// `name` is unconstrained free-form text (a --name flag or a directory's
// basename): unlike every other scalar this verb writes, its safety as a
// plain YAML scalar is NOT hand-verified, so it is always double-quoted —
// the one field a hand-built tree cannot treat like yaml_emit.h's parsed-
// node fast path (core/loader/yaml_build.h's header comment).
YamlNode build_project_config(const std::string& name) {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("format", make_scalar("1")));
    entries.push_back(make_entry("name", make_scalar(name, /*quoted=*/true)));
    entries.push_back(make_entry(
        "main_scene", make_scalar(std::string(kScenesDir) + "/" + std::string(kMainSceneName))));
    entries.push_back(make_entry("input_map", make_scalar(std::string(kInputMapName))));
    entries.push_back(make_entry("collision_layers", make_seq({make_scalar("default")})));
    entries.push_back(make_entry(
        "physics_gravity", make_seq({make_scalar("0"), make_scalar("-9.81"), make_scalar("0")})));
    entries.push_back(make_entry("physics_fixed_hz", make_scalar("60")));
    entries.push_back(make_entry("seed", make_scalar("0")));
    return make_map(std::move(entries));
}

YamlNode build_import_policy() {
    auto rule = [](std::string_view glob, std::string_view kind) {
        return make_entry(std::string(glob), make_scalar(std::string(kind)));
    };
    std::vector<YamlEntry> rules;
    rules.push_back(rule("**/*.gltf", "mesh"));
    rules.push_back(rule("**/*.glb", "mesh"));
    rules.push_back(rule("**/*.png", "texture"));
    rules.push_back(rule("**/*.jpg", "texture"));
    rules.push_back(rule("**/*.jpeg", "texture"));
    rules.push_back(rule("**/*.wav", "audio"));
    rules.push_back(rule("**/*.ogg", "audio"));

    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("format", make_scalar("1")));
    entries.push_back(make_entry("default_import", make_scalar("raw")));
    entries.push_back(make_entry("rules", make_map(std::move(rules))));
    return make_map(std::move(entries));
}

YamlNode build_binding(std::string_view device, std::string_view control) {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("device", make_scalar(std::string(device))));
    entries.push_back(make_entry("control", make_scalar(std::string(control))));
    return make_map(std::move(entries));
}

YamlNode build_action(std::vector<YamlNode> bindings) {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("bindings", make_seq(std::move(bindings))));
    return make_map(std::move(entries));
}

// A minimal, conflict-free WASD + jump map (formats/loader_yaml.md's own
// documented example shape) — every (device, control) pair below is
// distinct, so load_project_input's cross-file conflict scan is trivially
// clean the moment a project is born.
YamlNode build_input_map() {
    std::vector<YamlEntry> actions;
    actions.push_back(make_entry("move_up", build_action({build_binding("keyboard", "w")})));
    actions.push_back(make_entry("move_down", build_action({build_binding("keyboard", "s")})));
    actions.push_back(make_entry("move_left", build_action({build_binding("keyboard", "a")})));
    actions.push_back(make_entry("move_right", build_action({build_binding("keyboard", "d")})));
    actions.push_back(make_entry("jump",
                                 build_action({build_binding("keyboard", "space"),
                                               build_binding("gamepad", "button_south")})));

    std::vector<YamlEntry> stick_fields;
    stick_fields.push_back(make_entry("up", make_scalar("move_up")));
    stick_fields.push_back(make_entry("down", make_scalar("move_down")));
    stick_fields.push_back(make_entry("left", make_scalar("move_left")));
    stick_fields.push_back(make_entry("right", make_scalar("move_right")));
    std::vector<YamlEntry> sticks;
    sticks.push_back(make_entry("move", make_map(std::move(stick_fields))));

    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("format", make_scalar("1")));
    entries.push_back(make_entry("actions", make_map(std::move(actions))));
    entries.push_back(make_entry("sticks", make_map(std::move(sticks))));
    return make_map(std::move(entries));
}

// The M0 loader's smallest legal document (core/loader/scene_load.cpp):
// `events:`/`entities:` are both optional, so a genuinely EMPTY scene is
// exactly these two keys — the deliverable's literal "first empty scene",
// not a placeholder entity invented for this node.
YamlNode build_empty_scene() {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("format", make_scalar("1")));
    entries.push_back(make_entry("scene", make_scalar("main")));
    return make_map(std::move(entries));
}

// ---- the verb ---------------------------------------------------------

VerbOutcome new_verb(const VerbArgs& args) {
    const std::filesystem::path root(args.get_string("dir"));
    const std::string root_generic = generic(root);

    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        if (!std::filesystem::is_directory(root, ec))
            return fail(Exit::Failure,
                        Error{.code = "new.target_exists",
                              .message = root_generic + " already exists and is not a directory"});
        if (!std::filesystem::is_empty(root, ec))
            return fail(Exit::Failure,
                        Error{.code = "new.target_exists",
                              .message = root_generic + " already exists and is not empty"});
    }

    const std::string name =
        args.present("name") ? args.get_string("name") : default_project_name(root);

    // Validate the config/policy trees BEFORE anything touches disk: a
    // failure here is this generator's own bug (the content is entirely
    // code-generated), so refuse loudly instead of writing a broken
    // scaffold — nothing has been written yet at this point.
    const YamlNode project_config = build_project_config(name);
    loader::ValidateResult project_check = loader::validate_document(
        loader::project_config_schema(), project_config, kProjectConfigName);
    if (project_check.error.has_value())
        return fail(Exit::Failure, std::move(*project_check.error));

    const YamlNode import_policy = build_import_policy();
    loader::ValidateResult import_check =
        loader::validate_document(loader::import_policy_schema(), import_policy, kImportPolicyName);
    if (import_check.error.has_value())
        return fail(Exit::Failure, std::move(*import_check.error));

    for (const std::filesystem::path& sub :
         {root, root / kScenesDir, root / kScriptsDir, root / kAssetsDir}) {
        std::filesystem::create_directories(sub, ec);
        if (ec)
            return fail(Exit::Failure,
                        base::file_error("new.io", "cannot create directory " + generic(sub)));
    }

    const std::string project_config_path = generic(root / kProjectConfigName);
    const std::string import_policy_path = generic(root / kImportPolicyName);
    const std::string input_map_path = generic(root / kInputMapName);
    const std::string scene_path = generic(root / kScenesDir / kMainSceneName);
    const std::string gitignore_path = generic(root / kGitignoreName);

    if (auto error =
            base::write_file(project_config_path, loader::emit_yaml(project_config), "new.io"))
        return fail(Exit::Failure, std::move(*error));
    if (auto error =
            base::write_file(import_policy_path, loader::emit_yaml(import_policy), "new.io"))
        return fail(Exit::Failure, std::move(*error));
    if (auto error =
            base::write_file(input_map_path, loader::emit_yaml(build_input_map()), "new.io"))
        return fail(Exit::Failure, std::move(*error));
    if (auto error = base::write_file(scene_path, loader::emit_yaml(build_empty_scene()), "new.io"))
        return fail(Exit::Failure, std::move(*error));
    if (auto error = base::write_file(gitignore_path, kGitignoreBody, "new.io"))
        return fail(Exit::Failure, std::move(*error));

    // Post-write self-checks through the REAL loaders (never re-derived
    // logic): the input map has no in-memory validator (its grammar lives
    // behind a file path, core/loader/loader.h), and the scene deliberately
    // rides the M0 loader rather than a schema this node has no business
    // owning (scope: "do NOT depend on scene-format's schema").
    loader::ActionMapDecl input_decl;
    if (auto error = loader::load_input_file(input_map_path, input_decl))
        return fail(Exit::Failure, std::move(*error));
    if (auto error = loader::find_conflict(input_decl.actions))
        return fail(Exit::Failure, std::move(*error));

    reflect::Registry registry;
    reflect::register_builtin_events(registry);
    loader::SceneLoadResult scene_check = loader::load_scene(scene_path, registry);
    if (scene_check.error.has_value())
        return fail(Exit::Failure, std::move(*scene_check.error));

    VerbOutcome out;
    out.payload.set("dir", root_generic);
    out.payload.set("name", name);
    Json files = Json::array();
    for (const std::string& file :
         {project_config_path, import_policy_path, input_map_path, scene_path, gitignore_path})
        files.push(file);
    out.payload.set("files", std::move(files));
    out.human = "new project '" + name + "' -> " + root_generic + " (5 files, 3 directories)";
    return out;
}

constexpr FlagSpec kFlags[] = {
    {.name = "name",
     .type = "string",
     .doc = "project display name (default: the target directory's own name)"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "dir",
     .type = "string",
     .doc = "target directory for the new project (must not exist, or be empty)"},
};

} // namespace

const VerbSpec& new_spec() {
    static const VerbSpec spec{
        .name = "new",
        .summary =
            "scaffold a fresh project: config, import policy, input map, and a first empty scene",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &new_verb,
    };
    return spec;
}

} // namespace midday::cli

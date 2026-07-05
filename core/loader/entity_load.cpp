// core/loader/entity_load.cpp — `*.entity.yaml`: entity/prefab files
// (m1-scene-format, spec 4.1 "machines are prefab subtrees ... instanced
// as assets with per-entity property-diff overrides", spec 4.2 override
// path grammar). `entity:` + `base:` (every component generic — see
// entity_format.h) + `machines:` (`instance: {uid?, path}` + its own
// `override:`) + `attachments:` (sockets: `of:` an asset, optionally
// `entity: {prefab: <path>}` a nested entity file — m1-prefab-spawn's job
// to resolve into a live entity; this node only parses + reports).
//
// Strictness contract (core/loader/gaps.h): a machine instance's file is
// ALWAYS hard-required (a machine file is structurally necessary — you
// cannot even describe the prefab's states without it); a `base:`/`state`
// component's TYPE and an attachment's asset files are lenient-only Gaps
// (brand new grammar, no M0 precedent to preserve).

#include "core/loader/asset_ref_parse.h"
#include "core/loader/entity_format.h"
#include "core/loader/generic_components.h"
#include "core/loader/loader.h"
#include "core/loader/override.h"
#include "core/loader/parse_util.h"

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace midday::loader {

using detail::err_node;
using detail::Parsed;

namespace {

struct EntityCtx {
    const std::string& path;
    const reflect::Registry& registry;
    const EventsDecl& vocab;
    const ComponentVocab& components_vocab;
    bool lenient = false;
    EntityFile out = {};
    std::optional<base::Error> error = {};

    void fail(base::Error value) {
        if (!error.has_value())
            error = std::move(value);
    }

    [[nodiscard]] bool failed() const { return error.has_value(); }
};

AssetRefDesc parse_asset_ref_field(EntityCtx& ctx, const YamlNode& node) {
    AssetRefParseResult parsed = parse_asset_ref(node, ctx.path, ctx.out.root_dir);
    if (parsed.error.has_value())
        ctx.fail(std::move(*parsed.error));
    return parsed.ref;
}

AssetRefDesc parse_path_only_ref_field(EntityCtx& ctx, const YamlNode& node) {
    AssetRefParseResult parsed = parse_path_only_ref(node, ctx.path, ctx.out.root_dir);
    if (parsed.error.has_value())
        ctx.fail(std::move(*parsed.error));
    return parsed.ref;
}

void parse_base(EntityCtx& ctx, const YamlNode& node) {
    GenericComponentsResult<GenericComponentEntry> result =
        parse_generic_components<GenericComponentEntry>(
            node, ctx.path, ctx.components_vocab, ctx.lenient);
    if (result.error.has_value()) {
        ctx.fail(std::move(*result.error));
        return;
    }
    append_gaps(ctx.out.gaps, std::move(result.gaps));
    ctx.out.base_components = std::move(result.components);
}

void parse_machine_instance(EntityCtx& ctx, const YamlNode& node) {
    if (!node.is_map()) {
        ctx.fail(
            err_node("loader.bad_value", ctx.path, node, "expected an {instance[, override]} map"));
        return;
    }
    static constexpr std::array<std::string_view, 2> kAllowed = {"instance", "override"};
    if (auto error = detail::check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    detail::FieldResult instance =
        detail::require_field(node, ctx.path, "instance", "a machine entry");
    if (instance.error.has_value()) {
        ctx.fail(std::move(*instance.error));
        return;
    }
    AssetRefDesc ref = parse_asset_ref_field(ctx, *instance.node);
    if (ctx.failed())
        return;
    // A machine file is structurally required in EVERY mode — you cannot
    // describe a prefab's states at all without it (unlike a model/prefab
    // asset ref, which is content this node never opens).
    if (!ref.exists) {
        ctx.fail(err_node("loader.bad_ref",
                          ctx.path,
                          *instance.node,
                          "machine file '" + ref.path_authored +
                              "' not found (resolved: " + ref.path_resolved + ")"));
        return;
    }

    std::uint32_t index = 0;
    bool known = false;
    for (; index < ctx.out.machine_files.size(); ++index) {
        if (ctx.out.machine_files[index].path == ref.path_resolved) {
            known = true;
            break;
        }
    }
    if (!known) {
        MachineLoadResult loaded = load_machine_file(ref.path_resolved,
                                                     ctx.out.root_dir,
                                                     ctx.registry,
                                                     ctx.vocab,
                                                     ctx.components_vocab,
                                                     ctx.lenient);
        if (loaded.error.has_value()) {
            ctx.fail(std::move(*loaded.error));
            return;
        }
        if (!loaded.machine.has_value()) { // defensive: loaders return one or the other
            ctx.fail(err_node("loader.bad_ref", ctx.path, *instance.node, "machine load failed"));
            return;
        }
        index = static_cast<std::uint32_t>(ctx.out.machine_files.size());
        append_gaps(ctx.out.gaps, loaded.machine->gaps);
        ctx.out.machine_files.push_back(std::move(*loaded.machine));
    }

    EntityMachineInstance desc;
    desc.instance_ref = ref;
    desc.machine_index = index;
    if (const YamlNode* override_node = node.find("override")) {
        OverrideParseResult parsed = parse_override_block(*override_node, ctx.path);
        if (parsed.error.has_value()) {
            ctx.fail(std::move(*parsed.error));
            return;
        }
        desc.overrides = std::move(parsed.entries);
    }
    ctx.out.machines.push_back(std::move(desc));
}

void report_missing_asset(EntityCtx& ctx, std::string_view kind, const AssetRefDesc& ref) {
    if (ref.exists)
        return;
    if (!ctx.lenient) {
        ctx.fail(base::Error{.code = "loader.bad_ref",
                             .message = ctx.path + ":" + std::to_string(ref.line) + ":" +
                                        std::to_string(ref.col) + ": " + std::string(kind) + " '" +
                                        ref.path_authored +
                                        "' not found (resolved: " + ref.path_resolved + ")"});
        return;
    }
    ctx.out.gaps.push_back(missing_asset_gap(kind, ref, ctx.path));
}

void parse_attachment(EntityCtx& ctx, const YamlNode& node) {
    if (!node.is_map()) {
        ctx.fail(
            err_node("loader.bad_value", ctx.path, node, "expected a {socket, of[, entity]} map"));
        return;
    }
    static constexpr std::array<std::string_view, 3> kAllowed = {"socket", "of", "entity"};
    if (auto error = detail::check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    detail::FieldResult socket = detail::require_field(node, ctx.path, "socket", "an attachment");
    detail::FieldResult of = detail::require_field(node, ctx.path, "of", "an attachment");
    if (socket.error.has_value() || of.error.has_value()) {
        ctx.fail(socket.error.has_value() ? std::move(*socket.error) : std::move(*of.error));
        return;
    }
    Parsed<std::string> socket_name = detail::get_name(*socket.node, ctx.path);
    if (socket_name.error.has_value()) {
        ctx.fail(std::move(*socket_name.error));
        return;
    }
    AttachmentDesc desc;
    desc.socket = base::Name(socket_name.value);
    desc.line = node.line;
    desc.of = parse_asset_ref_field(ctx, *of.node);
    if (ctx.failed())
        return;
    report_missing_asset(ctx, "model", desc.of);
    if (ctx.failed())
        return;

    if (const YamlNode* entity_node = node.find("entity")) {
        if (!entity_node->is_map()) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, *entity_node, "expected a {prefab: <path>} map"));
            return;
        }
        static constexpr std::array<std::string_view, 1> kEntityAllowed = {"prefab"};
        if (auto error = detail::check_keys(*entity_node, ctx.path, kEntityAllowed)) {
            ctx.fail(std::move(*error));
            return;
        }
        detail::FieldResult prefab =
            detail::require_field(*entity_node, ctx.path, "prefab", "an attachment entity");
        if (prefab.error.has_value()) {
            ctx.fail(std::move(*prefab.error));
            return;
        }
        AssetRefDesc entity_ref = parse_path_only_ref_field(ctx, *prefab.node);
        if (ctx.failed())
            return;
        report_missing_asset(ctx, "entity", entity_ref);
        if (ctx.failed())
            return;
        desc.entity_prefab = entity_ref;
    }
    ctx.out.attachments.push_back(std::move(desc));
}

} // namespace

EntityLoadResult load_entity_file(const std::string& path,
                                  const reflect::Registry& registry,
                                  const EventsDecl& vocab,
                                  const ComponentVocab& components_vocab,
                                  bool lenient) {
    EntityLoadResult result;
    YamlParseResult parsed = parse_yaml_file(path);
    if (parsed.error.has_value()) {
        result.error = std::move(parsed.error);
        return result;
    }
    const YamlNode& root = parsed.root;
    if (auto error = detail::check_format(root, path, "entity")) {
        result.error = std::move(error);
        return result;
    }
    static constexpr std::array<std::string_view, 5> kAllowed = {
        "format", "entity", "base", "machines", "attachments"};
    if (auto error = detail::check_keys(root, path, kAllowed)) {
        result.error = std::move(error);
        return result;
    }

    EntityCtx ctx{.path = path,
                  .registry = registry,
                  .vocab = vocab,
                  .components_vocab = components_vocab,
                  .lenient = lenient};
    ctx.out.path = path;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    ctx.out.root_dir = parent.empty() ? std::string(".") : parent.generic_string();

    detail::FieldResult name = detail::require_field(root, path, "entity", "an entity file");
    if (name.error.has_value()) {
        result.error = std::move(name.error);
        return result;
    }
    Parsed<std::string> entity_name = detail::get_name(*name.node, path);
    if (entity_name.error.has_value()) {
        result.error = std::move(entity_name.error);
        return result;
    }
    ctx.out.name = base::Name(entity_name.value);

    if (const YamlNode* base = root.find("base")) {
        parse_base(ctx, *base);
        if (ctx.failed()) {
            result.error = std::move(ctx.error);
            return result;
        }
    }
    if (const YamlNode* machines = root.find("machines")) {
        if (!machines->is_seq()) {
            result.error = err_node("loader.bad_value",
                                    path,
                                    *machines,
                                    "expected a list of {instance[, override]} entries");
            return result;
        }
        for (const YamlNode& machine : machines->seq) {
            parse_machine_instance(ctx, machine);
            if (ctx.failed()) {
                result.error = std::move(ctx.error);
                return result;
            }
        }
    }
    if (const YamlNode* attachments = root.find("attachments")) {
        if (!attachments->is_seq()) {
            result.error =
                err_node("loader.bad_value", path, *attachments, "expected a list of attachments");
            return result;
        }
        for (const YamlNode& attachment : attachments->seq) {
            parse_attachment(ctx, attachment);
            if (ctx.failed()) {
                result.error = std::move(ctx.error);
                return result;
            }
        }
    }

    result.entity = std::move(ctx.out);
    return result;
}

} // namespace midday::loader

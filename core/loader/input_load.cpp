// core/loader/input_load.cpp — `*.input.yaml`: named actions -> raw device
// bindings + named virtual-stick composites (loader.h's "input action maps"
// section owns the format contract). Structure mirrors events_load.cpp
// deliberately (project-wide merge, incremental duplicate/conflict checks,
// the SAME parse_util strict-field helpers) — one loader, ever.

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "core/loader/parse_util.h"
#include "core/loader/yaml.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace midday::loader {

using detail::err_at;
using detail::err_node;
using detail::Parsed;

std::string_view to_string(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::kKeyboard:
        return "keyboard";
    case DeviceKind::kMouse:
        return "mouse";
    case DeviceKind::kGamepad:
        return "gamepad";
    case DeviceKind::kTouch:
        return "touch";
    }
    return "keyboard"; // unreachable: every enumerator handled above
}

std::optional<DeviceKind> device_kind_from_string(std::string_view text) {
    if (text == "keyboard")
        return DeviceKind::kKeyboard;
    if (text == "mouse")
        return DeviceKind::kMouse;
    if (text == "gamepad")
        return DeviceKind::kGamepad;
    if (text == "touch")
        return DeviceKind::kTouch;
    return std::nullopt;
}

const ActionDesc* ActionMapDecl::find_action(std::string_view name) const {
    for (const ActionDesc& action : actions)
        if (action.name == name)
            return &action;
    return nullptr;
}

bool ActionMapDecl::has_stick(std::string_view name) const {
    for (const StickDesc& stick : sticks)
        if (stick.name == name)
            return true;
    return false;
}

namespace {

bool same_control(const BindingDesc& a, const BindingDesc& b) {
    return a.device == b.device && a.control == b.control;
}

base::Json
conflict_details(std::string_view action_a, std::string_view action_b, const BindingDesc& binding) {
    base::Json details = base::Json::object();
    details.set("action_a", action_a);
    details.set("action_b", action_b);
    details.set("device", to_string(binding.device));
    details.set("control", binding.control);
    return details;
}

std::string
conflict_message(std::string_view action_a, std::string_view action_b, const BindingDesc& binding) {
    return "'" + std::string(binding.control) + "' (" + std::string(to_string(binding.device)) +
           ") is bound to both '" + std::string(action_a) + "' and '" + std::string(action_b) + "'";
}

// `binding` (like detail::Parsed<T>'s `value`) is a plain default-constructed
// field, never an optional: it is only MEANINGFUL when `error` is empty, and
// callers only ever read it after checking that — the same discipline
// Parsed<T> uses, and for the same reason (an optional-of-optional shape
// defeats bugprone-unchecked-optional-access's dataflow tracking across two
// independent fields).
struct BindingParseResult {
    BindingDesc binding{};
    std::optional<base::Error> error;
};

BindingParseResult parse_binding(const std::string& path, const YamlNode& node) {
    BindingParseResult out;
    if (!node.is_map()) {
        out.error =
            err_node("loader.bad_value", path, node, "expected a {device, control} mapping");
        return out;
    }
    static constexpr std::array<std::string_view, 2> kAllowed = {"device", "control"};
    if (auto error = detail::check_keys(node, path, kAllowed)) {
        out.error = std::move(error);
        return out;
    }
    detail::FieldResult device_field = detail::require_field(node, path, "device", "a binding");
    if (device_field.error.has_value()) {
        out.error = std::move(device_field.error);
        return out;
    }
    Parsed<std::string> device_text = detail::get_name(*device_field.node, path);
    if (device_text.error.has_value()) {
        out.error = std::move(device_text.error);
        return out;
    }
    std::optional<DeviceKind> device = device_kind_from_string(device_text.value);
    if (!device.has_value()) {
        out.error = err_node("loader.bad_value",
                             path,
                             *device_field.node,
                             "unknown device '" + device_text.value +
                                 "' (allowed: keyboard, mouse, gamepad, touch)");
        return out;
    }
    detail::FieldResult control_field = detail::require_field(node, path, "control", "a binding");
    if (control_field.error.has_value()) {
        out.error = std::move(control_field.error);
        return out;
    }
    Parsed<std::string> control_text = detail::get_name(*control_field.node, path);
    if (control_text.error.has_value()) {
        out.error = std::move(control_text.error);
        return out;
    }
    out.binding = BindingDesc{*device, std::move(control_text.value)};
    return out;
}

// Self-repeat within the CURRENT (in-progress, not-yet-merged) action, or a
// conflict against any OTHER already-merged action in `decl` — the ONE
// comparison rule authoring-time parsing uses; find_conflict() below is its
// post-merge (no source location) sibling.
std::optional<base::Error> add_binding(const ActionMapDecl& decl,
                                       ActionDesc& action,
                                       BindingDesc binding,
                                       const std::string& path,
                                       const YamlNode& node) {
    for (const BindingDesc& existing : action.bindings)
        if (same_control(existing, binding))
            return err_node("loader.duplicate",
                            path,
                            node,
                            "action '" + action.name + "' already binds " +
                                std::string(to_string(binding.device)) + ":" + binding.control);
    for (const ActionDesc& other : decl.actions) {
        for (const BindingDesc& existing : other.bindings) {
            if (!same_control(existing, binding))
                continue;
            base::Error error = err_node(
                "input.conflict", path, node, conflict_message(other.name, action.name, binding));
            error.details = conflict_details(other.name, action.name, binding);
            return error;
        }
    }
    action.bindings.push_back(std::move(binding));
    return std::nullopt;
}

// Shared by load_input_file's `actions:` and load_input_profile_file's
// `overlay:` (identical grammar, different top-level key) — the ONE parse
// path for "a map of action name -> {bindings: [...]}" (no duplicated loop).
std::optional<base::Error>
load_actions_map(const YamlNode& actions_node, const std::string& path, ActionMapDecl& decl) {
    if (!actions_node.is_map())
        return err_node(
            "loader.bad_value", path, actions_node, "expected an {action: ...} mapping");
    for (const YamlEntry& entry : actions_node.map) {
        if (entry.key.empty())
            return err_at("loader.bad_value",
                          path,
                          entry.key_line,
                          entry.key_col,
                          "action name must not be empty");
        if (decl.has_action(entry.key))
            return err_at("loader.duplicate",
                          path,
                          entry.key_line,
                          entry.key_col,
                          "action '" + entry.key + "' is already declared");
        ActionDesc action;
        action.name = entry.key;
        const YamlNode& body = entry.node();
        if (!body.is_null()) { // "name:" alone declares a binding-less action
            if (!body.is_map())
                return err_node(
                    "loader.bad_value", path, body, "expected an action definition mapping");
            static constexpr std::array<std::string_view, 1> kActionKeys = {"bindings"};
            if (auto error = detail::check_keys(body, path, kActionKeys))
                return error;
            if (const YamlNode* bindings = body.find("bindings")) {
                if (!bindings->is_seq())
                    return err_node(
                        "loader.bad_value", path, *bindings, "expected a list of bindings");
                for (const YamlNode& item : bindings->seq) {
                    BindingParseResult parsed_binding = parse_binding(path, item);
                    if (parsed_binding.error.has_value())
                        return std::move(parsed_binding.error);
                    if (auto error = add_binding(
                            decl, action, std::move(parsed_binding.binding), path, item))
                        return error;
                }
            }
        }
        decl.actions.push_back(std::move(action));
    }
    return std::nullopt;
}

} // namespace

std::optional<base::Error> load_input_file(const std::string& path, ActionMapDecl& decl) {
    YamlParseResult parsed = parse_yaml_file(path);
    if (parsed.error.has_value())
        return std::move(parsed.error);
    const YamlNode& root = parsed.root;
    if (auto error = detail::check_format(root, path, "input"))
        return error;
    static constexpr std::array<std::string_view, 3> kAllowed = {"format", "actions", "sticks"};
    if (auto error = detail::check_keys(root, path, kAllowed))
        return error;

    if (const YamlNode* actions = root.find("actions"))
        if (auto error = load_actions_map(*actions, path, decl))
            return error;

    if (const YamlNode* sticks = root.find("sticks")) {
        if (!sticks->is_map())
            return err_node("loader.bad_value", path, *sticks, "expected a {stick: ...} mapping");
        for (const YamlEntry& entry : sticks->map) {
            if (entry.key.empty())
                return err_at("loader.bad_value",
                              path,
                              entry.key_line,
                              entry.key_col,
                              "stick name must not be empty");
            if (decl.has_stick(entry.key))
                return err_at("loader.duplicate",
                              path,
                              entry.key_line,
                              entry.key_col,
                              "stick '" + entry.key + "' is already declared");
            const YamlNode& body = entry.node();
            if (!body.is_map())
                return err_node(
                    "loader.bad_value", path, body, "expected a stick definition mapping");
            static constexpr std::array<std::string_view, 5> kStickKeys = {
                "up", "down", "left", "right", "deadzone"};
            if (auto error = detail::check_keys(body, path, kStickKeys))
                return error;

            StickDesc stick;
            stick.name = entry.key;
            auto resolve_dir = [&](std::string_view key,
                                   std::string& out) -> std::optional<base::Error> {
                detail::FieldResult field = detail::require_field(body, path, key, "a stick");
                if (field.error.has_value())
                    return field.error;
                Parsed<std::string> name = detail::get_name(*field.node, path);
                if (name.error.has_value())
                    return name.error;
                if (!decl.has_action(name.value))
                    return err_node("loader.bad_ref",
                                    path,
                                    *field.node,
                                    "stick direction references undeclared action '" + name.value +
                                        "'");
                out = std::move(name.value);
                return std::nullopt;
            };
            if (auto error = resolve_dir("up", stick.up))
                return error;
            if (auto error = resolve_dir("down", stick.down))
                return error;
            if (auto error = resolve_dir("left", stick.left))
                return error;
            if (auto error = resolve_dir("right", stick.right))
                return error;

            if (const YamlNode* deadzone = body.find("deadzone")) {
                Parsed<double> value = detail::get_float(*deadzone, path);
                if (value.error.has_value())
                    return value.error;
                if (value.value < 0.0 || value.value >= 1.0)
                    return err_node(
                        "loader.bad_value", path, *deadzone, "deadzone must be in [0, 1)");
                stick.deadzone = static_cast<float>(value.value);
            }
            decl.sticks.push_back(std::move(stick));
        }
    }
    return std::nullopt;
}

namespace {
constexpr std::string_view kInputSuffix = ".input.yaml";
} // namespace

ProjectInputResult load_project_input(const std::string& root_dir) {
    ProjectInputResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(root_dir, ec)) {
        result.error = base::file_error("loader.io",
                                        "input project root '" + root_dir + "' is not a directory");
        return result;
    }
    std::filesystem::recursive_directory_iterator it(root_dir, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec))
            continue;
        if (std::string_view(it->path().filename().string()).ends_with(kInputSuffix))
            result.files.push_back(it->path().lexically_normal().generic_string());
    }
    if (ec) {
        result.error =
            base::file_error("loader.io", "cannot walk input project root '" + root_dir + "'");
        return result;
    }
    std::ranges::sort(result.files); // deterministic merge order, project-wide

    for (const std::string& file : result.files) {
        if (auto error = load_input_file(file, result.decl)) {
            result.error = std::move(error);
            return result;
        }
    }
    return result;
}

std::optional<base::Error> load_input_profile_file(const std::string& path,
                                                   ActionMapDecl& overlay) {
    YamlParseResult parsed = parse_yaml_file(path);
    if (parsed.error.has_value())
        return std::move(parsed.error);
    const YamlNode& root = parsed.root;
    if (auto error = detail::check_format(root, path, "input_profile"))
        return error;
    static constexpr std::array<std::string_view, 2> kAllowed = {"format", "overlay"};
    if (auto error = detail::check_keys(root, path, kAllowed))
        return error;

    const YamlNode* actions = root.find("overlay");
    if (actions == nullptr)
        return std::nullopt; // an empty overlay (no rebinds yet) is legal
    return load_actions_map(*actions, path, overlay);
}

std::optional<base::Error> find_conflict(const std::vector<ActionDesc>& actions) {
    for (std::size_t i = 0; i < actions.size(); ++i) {
        for (const BindingDesc& lhs : actions[i].bindings) {
            for (std::size_t j = i + 1; j < actions.size(); ++j) {
                for (const BindingDesc& rhs : actions[j].bindings) {
                    if (!same_control(lhs, rhs))
                        continue;
                    base::Error error;
                    error.code = "input.conflict";
                    error.message = conflict_message(actions[i].name, actions[j].name, lhs);
                    error.details = conflict_details(actions[i].name, actions[j].name, lhs);
                    return error;
                }
            }
        }
    }
    return std::nullopt;
}

ApplyOverlayResult apply_overlay(const ActionMapDecl& base, const ActionMapDecl& overlay) {
    ApplyOverlayResult result;
    result.map = base;
    for (const ActionDesc& rebind : overlay.actions) {
        ActionDesc* target = nullptr;
        for (ActionDesc& candidate : result.map.actions) {
            if (candidate.name == rebind.name) {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr) {
            base::Error error;
            error.code = "loader.bad_ref";
            error.message = "overlay rebinds undeclared action '" + rebind.name + "'";
            error.details.set("action", rebind.name);
            result.error = std::move(error);
            return result;
        }
        target->bindings = rebind.bindings;
    }
    result.error = find_conflict(result.map.actions);
    return result;
}

} // namespace midday::loader

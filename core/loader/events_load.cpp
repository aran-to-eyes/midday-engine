// core/loader/events_load.cpp — `*.events.yaml`: the project's custom event
// vocabulary (typed payload schemas, spec 4.2 "events are first-class
// definitions") plus the declared group keys usable as `key:` spellings.
// Types are canonical reflect::TypeDesc spellings ("float", "entity_ref",
// "vec3", "array<int>", ...) — the one type language of the schema
// manifest, not a YAML-local dialect.

#include "core/base/name.h"
#include "core/loader/loader.h"
#include "core/loader/parse_util.h"
#include "core/loader/yaml.h"

#include <array>
#include <string>
#include <utility>

namespace midday::loader {

using detail::err_node;
using detail::Parsed;

bool EventsDecl::has_event(std::string_view name) const {
    for (const EventDecl& event : events)
        if (event.name == name)
            return true;
    return false;
}

bool EventsDecl::has_group(std::string_view name) const {
    for (const std::string& group : group_keys)
        if (group == name)
            return true;
    return false;
}

namespace {

std::optional<base::Error> load_event(const std::string& path,
                                      const YamlEntry& entry,
                                      const reflect::Registry& registry,
                                      EventsDecl& decl) {
    if (entry.key.empty())
        return detail::err_at("loader.bad_value",
                              path,
                              entry.key_line,
                              entry.key_col,
                              "event name must not be empty");
    if (decl.has_event(entry.key))
        return detail::err_at("loader.duplicate",
                              path,
                              entry.key_line,
                              entry.key_col,
                              "event '" + entry.key + "' is already declared");
    if (registry.find_event(base::Name(entry.key)) != nullptr)
        return detail::err_at("loader.duplicate",
                              path,
                              entry.key_line,
                              entry.key_col,
                              "event '" + entry.key + "' collides with the built-in vocabulary");

    EventDecl event;
    event.name = entry.key;
    const YamlNode& body = entry.node();
    if (!body.is_null()) { // "name:" alone declares a payload-less event
        if (!body.is_map())
            return err_node("loader.bad_value", path, body, "expected an event definition mapping");
        static constexpr std::array<std::string_view, 2> kAllowed = {"payload", "doc"};
        if (auto error = detail::check_keys(body, path, kAllowed))
            return error;
        if (const YamlNode* doc = body.find("doc")) {
            Parsed<std::string> text = detail::get_string(*doc, path);
            if (text.error.has_value())
                return std::move(text.error);
            event.doc = std::move(text.value);
        }
        if (const YamlNode* payload = body.find("payload")) {
            if (!payload->is_map() && !payload->is_null())
                return err_node(
                    "loader.bad_value", path, *payload, "expected a {field: type} mapping");
            if (payload->is_map()) {
                for (const YamlEntry& field : payload->map) {
                    Parsed<std::string> spelling = detail::get_name(field.node(), path);
                    if (spelling.error.has_value())
                        return std::move(spelling.error);
                    std::optional<reflect::TypeDesc> type =
                        reflect::TypeDesc::parse(spelling.value);
                    if (!type.has_value())
                        return err_node("loader.bad_value",
                                        path,
                                        field.node(),
                                        "unknown type '" + spelling.value +
                                            "' (use canonical reflect spellings: float, int, "
                                            "bool, string, name, vec2, vec3, vec4, quat, color, "
                                            "entity_ref, asset_ref, array<T>, map<T>)");
                    event.payload.push_back(EventFieldDecl{field.key, std::move(*type)});
                }
            }
        }
    }
    decl.events.push_back(std::move(event));
    return std::nullopt;
}

} // namespace

std::optional<base::Error>
load_events_file(const std::string& path, const reflect::Registry& registry, EventsDecl& decl) {
    YamlParseResult parsed = parse_yaml_file(path);
    if (parsed.error.has_value())
        return std::move(parsed.error);
    const YamlNode& root = parsed.root;
    if (auto error = detail::check_format(root, path, "events"))
        return error;
    static constexpr std::array<std::string_view, 3> kAllowed = {"format", "events", "keys"};
    if (auto error = detail::check_keys(root, path, kAllowed))
        return error;

    if (const YamlNode* events = root.find("events")) {
        if (!events->is_map())
            return err_node("loader.bad_value", path, *events, "expected an {event: ...} mapping");
        for (const YamlEntry& entry : events->map)
            if (auto error = load_event(path, entry, registry, decl))
                return error;
    }

    if (const YamlNode* keys = root.find("keys")) {
        if (!keys->is_seq())
            return err_node("loader.bad_value", path, *keys, "expected a list of group key names");
        for (const YamlNode& key : keys->seq) {
            Parsed<std::string> name = detail::get_name(key, path);
            if (name.error.has_value())
                return std::move(name.error);
            if (name.value == "self" || name.value == "root" || name.value == "global")
                return err_node("loader.bad_value",
                                path,
                                key,
                                "'" + name.value + "' is a reserved key spelling");
            if (decl.has_group(name.value))
                return err_node("loader.duplicate",
                                path,
                                key,
                                "group key '" + name.value + "' is already declared");
            decl.group_keys.push_back(std::move(name.value));
        }
    }
    return std::nullopt;
}

} // namespace midday::loader

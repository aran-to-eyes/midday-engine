// core/loader/generic_components.cpp — generic_components.h: the shared
// `{Name: {field: value, ...}}` list engine, explicitly instantiated for
// the two entry types callers use.

#include "core/loader/generic_components.h"

#include "core/loader/entity_format.h"
#include "core/loader/parse_util.h"
#include "core/statechart/machine_desc.h"

#include <utility>

namespace midday::loader {

using detail::err_at;
using detail::err_node;
using detail::Parsed;

template <typename ComponentEntry>
GenericComponentsResult<ComponentEntry> parse_generic_components(const YamlNode& node,
                                                                 std::string_view file,
                                                                 const ComponentVocab& vocab,
                                                                 bool lenient) {
    GenericComponentsResult<ComponentEntry> out;
    if (!node.is_seq()) {
        out.error = err_node("loader.bad_value", file, node, "expected a list of components");
        return out;
    }
    for (const YamlNode& component : node.seq) {
        if (!component.is_map() || component.map.size() != 1) {
            out.error = err_node(
                "loader.bad_value", file, component, "expected one {ComponentName: {...}} entry");
            return out;
        }
        const YamlEntry& entry = component.map.front();
        const YamlNode& body = entry.node();
        if (!body.is_map() && !body.is_null()) {
            out.error =
                err_node("loader.bad_value", file, body, "expected a component property mapping");
            return out;
        }
        Parsed<base::Json> fields = body.is_null() ? Parsed<base::Json>{base::Json::object(), {}}
                                                   : detail::yaml_to_json(body, file);
        if (fields.error.has_value()) {
            out.error = std::move(fields.error);
            return out;
        }
        for (const ComponentEntry& existing : out.components) {
            if (existing.type.view() == entry.key) {
                out.error = err_at("loader.duplicate",
                                   file,
                                   entry.key_line,
                                   entry.key_col,
                                   "component '" + entry.key + "' is already declared");
                return out;
            }
        }
        if (!vocab.known(entry.key)) {
            if (!lenient) {
                out.error = err_at("loader.unknown_key",
                                   file,
                                   entry.key_line,
                                   entry.key_col,
                                   "unknown component '" + entry.key + "'");
                return out;
            }
            out.gaps.push_back(
                Gap{.kind = "component",
                    .what = entry.key,
                    .file = std::string(file),
                    .line = entry.key_line,
                    .col = entry.key_col,
                    .detail = "component type '" + entry.key + "' is not implemented yet"});
        }
        out.components.push_back(
            ComponentEntry{.type = base::Name(entry.key), .fields = fields.value});
    }
    return out;
}

template GenericComponentsResult<statechart::StateComponentDesc>
parse_generic_components(const YamlNode&, std::string_view, const ComponentVocab&, bool);
template GenericComponentsResult<GenericComponentEntry>
parse_generic_components(const YamlNode&, std::string_view, const ComponentVocab&, bool);

} // namespace midday::loader

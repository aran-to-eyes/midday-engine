// core/loader/yaml_parse.cpp — the ryml quarantine: the ONLY translation
// unit in the tree that sees a vendored rapidyaml type. Parses one strict
// document, walks the ryml tree ONCE into the owned first-party YamlNode
// model (every node and mapping key stamped with a 1-based location), and
// layers the strictness ryml deliberately leaves to callers: duplicate
// keys, anchors/aliases/tags, and multi-document streams all refuse with
// file:line:col (yaml.h header contract).
//
// Error transport: ryml error callbacks must not return; with exceptions
// available the sanctioned pattern is to throw from the callback and catch
// at the parse boundary. The exception NEVER crosses this TU — parse_yaml
// converts it into the structured base::Error the rest of the tree speaks
// (core/base/error.h: errors are values across subsystem boundaries).

#include "core/base/file_io.h"
#include "core/loader/yaml.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifndef _RYML_SINGLE_HEADER_AMALGAMATED_HPP_
#include "rapidyaml.hpp" // IWYU pragma: keep
#endif

namespace midday::loader {
namespace {

// Thrown by the ryml error callbacks (which must not return), caught at the
// parse boundary in parse_yaml — never escapes this TU.
struct ParseAbort {
    std::string message;
    std::size_t line = ryml::npos; // ryml's 0-based line, npos = unknown
    std::size_t col = ryml::npos;
};

[[noreturn]] void on_error_basic(ryml::csubstr msg, const ryml::ErrorDataBasic&, void*) {
    throw ParseAbort{std::string(msg.str, msg.len)};
}

[[noreturn]] void on_error_parse(ryml::csubstr msg, const ryml::ErrorDataParse& data, void*) {
    throw ParseAbort{std::string(msg.str, msg.len), data.ymlloc.line, data.ymlloc.col};
}

[[noreturn]] void on_error_visit(ryml::csubstr msg, const ryml::ErrorDataVisit&, void*) {
    throw ParseAbort{std::string(msg.str, msg.len)};
}

ryml::Callbacks strict_callbacks() {
    ryml::Callbacks callbacks;
    callbacks.set_error_basic(&on_error_basic);
    callbacks.set_error_parse(&on_error_parse);
    callbacks.set_error_visit(&on_error_visit);
    return callbacks;
}

// "<origin>:<line>:<col>: <what>" + details {file, line, col} — the loader
// diagnostic shape (line/col 0 when the parser could not locate).
base::Error located_error(
    std::string_view code, std::string_view origin, int line, int col, std::string_view what) {
    base::Error error;
    error.code = std::string(code);
    error.message = std::string(origin) + ":" + std::to_string(line) + ":" + std::to_string(col) +
                    ": " + std::string(what);
    error.details.set("file", origin);
    error.details.set("line", static_cast<std::int64_t>(line));
    error.details.set("col", static_cast<std::int64_t>(col));
    return error;
}

// ryml locations are 0-based; the loader speaks 1-based (json.h precedent).
int one_based(std::size_t value) {
    return value == ryml::npos ? 0 : static_cast<int>(value) + 1;
}

struct Walker {
    const ryml::Parser& parser;
    const ryml::Tree& tree;
    std::string_view origin;
    std::optional<base::Error> error = {}; // first strictness refusal wins

    void locate(ryml::id_type id, YamlNode& node) {
        const ryml::Location loc = tree.location(parser, id);
        node.line = one_based(loc.line);
        node.col = one_based(loc.col);
    }

    void refuse(ryml::id_type id, std::string_view what) {
        if (error.has_value())
            return;
        const ryml::Location loc = tree.location(parser, id);
        error = located_error("yaml.strict", origin, one_based(loc.line), one_based(loc.col), what);
    }

    YamlNode convert(ryml::id_type id) {
        YamlNode node;
        if (error.has_value())
            return node;
        locate(id, node);
        const ryml::NodeType type = tree.type(id);
        if (type.has_key_anchor() || type.has_val_anchor()) {
            refuse(id, "anchors are not allowed (strict subset)");
            return node;
        }
        if (type.is_key_ref() || type.is_val_ref()) {
            refuse(id, "aliases are not allowed (strict subset)");
            return node;
        }
        if (type.has_key_tag() || type.has_val_tag()) {
            refuse(id, "tags are not allowed (strict subset)");
            return node;
        }
        if (type.is_map()) {
            node.kind = YamlNode::Kind::kMap;
            for (ryml::id_type child = tree.first_child(id); child != ryml::NONE;
                 child = tree.next_sibling(child)) {
                const ryml::csubstr key = tree.key(child);
                YamlEntry entry;
                entry.key.assign(key.str, key.len);
                const ryml::Location key_loc = parser.val_location(key.str);
                entry.key_line = one_based(key_loc.line);
                entry.key_col = one_based(key_loc.col);
                for (const YamlEntry& seen : node.map) {
                    if (seen.key == entry.key) {
                        error = located_error("yaml.strict",
                                              origin,
                                              entry.key_line,
                                              entry.key_col,
                                              "duplicate key '" + entry.key + "'");
                        return node;
                    }
                }
                entry.value.push_back(convert(child));
                if (error.has_value())
                    return node;
                node.map.push_back(std::move(entry));
            }
            return node;
        }
        if (type.is_seq()) {
            node.kind = YamlNode::Kind::kSeq;
            for (ryml::id_type child = tree.first_child(id); child != ryml::NONE;
                 child = tree.next_sibling(child)) {
                node.seq.push_back(convert(child));
                if (error.has_value())
                    return node;
            }
            return node;
        }
        // Scalar or empty value. An authored-empty value ("key:" / "- ") has
        // no backing text in the source buffer; spelled scalars (including
        // the literal texts "null" and "~") stay raw scalars — the parser
        // never implicit-types (yaml.h contract).
        const ryml::csubstr value = tree.has_val(id) ? tree.val(id) : ryml::csubstr{};
        if (value.str == nullptr) {
            node.kind = YamlNode::Kind::kNull;
            return node;
        }
        node.kind = YamlNode::Kind::kScalar;
        node.scalar.assign(value.str, value.len);
        node.quoted = type.is_val_quoted();
        const ryml::Location val_loc = parser.val_location(value.str);
        node.line = one_based(val_loc.line);
        node.col = one_based(val_loc.col);
        return node;
    }
};

} // namespace

YamlParseResult parse_yaml(std::string_view text, std::string_view origin) {
    YamlParseResult result;
    const ryml::Callbacks callbacks = strict_callbacks();
    ryml::EventHandlerTree handler(callbacks);
    ryml::Parser parser(&handler, ryml::ParserOptions().locations(true));
    ryml::Tree tree(callbacks);
    try {
        ryml::parse_in_arena(&parser,
                             ryml::csubstr(origin.data(), origin.size()),
                             ryml::csubstr(text.data(), text.size()),
                             &tree);
    } catch (const ParseAbort& abort) {
        result.error = located_error("yaml.parse",
                                     origin,
                                     one_based(abort.line),
                                     one_based(abort.col),
                                     abort.message.empty() ? "malformed YAML" : abort.message);
        return result;
    }

    try {
        ryml::id_type root = tree.root_id();
        if (tree.is_stream(root)) {
            // A single leading "---" is tolerated (one doc unwraps); real
            // multi-document streams refuse — one file, one document.
            if (tree.num_children(root) != 1) {
                result.error =
                    located_error("yaml.strict", origin, 1, 1, "multiple YAML documents");
                return result;
            }
            root = tree.first_child(root);
        }
        if (tree.num_children(root) == 0 && !tree.has_val(root)) {
            result.root.kind = YamlNode::Kind::kNull; // empty / comments-only
            result.root.line = 1;
            result.root.col = 1;
            return result;
        }
        Walker walker{.parser = parser, .tree = tree, .origin = origin};
        result.root = walker.convert(root);
        if (walker.error.has_value())
            result.error = std::move(walker.error);
    } catch (const ParseAbort& abort) { // location queries fault-path
        result.error = located_error("yaml.parse",
                                     origin,
                                     one_based(abort.line),
                                     one_based(abort.col),
                                     abort.message.empty() ? "malformed YAML" : abort.message);
    }
    return result;
}

YamlParseResult parse_yaml_file(const std::string& path) {
    base::ReadFileResult read = base::read_file(path, "loader.io");
    if (read.error.has_value()) {
        YamlParseResult result;
        result.error = std::move(read.error);
        return result;
    }
    return parse_yaml(read.bytes, path);
}

} // namespace midday::loader

// core/loader/parse_util.cpp — strict-field helpers (parse_util.h). Scalar
// number reads go through the ONE core JSON number grammar (base::Json::
// parse over the scalar text — the same strictness the CLI applies to argv
// values, D-BUILD-039), so "1e3" / "-2.5" / "007" behave identically in
// every text surface of the engine.

#include "core/loader/parse_util.h"

#include <utility>

namespace midday::loader::detail {

base::Error
err_at(std::string_view code, std::string_view file, int line, int col, const std::string& what) {
    base::Error error;
    error.code = std::string(code);
    error.message =
        std::string(file) + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " + what;
    error.details.set("file", file);
    error.details.set("line", static_cast<std::int64_t>(line));
    error.details.set("col", static_cast<std::int64_t>(col));
    return error;
}

std::optional<base::Error>
check_keys(const YamlNode& map, std::string_view file, std::span<const std::string_view> allowed) {
    if (!map.is_map())
        return std::nullopt; // shape errors are the caller's message
    for (const YamlEntry& entry : map.map) {
        bool known = false;
        for (std::string_view key : allowed)
            known = known || entry.key == key;
        if (known)
            continue;
        std::string list;
        for (std::string_view key : allowed) {
            if (!list.empty())
                list += ", ";
            list += key;
        }
        const std::string suffix =
            allowed.empty() ? " (no keys allowed here in M0)" : " (allowed: " + list + ")";
        return err_at("loader.unknown_key",
                      file,
                      entry.key_line,
                      entry.key_col,
                      "unknown key '" + entry.key + "'" + suffix);
    }
    return std::nullopt;
}

FieldResult require_field(const YamlNode& map,
                          std::string_view file,
                          std::string_view key,
                          std::string_view context) {
    FieldResult result;
    result.node = map.find(key);
    if (result.node == nullptr)
        result.error = err_node("loader.bad_value",
                                file,
                                map,
                                std::string(context) + " requires '" + std::string(key) + "'");
    return result;
}

namespace {

base::Error not_a(std::string_view file, const YamlNode& node, std::string_view wanted) {
    return err_node("loader.bad_value", file, node, "expected " + std::string(wanted));
}

} // namespace

Parsed<std::string> get_string(const YamlNode& node, std::string_view file) {
    Parsed<std::string> out;
    if (!node.is_scalar()) {
        out.error = not_a(file, node, "a string");
        return out;
    }
    out.value = node.scalar;
    return out;
}

Parsed<std::string> get_name(const YamlNode& node, std::string_view file) {
    Parsed<std::string> out = get_string(node, file);
    if (!out.error.has_value() && out.value.empty())
        out.error = not_a(file, node, "a non-empty name");
    return out;
}

Parsed<bool> get_bool(const YamlNode& node, std::string_view file) {
    Parsed<bool> out;
    if (!node.is_scalar() || node.quoted || (node.scalar != "true" && node.scalar != "false")) {
        out.error = not_a(file, node, "true or false");
        return out;
    }
    out.value = node.scalar == "true";
    return out;
}

Parsed<std::int64_t> get_int(const YamlNode& node, std::string_view file) {
    Parsed<std::int64_t> out;
    if (node.is_scalar() && !node.quoted) {
        const base::Json::ParseResult parsed = base::Json::parse(node.scalar);
        if (!parsed.error.has_value() && parsed.value.is_int()) {
            out.value = parsed.value.as_int();
            return out;
        }
    }
    out.error = not_a(file, node, "an integer");
    return out;
}

Parsed<double> get_float(const YamlNode& node, std::string_view file) {
    Parsed<double> out;
    if (node.is_scalar() && !node.quoted) {
        const base::Json::ParseResult parsed = base::Json::parse(node.scalar);
        if (!parsed.error.has_value() && parsed.value.is_number()) {
            out.value = parsed.value.as_double();
            return out;
        }
    }
    out.error = not_a(file, node, "a number");
    return out;
}

Parsed<math::Vec3> get_vec3(const YamlNode& node, std::string_view file) {
    Parsed<math::Vec3> out;
    if (!node.is_seq() || node.seq.size() != 3) {
        out.error = not_a(file, node, "a [x, y, z] triple");
        return out;
    }
    float lanes[3] = {};
    for (int i = 0; i < 3; ++i) {
        Parsed<double> lane = get_float(node.seq[static_cast<std::size_t>(i)], file);
        if (lane.error.has_value()) {
            out.error = std::move(lane.error);
            return out;
        }
        lanes[i] = static_cast<float>(lane.value);
    }
    out.value = math::Vec3{lanes[0], lanes[1], lanes[2]};
    return out;
}

Parsed<base::Json> yaml_to_json(const YamlNode& node, std::string_view file) {
    Parsed<base::Json> out;
    switch (node.kind) {
    case YamlNode::Kind::kNull:
        out.error = not_a(file, node, "a value (empty values are not allowed here)");
        return out;
    case YamlNode::Kind::kScalar: {
        if (!node.quoted) {
            const base::Json::ParseResult parsed = base::Json::parse(node.scalar);
            if (!parsed.error.has_value() && parsed.value.is_number()) {
                out.value = parsed.value;
                return out;
            }
            if (node.scalar == "true" || node.scalar == "false") {
                out.value = base::Json(node.scalar == "true");
                return out;
            }
        }
        out.value = base::Json(node.scalar);
        return out;
    }
    case YamlNode::Kind::kMap: {
        out.value = base::Json::object();
        for (const YamlEntry& entry : node.map) {
            Parsed<base::Json> child = yaml_to_json(entry.node(), file);
            if (child.error.has_value()) {
                out.error = std::move(child.error);
                return out;
            }
            out.value.set(entry.key, std::move(child.value));
        }
        return out;
    }
    case YamlNode::Kind::kSeq: {
        out.value = base::Json::array();
        for (const YamlNode& element : node.seq) {
            Parsed<base::Json> child = yaml_to_json(element, file);
            if (child.error.has_value()) {
                out.error = std::move(child.error);
                return out;
            }
            out.value.push(std::move(child.value));
        }
        return out;
    }
    }
    out.error = not_a(file, node, "a value");
    return out;
}

std::optional<base::Error>
check_format(const YamlNode& root, std::string_view file, std::string_view format_name) {
    if (!root.is_map())
        return err_node(
            "loader.bad_value", file, root, std::string(format_name) + " file must be a mapping");
    const YamlNode* format = root.find("format");
    if (format == nullptr)
        return err_node("loader.bad_format",
                        file,
                        root,
                        std::string(format_name) + " file requires 'format: 1'");
    Parsed<std::int64_t> version = get_int(*format, file);
    if (version.error.has_value())
        return version.error;
    if (version.value != 1)
        return err_node("loader.bad_format",
                        file,
                        *format,
                        "unknown " + std::string(format_name) + " format version " +
                            std::to_string(version.value) + " (this engine reads format 1)");
    return std::nullopt;
}

} // namespace midday::loader::detail

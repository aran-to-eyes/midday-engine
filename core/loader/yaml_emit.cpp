// core/loader/yaml_emit.cpp — yaml_emit.h: the canonical strict-YAML
// serializer. See the header for the full rule set; this file is the one
// place that implements it.

#include "core/loader/yaml_emit.h"

#include <cstdio>
#include <string>
#include <vector>

namespace midday::loader {
namespace {

std::string indent(int depth) {
    // NOT `return {count, ' '};` — with a non-constant count that is a
    // narrowing-conversion compile error (initializer_list<char> vs the
    // (size_type, CharT) constructor), not the char-doubling constructor.
    const std::string spaces(static_cast<std::size_t>(depth) * 2, ' ');
    return spaces;
}

// Byte-wise double-quote escaping (YAML double-quote escapes are a superset
// of JSON's; the common cases are named, everything else falls back to
// \xHH — non-ASCII UTF-8 bytes pass through untouched, they need no escape).
std::string quote_escape(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\x%02x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += "\"";
    return out;
}

// Would `text` be unsafe as a bare (plain) YAML scalar in a "key:" or
// "- value" position? Conservative on purpose: false negatives would corrupt
// content, false positives only cost an unnecessary pair of quotes.
bool needs_quoting(std::string_view text) {
    if (text.empty())
        return true;
    if (text.front() == ' ' || text.back() == ' ')
        return true;
    if (text.find('\n') != std::string_view::npos)
        return true;
    constexpr std::string_view kIndicators = "-?:,[]{}#&*!|>'\"%@`";
    if (kIndicators.find(text.front()) != std::string_view::npos)
        return true;
    if (text.find(": ") != std::string_view::npos)
        return true;
    if (text.back() == ':')
        return true;
    return false;
}

std::string render_key(std::string_view key) {
    return needs_quoting(key) ? quote_escape(key) : std::string(key);
}

// Preserves the parser's `quoted` bit exactly (it is semantic, not stylistic
// — see yaml.h: quoted scalars are always strings, never implicitly typed).
// An unquoted scalar is re-emitted verbatim: the parser already proved it is
// a safe plain scalar in its ORIGINAL context, and block style (what the
// emitter always uses) is never more restrictive than the flow style it may
// have been written in — UNLESS it carries a raw newline, which no plain
// scalar can represent, so that one case forces double-quoted output.
std::string render_scalar(const YamlNode& node) {
    if (node.quoted || node.scalar.find('\n') != std::string::npos)
        return quote_escape(node.scalar);
    return node.scalar;
}

bool is_empty_container(const YamlNode& node) {
    return (node.is_map() && node.map.empty()) || (node.is_seq() && node.seq.empty());
}

std::string_view empty_container_token(const YamlNode& node) {
    return node.is_map() ? "{}" : "[]";
}

std::vector<std::string> render_lines(const YamlNode& node, int depth);

// Renders `child` as the value that follows "key:" or a sequence dash,
// appending onto `lines` (whose current back() is that "key:"/"- " line —
// the last line already has the marker, no trailing space when the value is
// non-inline). `inline_prefix` is what the marker line already ends with
// ("" for "key:", "- " already included for sequence items — callers pass
// whether a space+inline value should be appended to lines.back() instead).
void append_value(std::vector<std::string>& lines,
                  std::string& marker_line,
                  const YamlNode& child,
                  int depth) {
    if (child.is_null()) {
        lines.push_back(std::move(marker_line));
        return;
    }
    if (child.is_scalar()) {
        marker_line += " ";
        marker_line += render_scalar(child);
        lines.push_back(std::move(marker_line));
        return;
    }
    if (is_empty_container(child)) {
        marker_line += " ";
        marker_line += empty_container_token(child);
        lines.push_back(std::move(marker_line));
        return;
    }
    // Non-empty map/seq: the marker line stands alone, the child follows at
    // depth + 1.
    lines.push_back(std::move(marker_line));
    std::vector<std::string> child_lines = render_lines(child, depth + 1);
    lines.insert(lines.end(), child_lines.begin(), child_lines.end());
}

std::vector<std::string> render_lines(const YamlNode& node, int depth) {
    std::vector<std::string> lines;
    if (node.is_scalar()) {
        lines.push_back(indent(depth) + render_scalar(node));
        return lines;
    }
    if (node.is_null())
        return lines; // no standalone representation; callers never reach this for real content

    if (node.is_map()) {
        if (node.map.empty()) {
            lines.push_back(indent(depth) + "{}");
            return lines;
        }
        for (const YamlEntry& entry : node.map) {
            std::string marker = indent(depth) + render_key(entry.key) + ":";
            append_value(lines, marker, entry.node(), depth);
        }
        return lines;
    }

    // Sequence.
    if (node.seq.empty()) {
        lines.push_back(indent(depth) + "[]");
        return lines;
    }
    for (const YamlNode& element : node.seq) {
        if (element.is_null()) {
            lines.push_back(indent(depth) + "-");
            continue;
        }
        if (element.is_scalar()) {
            lines.push_back(indent(depth) + "- " + render_scalar(element));
            continue;
        }
        if (is_empty_container(element)) {
            lines.push_back(indent(depth) + "- " + std::string(empty_container_token(element)));
            continue;
        }
        // Non-empty map/seq under a dash: render at depth + 1, then splice
        // its first line's indent into the dash marker (both are exactly
        // one indent unit wide, so alignment holds for every following line
        // as-is).
        std::vector<std::string> child_lines = render_lines(element, depth + 1);
        const std::string first_indent = indent(depth + 1);
        const std::string& first = child_lines.front();
        lines.push_back(indent(depth) + "- " + first.substr(first_indent.size()));
        lines.insert(lines.end(), child_lines.begin() + 1, child_lines.end());
    }
    return lines;
}

} // namespace

std::string emit_yaml(const YamlNode& root) {
    const std::vector<std::string> lines = render_lines(root, 0);
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace midday::loader

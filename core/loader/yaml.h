// core/loader/yaml.h — the strict-YAML surface of the PERMANENT loader
// subset (m0-yaml-loader-run). One parse entry point turns authored text
// into an OWNED first-party node tree with a 1-based source location on
// every node and every mapping key; vendored rapidyaml is quarantined
// behind it (yaml_parse.cpp is the only TU that sees a ryml type).
//
// STRICTNESS IS THE PRODUCT (spec section 8). The wrapper refuses, with
// file:line:col, everything the agent formats never use:
//   * multiple documents / explicit stream markers carrying >1 doc
//   * anchors, aliases, and tags (no invisible aliasing, no type coercion
//     side channel)
//   * duplicate mapping keys (last-one-wins is silent data loss)
//   * non-scalar mapping keys
// Scalars stay RAW TEXT plus a quoted flag: interpretation (int / float /
// bool / name / string) is the loader's job at the point of use, so a type
// mismatch reports the FIELD's location and the allowed spelling — the
// parser never guesses types (no YAML implicit-typing surprises; "no" is a
// string, not a bool).
//
// Errors are structured base::Error values (never exceptions across this
// boundary): code "yaml.parse" (parser-level) or "yaml.strict" (subset
// refusals), message "<origin>:<line>:<col>: <what>", details
// {file, line, col}.

#pragma once

#include "core/base/error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::loader {

struct YamlNode;

// One mapping entry. Key order is authoring order — the deterministic
// iteration order everywhere (the core JSON object discipline).
struct YamlEntry {
    std::string key = {};
    int key_line = 0; // 1-based
    int key_col = 0;  // 1-based
    // YamlNode is complete below; vector<> members make the mutual recursion
    // well-formed. node() is defined after YamlNode: front() does pointer
    // arithmetic, which libstdc++ rejects on an incomplete element type.
    std::vector<YamlNode> value = {}; // exactly one element (vector = indirection)

    [[nodiscard]] const YamlNode& node() const;
};

struct YamlNode {
    enum class Kind : std::uint8_t {
        kNull = 0, // empty value ("key:" with nothing after it)
        kScalar,
        kMap,
        kSeq,
    };

    Kind kind = Kind::kNull;
    int line = 0; // 1-based position of the node's first token
    int col = 0;
    std::string scalar;              // kScalar: the raw text, escapes resolved
    bool quoted = false;             // kScalar: was quoted — always a string, never a
                                     // number/bool, and empty-string capable
    std::vector<YamlEntry> map = {}; // kMap entries, authoring order
    std::vector<YamlNode> seq = {};  // kSeq elements, authoring order

    [[nodiscard]] bool is_map() const { return kind == Kind::kMap; }

    [[nodiscard]] bool is_seq() const { return kind == Kind::kSeq; }

    [[nodiscard]] bool is_scalar() const { return kind == Kind::kScalar; }

    [[nodiscard]] bool is_null() const { return kind == Kind::kNull; }

    // kMap lookup; nullptr when absent or not a map.
    [[nodiscard]] const YamlNode* find(std::string_view key) const {
        if (kind != Kind::kMap)
            return nullptr;
        for (const YamlEntry& entry : map)
            if (entry.key == key)
                return &entry.node();
        return nullptr;
    }
};

inline const YamlNode& YamlEntry::node() const {
    return value.front();
}

struct YamlParseResult {
    YamlNode root;
    std::optional<base::Error> error; // engaged = root is meaningless
};

// Strict parse of exactly one YAML document. `origin` is the file path (or
// source label) carried into every diagnostic. An empty / comments-only
// document parses to a kNull root.
YamlParseResult parse_yaml(std::string_view text, std::string_view origin);

// read_file + parse_yaml. Unreadable file -> "loader.io".
YamlParseResult parse_yaml_file(const std::string& path);

} // namespace midday::loader

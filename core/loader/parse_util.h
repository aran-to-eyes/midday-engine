// core/loader/parse_util.h — INTERNAL strict-field helpers shared by the
// three format loaders (events/machine/scene). Not installed API: the
// public surface is loader.h. Every helper reports the offending node's
// file:line:col and, for key errors, the allowed vocabulary — the loader's
// "strictness is the product" contract lives here so every format refuses
// identically.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/loader/yaml.h"
#include "core/math/vec.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace midday::loader::detail {

// "<file>:<line>:<col>: <what>" + details {file, line, col}.
base::Error
err_at(std::string_view code, std::string_view file, int line, int col, const std::string& what);

inline base::Error err_node(std::string_view code,
                            std::string_view file,
                            const YamlNode& node,
                            const std::string& what) {
    return err_at(code, file, node.line, node.col, what);
}

// Every key of `map` must be in `allowed` (which is also the message's
// spelling list) -> "loader.unknown_key" at the offending KEY's location.
std::optional<base::Error>
check_keys(const YamlNode& map, std::string_view file, std::span<const std::string_view> allowed);

// Required field: "loader.bad_value" at the map's location when absent.
struct FieldResult {
    const YamlNode* node = nullptr;
    std::optional<base::Error> error;
};

FieldResult require_field(const YamlNode& map,
                          std::string_view file,
                          std::string_view key,
                          std::string_view context);

// ---- typed scalar reads (strict; quoted scalars are strings ONLY) ----------
template <typename T> struct Parsed {
    T value{};
    std::optional<base::Error> error;
};

Parsed<std::string> get_string(const YamlNode& node, std::string_view file);
// Non-empty scalar (interned by callers as base::Name).
Parsed<std::string> get_name(const YamlNode& node, std::string_view file);
Parsed<bool> get_bool(const YamlNode& node, std::string_view file);
Parsed<std::int64_t> get_int(const YamlNode& node, std::string_view file);
Parsed<double> get_float(const YamlNode& node, std::string_view file);
// Sequence of exactly three numbers.
Parsed<math::Vec3> get_vec3(const YamlNode& node, std::string_view file);

// YAML subtree -> JSON value (event payload literals): unquoted scalars
// type as int -> float -> bool, everything else (and every quoted scalar)
// stays a string; maps/seqs recurse; null values refuse.
Parsed<base::Json> yaml_to_json(const YamlNode& node, std::string_view file);

// The `format: N` gate every text format carries (spec section 8): absent
// -> refused (loudly versioned from day one), non-integer or future ->
// refused ("loader.bad_format").
std::optional<base::Error>
check_format(const YamlNode& root, std::string_view file, std::string_view format_name);

} // namespace midday::loader::detail

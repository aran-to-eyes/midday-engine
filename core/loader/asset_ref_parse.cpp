// core/loader/asset_ref_parse.cpp — asset_ref_parse.h.

#include "core/loader/asset_ref_parse.h"

#include "core/loader/parse_util.h"

#include <array>
#include <filesystem>
#include <utility>

namespace midday::loader {

using detail::err_node;
using detail::Parsed;

AssetRefParseResult
parse_asset_ref(const YamlNode& node, std::string_view file, const std::string& root_dir) {
    AssetRefParseResult out;
    if (!node.is_map()) {
        out.error = err_node("loader.bad_value", file, node, "expected a {uid?, path} mapping");
        return out;
    }
    static constexpr std::array<std::string_view, 2> kAllowed = {"uid", "path"};
    if (auto error = detail::check_keys(node, file, kAllowed)) {
        out.error = std::move(error);
        return out;
    }
    detail::FieldResult path_field =
        detail::require_field(node, file, "path", "an asset reference");
    if (path_field.error.has_value()) {
        out.error = std::move(path_field.error);
        return out;
    }
    Parsed<std::string> path_text = detail::get_name(*path_field.node, file);
    if (path_text.error.has_value()) {
        out.error = std::move(path_text.error);
        return out;
    }
    out.ref.path_authored = path_text.value;
    out.ref.path_resolved = (std::filesystem::path(root_dir) / path_text.value).generic_string();
    out.ref.line = node.line;
    out.ref.col = node.col;
    if (const YamlNode* uid = node.find("uid")) {
        Parsed<std::string> uid_text = detail::get_name(*uid, file);
        if (uid_text.error.has_value()) {
            out.error = std::move(uid_text.error);
            return out;
        }
        out.ref.uid = uid_text.value;
        out.ref.has_uid = true;
    }
    std::error_code exists_ec;
    out.ref.exists = std::filesystem::is_regular_file(out.ref.path_resolved, exists_ec);
    return out;
}

AssetRefParseResult
parse_path_only_ref(const YamlNode& node, std::string_view file, const std::string& root_dir) {
    AssetRefParseResult out;
    Parsed<std::string> text = detail::get_name(node, file);
    if (text.error.has_value()) {
        out.error = std::move(text.error);
        return out;
    }
    out.ref.path_authored = text.value;
    out.ref.path_resolved = (std::filesystem::path(root_dir) / text.value).generic_string();
    out.ref.line = node.line;
    out.ref.col = node.col;
    std::error_code exists_ec;
    out.ref.exists = std::filesystem::is_regular_file(out.ref.path_resolved, exists_ec);
    return out;
}

Gap missing_asset_gap(std::string_view kind, const AssetRefDesc& ref, std::string_view file) {
    return Gap{.kind = std::string(kind),
               .what = ref.path_authored,
               .file = std::string(file),
               .line = ref.line,
               .col = ref.col,
               .detail = std::string(kind) + " '" + ref.path_authored + "' does not exist yet"};
}

} // namespace midday::loader

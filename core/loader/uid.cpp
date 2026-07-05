// core/loader/uid.cpp — uid.h: textual form, minting, and .uid sidecar I/O.

#include "core/loader/uid.h"

#include "core/base/file_io.h"
#include "core/loader/parse_util.h"
#include "core/loader/yaml_emit.h"

#include <array>
#include <limits>
#include <utility>

namespace midday::loader {
namespace {

constexpr std::string_view kPrefix = "uid://";
constexpr std::string_view kAlphabet = "0123456789abcdefghijklmnopqrstuvwxyz";
constexpr int kBase = 36;
constexpr int kTextWidth = 13; // ceil(log36(2^64)) = 13; covers the full 64-bit range

int digit_of(char c) {
    const std::string_view::size_type pos = kAlphabet.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

// A leaf {format, uid} map, built directly (never parsed) for write_uid_sidecar.
YamlNode make_scalar(std::string text) {
    YamlNode node;
    node.kind = YamlNode::Kind::kScalar;
    node.scalar = std::move(text);
    return node;
}

YamlEntry make_entry(std::string key, YamlNode value) {
    YamlEntry entry;
    entry.key = std::move(key);
    entry.value.push_back(std::move(value));
    return entry;
}

constexpr std::array<std::string_view, 2> kSidecarKeys = {"format", "uid"};

} // namespace

std::string Uid::text() const {
    std::string body(kTextWidth, '0');
    std::uint64_t remaining = value_;
    for (int i = kTextWidth - 1; i >= 0; --i) {
        body[static_cast<std::size_t>(i)] = kAlphabet[remaining % kBase];
        remaining /= kBase;
    }
    return std::string(kPrefix) + body;
}

std::optional<Uid> parse_uid_text(std::string_view text) {
    if (!text.starts_with(kPrefix))
        return std::nullopt;
    const std::string_view body = text.substr(kPrefix.size());
    if (body.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : body) {
        const int digit = digit_of(c);
        if (digit < 0)
            return std::nullopt;
        constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
        if (value > (kMax - static_cast<std::uint64_t>(digit)) / kBase)
            return std::nullopt; // would overflow 64 bits
        value = value * kBase + static_cast<std::uint64_t>(digit);
    }
    return Uid::from_value(value);
}

UidRng make_uid_rng() {
    std::random_device entropy;
    // random_device's own result_type may be narrower than mt19937_64's
    // seed; two draws folded together give it the full 64 bits to work with.
    const std::uint64_t hi = entropy();
    const std::uint64_t lo = entropy();
    return UidRng((hi << 32) ^ lo);
}

Uid mint_uid(const std::unordered_set<std::uint64_t>& taken, UidRng& rng) {
    std::uniform_int_distribution<std::uint64_t> draw(1,
                                                      0x7FFFFFFFFFFFFFFFULL); // never 0 (invalid)
    while (true) {
        const std::uint64_t candidate = draw(rng);
        if (!taken.contains(candidate))
            return Uid::from_value(candidate);
    }
}

std::string sidecar_path_for(std::string_view asset_path) {
    return std::string(asset_path) + ".uid";
}

SidecarLoadResult load_uid_sidecar(const std::string& path) {
    SidecarLoadResult out;
    YamlParseResult parsed = parse_yaml_file(path);
    if (parsed.error.has_value()) {
        out.error = std::move(parsed.error);
        return out;
    }
    if (auto error = detail::check_format(parsed.root, path, "uid sidecar")) {
        out.error = std::move(error);
        return out;
    }
    if (auto error = detail::check_keys(parsed.root, path, kSidecarKeys)) {
        out.error = std::move(error);
        return out;
    }
    detail::FieldResult uid_field =
        detail::require_field(parsed.root, path, "uid", "a uid sidecar");
    if (uid_field.error.has_value()) {
        out.error = std::move(uid_field.error);
        return out;
    }
    detail::Parsed<std::string> uid_text = detail::get_string(*uid_field.node, path);
    if (uid_text.error.has_value()) {
        out.error = std::move(uid_text.error);
        return out;
    }
    const std::optional<Uid> uid = parse_uid_text(uid_text.value);
    if (!uid.has_value() || !uid->valid()) {
        out.error = detail::err_node(
            "uid.malformed", path, *uid_field.node, "'" + uid_text.value + "' is not a valid uid");
        return out;
    }
    out.sidecar = UidSidecar{.uid = *uid};
    return out;
}

std::optional<base::Error> write_uid_sidecar(const std::string& path, Uid uid) {
    YamlNode root;
    root.kind = YamlNode::Kind::kMap;
    root.map.push_back(make_entry("format", make_scalar("1")));
    root.map.push_back(make_entry("uid", make_scalar(uid.text())));
    return base::write_file(path, emit_yaml(root), "uid.io");
}

} // namespace midday::loader

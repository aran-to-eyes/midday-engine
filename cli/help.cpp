// cli/help.cpp — help generation from verb metadata. Deterministic by
// construction: declaration order for flags/positionals, manifest order for
// the verb list, no environment input anywhere.

#include "cli/help.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace midday::cli {
namespace {

bool is_bool_flag(const FlagSpec& flag) {
    return flag.type == "bool";
}

Json flag_schema(const FlagSpec& flag) {
    Json schema = Json::object();
    schema.set("name", flag.name);
    schema.set("type", flag.type);
    schema.set("required", flag.required);
    if (flag.default_text != nullptr) {
        std::optional<Json> value = arg_value_from_text(flag.type, flag.default_text);
        schema.set("default", value ? std::move(*value) : Json(flag.default_text));
    }
    schema.set("doc", flag.doc);
    return schema;
}

Json positional_schema(const PositionalSpec& pos) {
    Json schema = Json::object();
    schema.set("name", pos.name);
    schema.set("type", pos.type);
    schema.set("required", pos.required);
    schema.set("variadic", pos.variadic);
    schema.set("doc", pos.doc);
    return schema;
}

std::string flag_column(const FlagSpec& flag) {
    std::string text = "--" + std::string(flag.name);
    if (!is_bool_flag(flag))
        text += " <" + std::string(flag.type) + ">";
    return text;
}

std::string positional_token(const PositionalSpec& pos) {
    std::string token = "<" + std::string(pos.name) + ">";
    if (pos.variadic)
        token += "...";
    if (!pos.required)
        token = "[" + token + "]";
    return token;
}

std::size_t flag_column_width(std::span<const FlagSpec> flags) {
    std::size_t width = 0;
    for (const FlagSpec& flag : flags)
        width = std::max(width, flag_column(flag).size());
    return width;
}

void append_row(std::string& out,
                const std::string& left,
                std::size_t width,
                std::string_view doc) {
    out += "\n  " + left + std::string(width - left.size() + 2, ' ') + std::string(doc);
}

void append_flag_rows(std::string& out, std::span<const FlagSpec> flags, std::size_t width) {
    for (const FlagSpec& flag : flags)
        append_row(out, flag_column(flag), width, flag.doc);
}

std::string usage_line(const VerbSpec& spec) {
    std::string line = "usage: midday " + std::string(spec.name);
    for (const FlagSpec& flag : spec.flags) {
        const std::string column = flag_column(flag);
        line += flag.required ? " " + column : " [" + column + "]";
    }
    for (const PositionalSpec& pos : spec.positionals)
        line += " " + positional_token(pos);
    line += " [--json]";
    return line;
}

std::string human_verb_help(const VerbSpec& spec) {
    std::string text = "midday " + std::string(spec.name) + " - " + std::string(spec.summary);
    text += "\n" + usage_line(spec);
    const std::size_t width =
        std::max(flag_column_width(spec.flags), flag_column_width(global_flags()));
    if (!spec.flags.empty()) {
        text += "\nflags:";
        append_flag_rows(text, spec.flags, width);
    }
    if (!spec.positionals.empty()) {
        text += "\narguments:";
        std::size_t pos_width = 0;
        for (const PositionalSpec& pos : spec.positionals)
            pos_width = std::max(pos_width, positional_token(pos).size());
        for (const PositionalSpec& pos : spec.positionals)
            append_row(text, positional_token(pos), pos_width, pos.doc);
    }
    text += "\nglobal flags:";
    append_flag_rows(text, global_flags(), width);
    return text;
}

std::string human_overview() {
    std::string text =
        "midday - Midday Engine CLI (exit codes: 0 ok, 1 failure, 2 usage, 3 validation)";
    text += "\nusage: midday <verb> [flags] [args] [--json]";
    text += "\n       midday help <verb>";
    text += "\nverbs:";
    std::size_t width = 0;
    for (const VerbSpec* spec : verbs())
        width = std::max(width, spec->name.size());
    for (const VerbSpec* spec : verbs())
        append_row(text, std::string(spec->name), width, spec->summary);
    text += "\nglobal flags:";
    append_flag_rows(text, global_flags(), flag_column_width(global_flags()));
    return text;
}

} // namespace

Json verb_schema(const VerbSpec& spec) {
    Json schema = Json::object();
    schema.set("name", spec.name);
    schema.set("summary", spec.summary);
    Json flags = Json::array();
    for (const FlagSpec& flag : spec.flags)
        flags.push(flag_schema(flag));
    schema.set("flags", std::move(flags));
    Json positionals = Json::array();
    for (const PositionalSpec& pos : spec.positionals)
        positionals.push(positional_schema(pos));
    schema.set("positionals", std::move(positionals));
    return schema;
}

Json global_flags_json() {
    Json flags = Json::array();
    for (const FlagSpec& flag : global_flags())
        flags.push(flag_schema(flag));
    return flags;
}

VerbOutcome help_for(const VerbSpec& spec) {
    VerbOutcome out;
    out.payload = verb_schema(spec);
    out.payload.set("globals", global_flags_json());
    out.human = human_verb_help(spec);
    return out;
}

VerbOutcome help_overview() {
    VerbOutcome out;
    Json list = Json::array();
    for (const VerbSpec* spec : verbs())
        list.push(verb_schema(*spec));
    out.payload.set("verbs", std::move(list));
    out.payload.set("globals", global_flags_json());
    out.human = human_overview();
    return out;
}

} // namespace midday::cli

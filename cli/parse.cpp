// cli/parse.cpp — framework argument parsing: argv -> Invocation -> typed,
// validated VerbArgs. Every usage.* error is produced HERE (exit 2 with
// structured details); verbs never parse or reject arguments themselves.
// Semantics pinned by D-BUILD-039: flag types are the reflect TypeDesc scalar
// subset, numeric values use the strict core JSON number grammar, repeating a
// value flag is an error (bool switches are idempotent), and --help anywhere
// wins over usage errors.

#include "cli/verb.h"
#include "core/reflect/type_model.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace midday::cli {
namespace {

using reflect::TypeDesc;
using reflect::TypeKind;

[[noreturn]] void fatal(const std::string& message) {
    std::fprintf(stderr, "midday: fatal: cli: %s\n", message.c_str());
    std::abort();
}

TypeKind kind_of(std::string_view spelling) {
    std::optional<TypeDesc> desc = TypeDesc::parse(spelling);
    if (!desc)
        fatal("unparsable type spelling '" + std::string(spelling) + "'");
    return desc->kind();
}

bool flag_kind_allowed(TypeKind kind) {
    return kind == TypeKind::kBool || kind == TypeKind::kInt || kind == TypeKind::kFloat ||
           kind == TypeKind::kString || kind == TypeKind::kName;
}

bool positional_kind_allowed(TypeKind kind) {
    return kind != TypeKind::kBool && flag_kind_allowed(kind);
}

bool valid_arg_name(std::string_view name) {
    return !name.empty() && name.front() != '-' && name.find('=') == std::string_view::npos;
}

Error usage_error(std::string_view code, std::string message) {
    Error error;
    error.code = std::string(code);
    error.message = std::move(message);
    return error;
}

bool is_global_flag(std::string_view name) {
    return name == "json" || name == "help";
}

const FlagSpec* find_flag(const VerbSpec& spec, std::string_view name) {
    for (const FlagSpec& flag : global_flags())
        if (flag.name == name)
            return &flag;
    for (const FlagSpec& flag : spec.flags)
        if (flag.name == name)
            return &flag;
    return nullptr;
}

// Deterministic help for usage.unknown_flag: verb flags in declaration
// order, then the framework globals.
Json known_flags(const VerbSpec& spec) {
    Json names = Json::array();
    for (const FlagSpec& flag : spec.flags)
        names.push(flag.name);
    for (const FlagSpec& flag : global_flags())
        names.push(flag.name);
    return names;
}

Error invalid_flag_value(std::string_view flag, std::string_view type, std::string_view text) {
    Error error = usage_error("usage.invalid_flag_value",
                              "flag --" + std::string(flag) + " expects <" + std::string(type) +
                                  ">, got '" + std::string(text) + "'");
    error.details.set("flag", flag);
    error.details.set("type", type);
    error.details.set("value", text);
    return error;
}

// The declared schema entry behind an accessor name (flag or positional).
struct DeclaredArg {
    std::string_view type;
    bool variadic = false;
};

DeclaredArg declared(const VerbSpec* spec, std::string_view name) {
    if (spec == nullptr)
        fatal("VerbArgs used without a bound spec (not built by the framework)");
    for (const FlagSpec& flag : spec->flags)
        if (flag.name == name)
            return {.type = flag.type};
    for (const PositionalSpec& pos : spec->positionals)
        if (pos.name == name)
            return {.type = pos.type, .variadic = pos.variadic};
    fatal("verb '" + std::string(spec->name) + "' declares no argument named '" +
          std::string(name) + "'");
}

} // namespace

// Parse-time construction seam: parsing stays free code, VerbArgs stays
// read-only for verbs.
struct ArgAssembler {
    static void bind(VerbArgs& args, const VerbSpec& spec) { args.spec_ = &spec; }

    static void put(VerbArgs& args, std::string_view name, Json value) {
        args.values_.emplace_back(name, std::move(value));
    }

    static bool has(const VerbArgs& args, std::string_view name) {
        return args.find_value(name) != nullptr;
    }
};

std::span<const FlagSpec> global_flags() {
    static constexpr FlagSpec kGlobals[] = {
        {.name = "json",
         .type = "bool",
         .doc = "emit the machine-readable JSON envelope on stdout (accepted anywhere)"},
        {.name = "help",
         .type = "bool",
         .doc = "print generated help (with --json: the verb's machine schema)"},
    };
    return kGlobals;
}

std::optional<Json> arg_value_from_text(std::string_view type, std::string_view text) {
    switch (kind_of(type)) {
    case TypeKind::kBool:
        if (text == "true")
            return Json(true);
        if (text == "false")
            return Json(false);
        return std::nullopt;
    case TypeKind::kInt: {
        Json::ParseResult parsed = Json::parse(text);
        if (!parsed || !parsed.value.is_int())
            return std::nullopt;
        return std::move(parsed.value);
    }
    case TypeKind::kFloat: {
        Json::ParseResult parsed = Json::parse(text);
        if (!parsed || !parsed.value.is_number())
            return std::nullopt;
        return Json(parsed.value.as_double());
    }
    case TypeKind::kString:
    case TypeKind::kName:
        return Json(text);
    default:
        return std::nullopt; // other kinds never pass validate_spec
    }
}

Invocation split_invocation(int argc, const char* const* argv) {
    Invocation inv;
    int i = 1;
    for (; i < argc; ++i) {
        const std::string_view tok = argv[i];
        if (tok == "--json") {
            inv.json = true;
            continue;
        }
        if (tok == "--help") {
            inv.help = true;
            continue;
        }
        if (tok.starts_with("--")) {
            Error error = usage_error("usage.unknown_flag",
                                      "only --json/--help may precede the verb, got '" +
                                          std::string(tok) + "'");
            error.details.set("flag", tok);
            inv.usage = std::move(error);
            return inv;
        }
        inv.verb = tok;
        ++i;
        break;
    }
    for (; i < argc; ++i)
        inv.rest.emplace_back(argv[i]);
    return inv;
}

ParsedArgs parse_verb_args(const VerbSpec& spec, const Invocation& inv) {
    ParsedArgs out;
    ArgAssembler::bind(out.args, spec);
    out.args.json = inv.json;
    out.help = inv.help;

    auto fail = [&out](Error error) {
        if (!out.usage)
            out.usage = std::move(error);
    };

    std::size_t pos_index = 0;
    Json variadic_values = Json::array();
    bool variadic_seen = false;

    for (std::size_t i = 0; i < inv.rest.size(); ++i) {
        const std::string_view tok = inv.rest[i];
        if (out.usage) {
            // Salvage pass after the first error: only the literal global
            // switches still matter (--help must stay reachable).
            if (tok == "--help")
                out.help = true;
            else if (tok == "--json")
                out.args.json = true;
            continue;
        }
        if (tok.size() > 2 && tok.starts_with("--")) {
            const std::string_view body = tok.substr(2);
            std::string_view name = body;
            std::optional<std::string_view> inline_value;
            if (const std::size_t eq = body.find('='); eq != std::string_view::npos) {
                name = body.substr(0, eq);
                inline_value = body.substr(eq + 1);
            }
            const FlagSpec* flag = find_flag(spec, name);
            if (flag == nullptr) {
                Error error = usage_error("usage.unknown_flag",
                                          "unknown flag --" + std::string(name) + " for verb '" +
                                              std::string(spec.name) + "'");
                error.details.set("flag", name);
                error.details.set("known", known_flags(spec));
                fail(std::move(error));
                continue;
            }
            const TypeKind kind = kind_of(flag->type);
            Json value;
            if (kind == TypeKind::kBool) {
                value = Json(true);
                if (inline_value) {
                    std::optional<Json> parsed = arg_value_from_text("bool", *inline_value);
                    if (!parsed) {
                        fail(invalid_flag_value(name, flag->type, *inline_value));
                        continue;
                    }
                    value = std::move(*parsed);
                }
            } else {
                std::string_view text;
                if (inline_value) {
                    text = *inline_value;
                } else if (i + 1 < inv.rest.size()) {
                    text = inv.rest[++i];
                } else {
                    Error error = usage_error("usage.missing_flag_value",
                                              "flag --" + std::string(name) + " needs a <" +
                                                  std::string(flag->type) + "> value");
                    error.details.set("flag", name);
                    error.details.set("type", flag->type);
                    fail(std::move(error));
                    continue;
                }
                std::optional<Json> parsed = arg_value_from_text(flag->type, text);
                if (!parsed) {
                    fail(invalid_flag_value(name, flag->type, text));
                    continue;
                }
                value = std::move(*parsed);
            }
            if (is_global_flag(name)) {
                if (name == "json")
                    out.args.json = value.as_bool();
                else
                    out.help = value.as_bool();
                continue;
            }
            if (ArgAssembler::has(out.args, flag->name)) {
                if (kind == TypeKind::kBool)
                    continue; // repeating a switch is idempotent
                Error error = usage_error("usage.duplicate_flag",
                                          "flag --" + std::string(name) + " given more than once");
                error.details.set("flag", name);
                fail(std::move(error));
                continue;
            }
            ArgAssembler::put(out.args, flag->name, std::move(value));
        } else {
            if (pos_index >= spec.positionals.size()) {
                Error error = usage_error("usage.unexpected_argument",
                                          "unexpected argument '" + std::string(tok) +
                                              "' for verb '" + std::string(spec.name) + "'");
                error.details.set("argument", tok);
                fail(std::move(error));
                continue;
            }
            const PositionalSpec& pos = spec.positionals[pos_index];
            std::optional<Json> parsed = arg_value_from_text(pos.type, tok);
            if (!parsed) {
                Error error =
                    usage_error("usage.invalid_argument_value",
                                "argument <" + std::string(pos.name) + "> expects <" +
                                    std::string(pos.type) + ">, got '" + std::string(tok) + "'");
                error.details.set("argument", pos.name);
                error.details.set("type", pos.type);
                error.details.set("value", tok);
                fail(std::move(error));
                continue;
            }
            if (pos.variadic) {
                variadic_values.push(std::move(*parsed));
                variadic_seen = true;
            } else {
                ArgAssembler::put(out.args, pos.name, std::move(*parsed));
                ++pos_index;
            }
        }
    }

    if (variadic_seen)
        ArgAssembler::put(out.args, spec.positionals.back().name, std::move(variadic_values));

    if (out.help || out.usage)
        return out; // --help skips completeness checks; errors are final

    Json missing = Json::array();
    std::string missing_names;
    for (const FlagSpec& flag : spec.flags) {
        if (flag.required && !ArgAssembler::has(out.args, flag.name)) {
            missing.push(flag.name);
            missing_names += (missing_names.empty() ? "--" : ", --") + std::string(flag.name);
        }
    }
    if (!missing.elements().empty()) {
        Error error =
            usage_error("usage.missing_flag", "missing required flag(s): " + missing_names);
        error.details.set("missing", std::move(missing));
        out.usage = std::move(error);
        return out;
    }

    for (const FlagSpec& flag : spec.flags) {
        if (flag.default_text == nullptr || ArgAssembler::has(out.args, flag.name))
            continue;
        std::optional<Json> value = arg_value_from_text(flag.type, flag.default_text);
        if (!value) // unreachable behind validate_spec; kept loud, not silent
            fatal("default for --" + std::string(flag.name) + " does not inhabit " +
                  std::string(flag.type));
        ArgAssembler::put(out.args, flag.name, std::move(*value));
    }

    for (const PositionalSpec& pos : spec.positionals) {
        const bool given = pos.variadic ? variadic_seen : ArgAssembler::has(out.args, pos.name);
        if (pos.required && !given) {
            Error error = usage_error("usage.missing_argument",
                                      "missing required argument <" + std::string(pos.name) + ">");
            error.details.set("argument", pos.name);
            out.usage = std::move(error);
            return out;
        }
    }
    return out;
}

std::optional<Error> validate_spec(const VerbSpec& spec) {
    auto spec_error = [&spec](std::string_view code, const std::string& message) {
        Error error;
        error.code = std::string(code);
        error.message = "verb '" + std::string(spec.name) + "': " + message;
        return error;
    };

    if (!valid_arg_name(spec.name))
        return spec_error("spec.bad_name", "verb name is empty or malformed");
    if (spec.run == nullptr)
        return spec_error("spec.no_run", "no run function");

    std::vector<std::string_view> taken = {"json", "help"}; // framework globals are reserved
    auto claim = [&taken](std::string_view name) {
        if (std::ranges::find(taken, name) != taken.end())
            return false;
        taken.push_back(name);
        return true;
    };

    for (const FlagSpec& flag : spec.flags) {
        const std::string label = "flag --" + std::string(flag.name);
        if (!valid_arg_name(flag.name))
            return spec_error("spec.bad_name", label + ": malformed name");
        if (!claim(flag.name))
            return spec_error("spec.duplicate_name",
                              label + ": name already taken (--json/--help are reserved)");
        const std::optional<TypeDesc> desc = TypeDesc::parse(flag.type);
        if (!desc || !flag_kind_allowed(desc->kind()))
            return spec_error("spec.bad_type",
                              label + ": type '" + std::string(flag.type) +
                                  "' is not bool|int|float|string|name");
        const bool is_bool = desc->kind() == TypeKind::kBool;
        if (is_bool && flag.required)
            return spec_error("spec.bool_flag_required",
                              label + ": a bool switch cannot be required (absence means false)");
        if (is_bool && flag.default_text != nullptr)
            return spec_error("spec.bool_flag_default",
                              label + ": a bool switch cannot declare a default");
        if (flag.required && flag.default_text != nullptr)
            return spec_error("spec.required_has_default",
                              label + ": required and defaulted are contradictory");
        if (flag.default_text != nullptr && !arg_value_from_text(flag.type, flag.default_text))
            return spec_error("spec.bad_default",
                              label + ": default '" + std::string(flag.default_text) +
                                  "' does not inhabit " + std::string(flag.type));
    }

    bool seen_optional = false;
    for (std::size_t i = 0; i < spec.positionals.size(); ++i) {
        const PositionalSpec& pos = spec.positionals[i];
        const std::string label = "positional <" + std::string(pos.name) + ">";
        if (!valid_arg_name(pos.name))
            return spec_error("spec.bad_name", label + ": malformed name");
        if (!claim(pos.name))
            return spec_error("spec.duplicate_name", label + ": name already taken");
        const std::optional<TypeDesc> desc = TypeDesc::parse(pos.type);
        if (!desc || !positional_kind_allowed(desc->kind()))
            return spec_error("spec.bad_type",
                              label + ": type '" + std::string(pos.type) +
                                  "' is not int|float|string|name");
        if (pos.variadic && i + 1 != spec.positionals.size())
            return spec_error("spec.variadic_not_last", label + ": variadic must be last");
        if (!pos.required)
            seen_optional = true;
        else if (seen_optional)
            return spec_error("spec.positional_order",
                              label + ": required positional after an optional one");
    }
    return std::nullopt;
}

const Json* VerbArgs::find_value(std::string_view name) const {
    for (const auto& [key, value] : values_)
        if (key == name)
            return &value;
    return nullptr;
}

bool VerbArgs::present(std::string_view name) const {
    declared(spec_, name); // aborts on undeclared names
    return find_value(name) != nullptr;
}

bool VerbArgs::get_bool(std::string_view name) const {
    if (kind_of(declared(spec_, name).type) != TypeKind::kBool)
        fatal("get_bool on non-bool argument '" + std::string(name) + "'");
    const Json* value = find_value(name);
    return value != nullptr && value->as_bool();
}

std::int64_t VerbArgs::get_int(std::string_view name) const {
    if (kind_of(declared(spec_, name).type) != TypeKind::kInt)
        fatal("get_int on non-int argument '" + std::string(name) + "'");
    const Json* value = find_value(name);
    if (value == nullptr)
        fatal("argument '" + std::string(name) + "' is absent; guard with present()");
    return value->as_int();
}

double VerbArgs::get_float(std::string_view name) const {
    if (kind_of(declared(spec_, name).type) != TypeKind::kFloat)
        fatal("get_float on non-float argument '" + std::string(name) + "'");
    const Json* value = find_value(name);
    if (value == nullptr)
        fatal("argument '" + std::string(name) + "' is absent; guard with present()");
    return value->as_double();
}

const std::string& VerbArgs::get_string(std::string_view name) const {
    const TypeKind kind = kind_of(declared(spec_, name).type);
    if (kind != TypeKind::kString && kind != TypeKind::kName)
        fatal("get_string on non-string/name argument '" + std::string(name) + "'");
    const Json* value = find_value(name);
    if (value == nullptr)
        fatal("argument '" + std::string(name) + "' is absent; guard with present()");
    return value->as_string();
}

const Json::Array& VerbArgs::get_list(std::string_view name) const {
    if (!declared(spec_, name).variadic)
        fatal("get_list on non-variadic argument '" + std::string(name) + "'");
    const Json* value = find_value(name);
    static const Json kEmpty = Json::array();
    return (value != nullptr ? *value : kEmpty).elements();
}

} // namespace midday::cli

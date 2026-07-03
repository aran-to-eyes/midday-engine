#include "api/engine_api.h"

#include "core/base/hex.h"
#include "core/expr/functions.h"
#include "core/reflect/builtin_events.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace midday::api {
namespace {

using base::Error;
using base::Json;

// Section order is canonical and hashed: append new sections at the end.
struct Section {
    std::string_view key;  // document key ("classes")
    std::string_view kind; // diff-report kind word ("class")
};

constexpr Section kSections[] = {
    {"classes", "class"},
    {"events", "event"},
    {"functions", "function"},
    {"verbs", "verb"},
};

std::string hash_of(const Json& signature) {
    const std::string bytes = signature.dump();
    return base::hex64(XXH3_64bits(bytes.data(), bytes.size()));
}

// The signature subset of a described value: every "doc"/"summary" key
// dropped recursively (D-BUILD-021 — doc edits are not API drift).
Json strip_docs(const Json& value) {
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, member] : value.items())
            if (key != "doc" && key != "summary")
                out.set(key, strip_docs(member));
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& element : value.elements())
            out.push(strip_docs(element));
        return out;
    }
    return value;
}

// {"name","compat_hash"} per entry — the top-level hash's view of a section.
Json section_signature(const Json& entries) {
    Json out = Json::array();
    for (const Json& entry : entries.elements()) {
        Json item = Json::object();
        item.set("name", *entry.find("name"));
        item.set("compat_hash", *entry.find("compat_hash"));
        out.push(item);
    }
    return out;
}

bool is_hex64(const Json* value) {
    if (value == nullptr || !value->is_string() || value->as_string().size() != 16)
        return false;
    for (const char c : value->as_string())
        if (std::isdigit(static_cast<unsigned char>(c)) == 0 && (c < 'a' || c > 'f'))
            return false;
    return true;
}

Error malformed(std::string message) {
    return Error{.code = "api.malformed", .message = "engine_api document: " + std::move(message)};
}

Json diff_entry(std::string_view kind, const Json& name, const Json& hash) {
    Json out = Json::object();
    out.set("kind", kind);
    out.set("name", name);
    out.set("compat_hash", hash);
    return out;
}

} // namespace

BootRegistry::BootRegistry() : lifecycle(registry) {
    lifecycle.add_hooks(reflect::InitLevel::kCore,
                        {.initialize =
                             [](reflect::Registry& r) {
                                 reflect::register_builtin_events(r);
                                 expr::register_expr_functions(r);
                             },
                         .teardown = {}});
    lifecycle.initialize_to(reflect::InitLevel::kTools);
}

Json build_document(const reflect::Registry& registry,
                    const Json& verb_schemas,
                    std::string_view engine_version) {
    Json sections = registry.to_json(); // classes/events/functions, described

    Json verbs = Json::array();
    for (const Json& schema : verb_schemas.elements()) {
        Json entry = schema;
        entry.set("compat_hash", hash_of(strip_docs(schema)));
        verbs.push(std::move(entry));
    }
    sections.set("verbs", std::move(verbs));

    // Top-level hash: format version + every section's (name, compat_hash)
    // list in canonical order. engine_version and docs are OUT by
    // construction — the hash never sees them.
    Json signature = Json::object();
    signature.set("format_version", kFormatVersion);
    for (const Section& section : kSections)
        signature.set(section.key, section_signature(*sections.find(section.key)));

    Json document = Json::object();
    document.set("format_version", kFormatVersion);
    document.set("engine_version", engine_version);
    document.set("api_compat_hash", hash_of(signature));
    for (const Section& section : kSections)
        document.set(section.key, *sections.find(section.key));
    return document;
}

std::optional<Error> check_document(const Json& document) {
    if (!document.is_object())
        return malformed("not a JSON object");
    const Json* format = document.find("format_version");
    if (format == nullptr || !format->is_int())
        return malformed("missing integer format_version");
    if (format->as_int() != kFormatVersion)
        return malformed("unknown format_version " + format->dump() + " (this build reads " +
                         std::to_string(kFormatVersion) + ")");
    const Json* version = document.find("engine_version");
    if (version == nullptr || !version->is_string())
        return malformed("missing string engine_version");
    if (!is_hex64(document.find("api_compat_hash")))
        return malformed("api_compat_hash is not 16-digit lowercase hex");
    for (const Section& section : kSections) {
        const Json* entries = document.find(section.key);
        if (entries == nullptr || !entries->is_array())
            return malformed("missing array section '" + std::string(section.key) + "'");
        std::vector<std::string_view> names;
        for (const Json& entry : entries->elements()) {
            const std::string context = "section '" + std::string(section.key) + "': ";
            if (!entry.is_object())
                return malformed(context + "entry is not an object");
            const Json* name = entry.find("name");
            if (name == nullptr || !name->is_string() || name->as_string().empty())
                return malformed(context + "entry without a name");
            if (!is_hex64(entry.find("compat_hash")))
                return malformed(context + "entry '" + name->as_string() +
                                 "' compat_hash is not 16-digit lowercase hex");
            for (const std::string_view seen : names)
                if (seen == name->as_string())
                    return malformed(context + "duplicate entry '" + name->as_string() + "'");
            names.push_back(name->as_string());
        }
    }
    return std::nullopt;
}

Diff diff_documents(const Json& old_document, const Json& new_document) {
    Json added = Json::array();
    Json removed = Json::array();
    Json changed = Json::array();

    for (const Section& section : kSections) {
        const Json::Array& old_entries = old_document.find(section.key)->elements();
        const Json::Array& new_entries = new_document.find(section.key)->elements();
        auto find_in = [](const Json::Array& entries, const std::string& name) -> const Json* {
            for (const Json& entry : entries)
                if (entry.find("name")->as_string() == name)
                    return &entry;
            return nullptr;
        };
        for (const Json& entry : new_entries) {
            const Json* old_entry = find_in(old_entries, entry.find("name")->as_string());
            const Json& new_hash = *entry.find("compat_hash");
            if (old_entry == nullptr) {
                added.push(diff_entry(section.kind, *entry.find("name"), new_hash));
            } else if (old_entry->find("compat_hash")->as_string() != new_hash.as_string()) {
                Json item = Json::object();
                item.set("kind", section.kind);
                item.set("name", *entry.find("name"));
                item.set("old_compat_hash", *old_entry->find("compat_hash"));
                item.set("compat_hash", new_hash);
                changed.push(std::move(item));
            }
        }
        for (const Json& entry : old_entries)
            if (find_in(new_entries, entry.find("name")->as_string()) == nullptr)
                removed.push(
                    diff_entry(section.kind, *entry.find("name"), *entry.find("compat_hash")));
    }

    const std::string& old_hash = old_document.find("api_compat_hash")->as_string();
    const std::string& new_hash = new_document.find("api_compat_hash")->as_string();
    Diff diff;
    diff.identical = old_hash == new_hash && added.elements().empty() &&
                     removed.elements().empty() && changed.elements().empty();
    diff.report = Json::object();
    diff.report.set("identical", diff.identical);
    diff.report.set("old_api_compat_hash", old_hash);
    diff.report.set("api_compat_hash", new_hash);
    diff.report.set("added", std::move(added));
    diff.report.set("removed", std::move(removed));
    diff.report.set("changed", std::move(changed));
    return diff;
}

} // namespace midday::api

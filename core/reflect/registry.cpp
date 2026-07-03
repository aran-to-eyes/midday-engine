#include "core/reflect/registry.h"

#include "core/base/hex.h"
#include "core/reflect/fatal.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cctype>
#include <cstddef>
#include <utility>

namespace midday::reflect {
namespace {

using base::Json;
using base::Name;

// ---- canonical signature JSON → compat hash --------------------------------
// The signature is a JSON value serialized by the deterministic core writer
// (byte-identical across platforms, D-BUILD-015), hashed with XXH3-64 — the
// same primitive as Name ids. Docs are EXCLUDED: doc edits are not API drift.

std::uint64_t hash_of(const Json& signature) {
    const std::string bytes = signature.dump();
    return XXH3_64bits(bytes.data(), bytes.size());
}

Json flag_names(std::uint32_t flags) {
    // Bit order — append new flags at the end, never reorder.
    constexpr std::pair<std::uint32_t, const char*> kFlagNames[] = {
        {kPropertySave, "save"},
        {kPropertyReplicated, "replicated"},
        {kPropertyReadOnly, "read_only"},
    };
    Json out = Json::array();
    for (const auto& [bit, name] : kFlagNames)
        if ((flags & bit) != 0)
            out.push(name);
    return out;
}

Json param_json(const ParamDesc& param) {
    Json out = Json::object();
    out.set("name", param.name.view());
    out.set("type", param.type.canonical());
    if (!param.default_value.is_null())
        out.set("default", param.default_value);
    return out;
}

Json property_json(const PropertyDesc& property, bool with_docs) {
    Json out = Json::object();
    out.set("name", property.name.view());
    out.set("type", property.type.canonical());
    if (!property.default_value.is_null())
        out.set("default", property.default_value);
    if (property.flags != 0)
        out.set("flags", flag_names(property.flags));
    if (with_docs && !property.doc.empty())
        out.set("doc", property.doc);
    return out;
}

// Signature shape shared by class methods and free functions; `with_docs`
// additionally emits doc + compat_hash (the full-description form).
Json method_json(const MethodDesc& method, bool with_docs) {
    Json out = Json::object();
    out.set("name", method.name.view());
    Json params = Json::array();
    for (const ParamDesc& param : method.params)
        params.push(param_json(param));
    out.set("params", std::move(params));
    out.set("returns", method.returns ? Json(method.returns->canonical()) : Json("void"));
    if (with_docs) {
        if (!method.doc.empty())
            out.set("doc", method.doc);
        out.set("compat_hash", base::hex64(method.compat_hash));
    }
    return out;
}

Json event_field_json(const EventFieldDesc& field, bool with_docs) {
    Json out = Json::object();
    out.set("name", field.name.view());
    out.set("type", field.type.canonical());
    if (with_docs && !field.doc.empty())
        out.set("doc", field.doc);
    return out;
}

Json class_json(const ClassDesc& cls, InitLevel level, bool with_docs) {
    Json out = Json::object();
    out.set("name", cls.name.view());
    out.set("level", to_string(level));
    if (with_docs && !cls.doc.empty())
        out.set("doc", cls.doc);
    if (!cls.base.empty())
        out.set("base", cls.base.view());
    Json properties = Json::array();
    for (const PropertyDesc& property : cls.properties)
        properties.push(property_json(property, with_docs));
    out.set("properties", std::move(properties));
    Json methods = Json::array();
    for (const MethodDesc& method : cls.methods)
        methods.push(method_json(method, with_docs));
    out.set("methods", std::move(methods));
    if (with_docs)
        out.set("compat_hash", base::hex64(cls.compat_hash));
    return out;
}

Json event_json(const EventDesc& event, InitLevel level, bool with_docs) {
    Json out = Json::object();
    out.set("name", event.name.view());
    out.set("level", to_string(level));
    if (with_docs && !event.doc.empty())
        out.set("doc", event.doc);
    Json payload = Json::array();
    for (const EventFieldDesc& field : event.payload)
        payload.push(event_field_json(field, with_docs));
    out.set("payload", std::move(payload));
    if (with_docs)
        out.set("compat_hash", base::hex64(event.compat_hash));
    return out;
}

Json function_json(const MethodDesc& function, InitLevel level, bool with_docs) {
    // A free function is a method signature at registry scope; level slots
    // in after name so all top-level entries lead with (name, level).
    Json out = Json::object();
    const Json method = method_json(function, with_docs);
    for (const auto& [key, value] : method.items()) {
        out.set(key, value);
        if (key == "name")
            out.set("level", to_string(level));
    }
    return out;
}

// ---- registration-time validation (all failures abort loudly) --------------

std::string where(std::string_view kind_word, Name owner) {
    return std::string(kind_word) + " '" + std::string(owner.view()) + "': ";
}

void check_member_name(const std::string& context,
                       std::string_view member_word,
                       Name name,
                       std::vector<std::uint64_t>& seen) {
    if (name.empty())
        detail::fatal(context + "unnamed " + std::string(member_word));
    for (const std::uint64_t id : seen)
        if (id == name.id())
            detail::fatal(context + "duplicate " + std::string(member_word) + " '" +
                          std::string(name.view()) + "'");
    seen.push_back(name.id());
}

void check_default(const std::string& context, const TypeDesc& type, const Json& value) {
    if (!value.is_null() && !type.accepts(value))
        detail::fatal(context + "default " + value.dump() + " does not inhabit type '" +
                      type.canonical() + "'");
}

void validate_method(std::string_view kind_word, const MethodDesc& method) {
    const std::string context = where(kind_word, method.name);
    std::vector<std::uint64_t> seen;
    bool optional_seen = false;
    for (const ParamDesc& param : method.params) {
        check_member_name(context, "param", param.name, seen);
        check_default(context + "param '" + std::string(param.name.view()) + "': ",
                      param.type,
                      param.default_value);
        const bool optional = !param.default_value.is_null();
        if (optional_seen && !optional)
            detail::fatal(context + "required param '" + std::string(param.name.view()) +
                          "' follows an optional one");
        optional_seen = optional_seen || optional;
    }
}

void validate_class(const ClassDesc& cls) {
    const std::string context = where("class", cls.name);
    std::vector<std::uint64_t> properties_seen;
    for (const PropertyDesc& property : cls.properties) {
        check_member_name(context, "property", property.name, properties_seen);
        check_default(context + "property '" + std::string(property.name.view()) + "': ",
                      property.type,
                      property.default_value);
        if ((property.flags & ~kPropertyFlagsMask) != 0)
            detail::fatal(context + "unknown flag bits on property '" +
                          std::string(property.name.view()) + "'");
    }
    std::vector<std::uint64_t> methods_seen;
    for (const MethodDesc& method : cls.methods) {
        check_member_name(context, "method", method.name, methods_seen);
        validate_method("method", method);
    }
}

void validate_event(const EventDesc& event) {
    const std::string context = where("event", event.name);
    std::vector<std::uint64_t> seen;
    for (const EventFieldDesc& field : event.payload)
        check_member_name(context, "payload field", field.name, seen);
}

} // namespace

// ---- Registry ---------------------------------------------------------------

template <typename Desc>
const Registered<Desc>&
Registry::store(Bucket<Desc>& bucket, Desc desc, std::string_view kind_word) {
    if (desc.name.empty())
        detail::fatal("unnamed " + std::string(kind_word) + " registration");
    if (bucket.by_name.contains(desc.name.id()))
        detail::fatal("duplicate " + std::string(kind_word) + " registration: '" +
                      std::string(desc.name.view()) + "'");
    auto& level_entries = bucket.by_level[static_cast<std::size_t>(active_level_)];
    level_entries.push_back(
        std::make_unique<Registered<Desc>>(Registered<Desc>{std::move(desc), active_level_}));
    const Registered<Desc>* entry = level_entries.back().get();
    bucket.by_name.emplace(entry->desc.name.id(), entry);
    return *entry;
}

const Registered<ClassDesc>& Registry::add_class(ClassDesc desc) {
    validate_class(desc);
    for (MethodDesc& method : desc.methods)
        method.compat_hash = hash_of(method_json(method, /*with_docs=*/false));
    desc.compat_hash = hash_of(class_json(desc, active_level_, /*with_docs=*/false));
    return store(classes_, std::move(desc), "class");
}

const Registered<EventDesc>& Registry::add_event(EventDesc desc) {
    validate_event(desc);
    desc.compat_hash = hash_of(event_json(desc, active_level_, /*with_docs=*/false));
    return store(events_, std::move(desc), "event");
}

const Registered<MethodDesc>& Registry::add_function(MethodDesc desc) {
    validate_method("function", desc);
    desc.compat_hash = hash_of(function_json(desc, active_level_, /*with_docs=*/false));
    return store(functions_, std::move(desc), "function");
}

const Registered<ClassDesc>* Registry::find_class(Name name) const {
    const auto it = classes_.by_name.find(name.id());
    return it == classes_.by_name.end() ? nullptr : it->second;
}

const Registered<EventDesc>* Registry::find_event(Name name) const {
    const auto it = events_.by_name.find(name.id());
    return it == events_.by_name.end() ? nullptr : it->second;
}

const Registered<MethodDesc>* Registry::find_function(Name name) const {
    const auto it = functions_.by_name.find(name.id());
    return it == functions_.by_name.end() ? nullptr : it->second;
}

namespace {

template <typename Desc>
std::vector<const Registered<Desc>*> enumerate(
    const std::array<std::vector<std::unique_ptr<Registered<Desc>>>, kInitLevelCount>& by_level) {
    std::vector<const Registered<Desc>*> out;
    for (const auto& level_entries : by_level)
        for (const auto& entry : level_entries)
            out.push_back(entry.get());
    return out;
}

} // namespace

std::vector<const Registered<ClassDesc>*> Registry::classes() const {
    return enumerate(classes_.by_level);
}

std::vector<const Registered<EventDesc>*> Registry::events() const {
    return enumerate(events_.by_level);
}

std::vector<const Registered<MethodDesc>*> Registry::functions() const {
    return enumerate(functions_.by_level);
}

Json Registry::to_json() const {
    Json out = Json::object();
    Json class_list = Json::array();
    for (const auto* entry : classes())
        class_list.push(describe(*entry));
    out.set("classes", std::move(class_list));
    Json event_list = Json::array();
    for (const auto* entry : events())
        event_list.push(describe(*entry));
    out.set("events", std::move(event_list));
    Json function_list = Json::array();
    for (const auto* entry : functions())
        function_list.push(describe(*entry));
    out.set("functions", std::move(function_list));
    return out;
}

namespace {

template <typename Desc>
void remove_level_from(
    std::array<std::vector<std::unique_ptr<Registered<Desc>>>, kInitLevelCount>& by_level,
    std::unordered_map<std::uint64_t, const Registered<Desc>*>& by_name,
    InitLevel level) {
    auto& level_entries = by_level[static_cast<std::size_t>(level)];
    for (const auto& entry : level_entries)
        by_name.erase(entry->desc.name.id());
    level_entries.clear();
}

} // namespace

void Registry::remove_level(InitLevel level) {
    remove_level_from(classes_.by_level, classes_.by_name, level);
    remove_level_from(events_.by_level, events_.by_name, level);
    remove_level_from(functions_.by_level, functions_.by_name, level);
}

// ---- descriptions -----------------------------------------------------------

Json describe(const Registered<ClassDesc>& entry) {
    return class_json(entry.desc, entry.level, /*with_docs=*/true);
}

Json describe(const Registered<EventDesc>& entry) {
    return event_json(entry.desc, entry.level, /*with_docs=*/true);
}

Json describe(const Registered<MethodDesc>& entry) {
    return function_json(entry.desc, entry.level, /*with_docs=*/true);
}

std::string event_payload_type_name(std::string_view event_name) {
    std::string out;
    out.reserve(event_name.size());
    bool upper_next = true;
    for (const char c : event_name) {
        if (c == '.' || c == '_') {
            upper_next = true;
            continue;
        }
        out.push_back(upper_next ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                 : c);
        upper_next = false;
    }
    return out;
}

} // namespace midday::reflect

// tools/codegen_bootstrap/docs_emit.cpp — api_docs.md emitter: the generated
// reference agents read (every entry with docs + compat hash). Layout spec:
// api/CODEGEN.md "api_docs.md layout". Paragraphs are joined by exactly one
// blank line; doc text is flattened to one line; table cells escape '|'.

#include "tools/codegen_bootstrap/codegen.h"
#include "tools/codegen_bootstrap/emit_util.h"

#include <string>
#include <string_view>
#include <vector>

namespace midday::codegen {

using base::Json;
using detail::entries;
using detail::str;
using detail::text;
using detail::truthy;

namespace {

// Doc text as one markdown line (newlines -> spaces); empty stays empty.
std::string flat(std::string_view doc) {
    std::string out(doc);
    for (char& c : out)
        if (c == '\n')
            c = ' ';
    return out;
}

std::string tick(std::string_view value) {
    return "`" + std::string(value) + "`";
}

// `- level: `x`` / `- base: `x`` / `- compat_hash: `x``, one paragraph.
std::string meta(const Json& entry, bool with_level) {
    std::string out;
    if (with_level)
        out += "- level: " + tick(str(entry, "level")) + "\n";
    if (const Json* base_class = entry.find("base"))
        out += "- base: " + tick(base_class->as_string()) + "\n";
    out += "- compat_hash: " + tick(str(entry, "compat_hash"));
    return out;
}

// `name(a: float, b: int = 5) -> ret` (docs signature spelling).
std::string signature(const Json& holder) {
    std::string out = str(holder, "name") + "(";
    bool first = true;
    for (const Json& param : entries(holder, "params")) {
        if (!first)
            out += ", ";
        first = false;
        out += str(param, "name") + ": " + str(param, "type");
        if (const Json* fallback = param.find("default"))
            out += " = " + fallback->dump();
    }
    out += ") -> " + str(holder, "returns");
    return out;
}

std::string row(const std::vector<std::string>& cells) {
    std::string out = "|";
    for (const std::string& cell : cells)
        out += " " + cell + " |";
    return out;
}

std::string table(const std::vector<std::string>& header, const std::vector<std::string>& rows) {
    std::string out = row(header) + "\n" + row(std::vector<std::string>(header.size(), "---"));
    for (const std::string& line : rows) {
        out += "\n";
        out += line;
    }
    return out;
}

void add_doc_paragraph(std::vector<std::string>& paragraphs,
                       const Json& entry,
                       std::string_view key) {
    const std::string_view doc = text(entry, key);
    if (!doc.empty())
        paragraphs.push_back(flat(doc));
}

void classes_section(std::vector<std::string>& out, const Json& document) {
    out.emplace_back("## Classes");
    const Json::Array& classes = entries(document, "classes");
    if (classes.empty()) {
        out.emplace_back("_None registered._");
        return;
    }
    for (const Json& entry : classes) {
        out.push_back("### " + tick(str(entry, "name")));
        add_doc_paragraph(out, entry, "doc");
        out.push_back(meta(entry, true));

        const Json::Array& properties = entries(entry, "properties");
        if (properties.empty()) {
            out.emplace_back("_No properties._");
        } else {
            std::vector<std::string> rows;
            for (const Json& property : properties) {
                const Json* fallback = property.find("default");
                std::string flags;
                if (const Json* list = property.find("flags"))
                    for (const Json& flag : list->elements())
                        flags += (flags.empty() ? "" : ", ") + flag.as_string();
                rows.push_back(row({tick(str(property, "name")),
                                    tick(str(property, "type")),
                                    fallback != nullptr ? tick(fallback->dump()) : "",
                                    flags,
                                    cell_escape(text(property, "doc"))}));
            }
            out.push_back(table({"property", "type", "default", "flags", "doc"}, rows));
        }

        const Json::Array& methods = entries(entry, "methods");
        if (methods.empty()) {
            out.emplace_back("_No methods._");
        } else {
            out.emplace_back("Methods:");
            std::string list;
            for (const Json& method : methods) {
                if (!list.empty())
                    list += "\n";
                list += "- " + tick(signature(method)) + " (compat_hash " +
                        tick(str(method, "compat_hash")) + ")";
                const std::string_view doc = text(method, "doc");
                if (!doc.empty())
                    list += " -- " + flat(doc);
            }
            out.push_back(std::move(list));
        }
    }
}

void events_section(std::vector<std::string>& out, const Json& document) {
    out.emplace_back("## Events");
    const Json::Array& events = entries(document, "events");
    if (events.empty()) {
        out.emplace_back("_None registered._");
        return;
    }
    for (const Json& entry : events) {
        out.push_back("### " + tick(str(entry, "name")));
        add_doc_paragraph(out, entry, "doc");
        out.push_back(meta(entry, true));
        const Json::Array& payload = entries(entry, "payload");
        if (payload.empty()) {
            out.emplace_back("_No payload._");
            continue;
        }
        std::vector<std::string> rows;
        for (const Json& field : payload)
            rows.push_back(row({tick(str(field, "name")),
                                tick(str(field, "type")),
                                cell_escape(text(field, "doc"))}));
        out.push_back(table({"field", "type", "doc"}, rows));
    }
}

void functions_section(std::vector<std::string>& out, const Json& document) {
    out.emplace_back("## Expression functions");
    const Json::Array& functions = entries(document, "functions");
    if (functions.empty()) {
        out.emplace_back("_None registered._");
        return;
    }
    for (const Json& entry : functions) {
        out.push_back("### " + tick(signature(entry)));
        add_doc_paragraph(out, entry, "doc");
        out.push_back(meta(entry, true));
    }
}

void verbs_section(std::vector<std::string>& out, const Json& document) {
    out.emplace_back("## CLI verbs");
    const Json::Array& verbs = entries(document, "verbs");
    if (verbs.empty()) {
        out.emplace_back("_None registered._");
        return;
    }
    for (const Json& entry : verbs) {
        out.push_back("### " + tick("midday " + str(entry, "name")));
        add_doc_paragraph(out, entry, "summary");
        out.push_back(meta(entry, false));

        const Json::Array& flags = entries(entry, "flags");
        if (flags.empty()) {
            out.emplace_back("_No flags._");
        } else {
            out.emplace_back("Flags:");
            std::vector<std::string> rows;
            for (const Json& flag : flags) {
                const Json* fallback = flag.find("default");
                rows.push_back(row({tick("--" + str(flag, "name")),
                                    tick(str(flag, "type")),
                                    truthy(flag, "required") ? "yes" : "no",
                                    fallback != nullptr ? tick(fallback->dump()) : "",
                                    cell_escape(text(flag, "doc"))}));
            }
            out.push_back(table({"flag", "type", "required", "default", "doc"}, rows));
        }

        const Json::Array& positionals = entries(entry, "positionals");
        if (positionals.empty()) {
            out.emplace_back("_No positionals._");
        } else {
            out.emplace_back("Positionals:");
            std::vector<std::string> rows;
            for (const Json& positional : positionals)
                rows.push_back(row({tick(str(positional, "name")),
                                    tick(str(positional, "type")),
                                    truthy(positional, "required") ? "yes" : "no",
                                    truthy(positional, "variadic") ? "yes" : "no",
                                    cell_escape(text(positional, "doc"))}));
            out.push_back(table({"positional", "type", "required", "variadic", "doc"}, rows));
        }
    }
}

} // namespace

std::string emit_docs(const Json& document) {
    std::vector<std::string> paragraphs;
    paragraphs.emplace_back("# Midday Engine API reference");
    // Generator-neutral provenance (byte-identical bootstrap/selfhost output).
    paragraphs.emplace_back(
        "GENERATED from engine_api.json. DO NOT EDIT.\n"
        "Signature compat hashes are XXH3-64 over signature-only JSON (docs excluded).");
    paragraphs.push_back("- engine_version: " + tick(str(document, "engine_version")) +
                         "\n- api_compat_hash: " + tick(str(document, "api_compat_hash")));
    classes_section(paragraphs, document);
    events_section(paragraphs, document);
    functions_section(paragraphs, document);
    verbs_section(paragraphs, document);

    std::string out;
    for (std::size_t i = 0; i < paragraphs.size(); ++i) {
        if (i != 0)
            out += "\n\n";
        out += paragraphs[i];
    }
    out += "\n";
    return out;
}

} // namespace midday::codegen

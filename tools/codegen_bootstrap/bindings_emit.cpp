// tools/codegen_bootstrap/bindings_emit.cpp — bindings_spec.json emitter:
// the glue spec m0-batch-bindings implements (per-function call signatures,
// event payload decode specs, batch envelope placeholder). Shape spec:
// api/CODEGEN.md "bindings_spec.json layout". No subsystem ever gets
// hand-written bindings — this document is the only bridge input.

#include "tools/codegen_bootstrap/codegen.h"
#include "tools/codegen_bootstrap/emit_util.h"

#include <cstdint>
#include <utility>

namespace midday::codegen {

using base::Json;

namespace {

// Deep copy minus every doc/summary key: signatures stay verbatim (level,
// defaults, flags, compat hashes IN), prose stays out of the glue contract.
Json strip_docs(const Json& value) {
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items())
            if (key != "doc" && key != "summary")
                out.set(key, strip_docs(item));
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value.elements())
            out.push(strip_docs(item));
        return out;
    }
    return value;
}

} // namespace

std::string emit_bindings(const Json& document) {
    Json spec = Json::object();
    spec.set("format_version", std::int64_t{1});
    spec.set("api_compat_hash", detail::str(document, "api_compat_hash"));
    spec.set("expr_functions", strip_docs(*document.find("functions")));
    spec.set("events", strip_docs(*document.find("events")));
    spec.set("classes", strip_docs(*document.find("classes")));

    Json envelope = Json::object();
    envelope.set("envelope_version", std::int64_t{0});
    envelope.set("status", "placeholder");
    envelope.set("doc",
                 "Reserved: m0-batch-bindings designs the real batch envelope (per-query SoA "
                 "views backed by typed arrays, one segment per component column, pooled math "
                 "slots, per-tick crossing/GC counters). views stays empty and envelope_version "
                 "stays 0 until then; refuse envelope_version 0 for actual batching.");
    envelope.set("views", Json::array());
    spec.set("batch_envelope", std::move(envelope));
    return spec.dump() + "\n";
}

} // namespace midday::codegen

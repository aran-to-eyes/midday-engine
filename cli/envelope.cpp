#include "cli/envelope.h"

namespace midday::cli {

Json make_envelope(std::string_view verb, Exit exit, const Json& payload, const Error* error) {
    const bool ok = exit == Exit::Ok;

    Json env = Json::object();
    env.set("ok", ok);
    env.set("verb", verb.empty() ? std::string_view("-") : verb);
    env.set("exit_code", static_cast<int>(exit));

    if (!ok) {
        Json err = Json::object();
        if (error != nullptr) {
            err.set("code", error->code);
            err.set("message", error->message);
            if (error->details.is_object() && !error->details.items().empty()) {
                err.set("details", error->details);
            }
        } else {
            // A failing verb that provided no Error still yields a
            // schema-valid envelope; the generic code is greppable.
            err.set("code", "internal.unspecified");
            err.set("message", "verb failed without a structured error");
        }
        env.set("error", std::move(err));
    }

    if (payload.is_object()) {
        for (const auto& [key, value] : payload.items()) {
            if (env.find(key) == nullptr) { // envelope fields win over payload
                env.set(key, value);
            }
        }
    }
    return env;
}

} // namespace midday::cli

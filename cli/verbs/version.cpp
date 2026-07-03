#include "cli/verb.h"
#include "core/base/platform.h"

#ifndef MIDDAY_VERSION
#define MIDDAY_VERSION "0.0.0-unversioned"
#endif

namespace midday::cli {

VerbOutcome verb_version(const VerbArgs&) {
    VerbOutcome out;
    out.payload.set("name", "midday");
    out.payload.set("version", MIDDAY_VERSION);
    out.payload.set("platform", base::platform_os());
    out.payload.set("arch", base::platform_arch());
    out.human = std::string("midday ") + MIDDAY_VERSION + " (" + std::string(base::platform_os()) +
                "-" + std::string(base::platform_arch()) + ")";
    return out;
}

} // namespace midday::cli

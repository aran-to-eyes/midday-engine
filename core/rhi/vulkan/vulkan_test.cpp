// core/rhi/vulkan/vulkan_test.cpp — Vulkan-ONLY truths (rhi.vk.*). The
// backend-independent semantics (handles, protocol, clear/triangle/quad
// pixel truths) live in the conformance corpus (core/rhi/conformance_test.
// cpp), which drives this same shared device; what remains here is the
// pinned-driver-class hash gate — hash-equality against testkit/goldens/m0
// is only meaningful when the active driver matches the committed
// DRIVER_PIN.txt (two-tier semantics, D-BUILD-090/091).
//
// PLATFORM REALITY: many dev hosts have no Vulkan ICD. Tests SKIP loudly
// through the shared registry (testkit/rhi_backends.h) — skip is never a
// silent pass, and the golden-software lane (lavapipe guaranteed) is the
// enforcement point. Note: this file contains no Vulkan includes — the
// backend is exercised entirely through the seam (the boundary, demonstrated).

#include "core/base/hex.h"
#include "core/rhi/goldens.h"
#include "core/rhi/scenes.h"
#include "testkit/doctest.h"
#include "testkit/rhi_backends.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {

using namespace midday;
using namespace midday::rhi;

// Golden dir: MIDDAY_RHI_GOLDEN_DIR (CI lane / verify set it) or the repo
// default when running from the repo root. Absent dir = no hash gate here.
std::string golden_dir() {
    // push/disable/pop, not warning(suppress): an #endif eats a suppress
    // (journal writer_test precedent, D-BUILD ledger 2026-07-03).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* env = std::getenv("MIDDAY_RHI_GOLDEN_DIR");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (env != nullptr && *env != '\0')
        return env;
    const std::string fallback = "testkit/goldens/m0";
    return std::filesystem::exists(fallback) ? fallback : std::string();
}

TEST_CASE("rhi.vk.goldens_match_within_pinned_driver_class") {
    testkit::RhiBackend& backend = testkit::rhi_backend("vulkan");
    RhiDevice* device =
        testkit::acquire(backend, "rhi.vk.goldens_match_within_pinned_driver_class");
    if (device == nullptr)
        return;
    const std::string dir = golden_dir();
    if (dir.empty()) {
        MESSAGE("no golden dir (env MIDDAY_RHI_GOLDEN_DIR unset, no testkit/goldens/m0 in cwd)");
        return;
    }
    const std::optional<std::string> pin = read_driver_pin(dir);
    if (!pin.has_value()) {
        MESSAGE("goldens not minted yet (no DRIVER_PIN.txt in " << dir << ") — bootstrap pending");
        return;
    }
    if (*pin != device->caps().driver_info) {
        MESSAGE("driver '" << device->caps().driver_info << "' is not the pinned class '" << *pin
                           << "' — hash gate skipped (structural tests still ran)");
        return;
    }
    for (SceneId scene : kAllScenes) {
        INFO("scene: " << std::string(to_string(scene)));
        const std::string want =
            read_golden_hash(dir, scene).value_or("(golden hash file missing)");
        REQUIRE_MESSAGE(want != "(golden hash file missing)",
                        "golden hash file missing for pinned driver");
        SceneRender render = render_scene(*device, scene);
        REQUIRE(render.ok());
        CHECK(base::hex64(pixel_hash(render.image)) == want);
    }
}

} // namespace

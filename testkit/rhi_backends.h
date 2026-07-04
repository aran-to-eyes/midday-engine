// testkit/rhi_backends.h — the RHI conformance corpus's backend registry:
// ONE process-wide device per backend, shared by every rhi test TU (the
// Vulkan backend allows a single live device per process in M0, so the
// corpus and the golden tests must draw from the same instance).
//
// Availability is a per-host fact, never a silent pass: acquire() logs a
// LOUD skip (stderr + doctest MESSAGE, counted) when a backend cannot come
// up, and rhi.conformance.zz_backend_report accounts for every skip. The
// lanes make skipping impossible where it matters: golden-software
// guarantees Vulkan (lavapipe), build-macos guarantees Metal (probe-gated).

#pragma once

#include "core/rhi/device.h"
#include "core/rhi/metal/metal_device.h"
#include "core/rhi/null_device.h"
#include "core/rhi/vulkan/vulkan_device.h"
#include "testkit/doctest.h"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace midday::testkit {

struct RhiBackend {
    std::string name;           // caps().backend must agree
    bool rasterizes = false;    // NullDevice invents no pixels (D-BUILD-092)
    bool compiles_glsl = false; // NullDevice stores source, never compiles
    rhi::DeviceResult result{};
    int skips = 0;
};

// Factories must return errors, never throw (seam contract) — but this
// registry initializes ONCE for the whole process inside the first test
// case, and C++ retries a static initializer that exits via exception, so a
// single misbehaving factory (vendored driver code raising through the C
// ABI) would re-kill EVERY registry-touching case. Contain it: an escaped
// exception becomes an unavailable-with-reason result, i.e. a LOUD skip the
// zz report accounts for (and still a hard failure for Metal on macOS).
inline rhi::DeviceResult contained(const char* backend, rhi::DeviceResult (*make)()) {
    try {
        return make();
    } catch (const std::exception& error) {
        return {.error =
                    base::Error{.code = "rhi.device_fault",
                                .message = std::string(backend) +
                                           " factory threw instead of returning an error (seam "
                                           "contract violation): " +
                                           error.what()}};
    } catch (...) { // NSException through the C ABI lands here on Apple
        return {.error = base::Error{.code = "rhi.device_fault",
                                     .message =
                                         std::string(backend) +
                                         " factory threw a non-std exception instead of returning "
                                         "an error (seam contract violation)"}};
    }
}

inline std::vector<RhiBackend>& rhi_backends() {
    static std::vector<RhiBackend> backends = [] {
        std::vector<RhiBackend> list;
        list.push_back({.name = "null",
                        .rasterizes = false,
                        .compiles_glsl = false,
                        .result = {.device = std::make_unique<rhi::NullDevice>()}});
        list.push_back(
            {.name = "vulkan",
             .rasterizes = true,
             .compiles_glsl = true,
             .result = contained("vulkan", [] { return rhi::create_vulkan_device({}); })});
        list.push_back({.name = "metal",
                        .rasterizes = true,
                        .compiles_glsl = true,
                        .result = contained("metal", [] { return rhi::create_metal_device({}); })});
        return list;
    }();
    return backends;
}

inline RhiBackend& rhi_backend(std::string_view name) {
    for (RhiBackend& backend : rhi_backends())
        if (backend.name == name)
            return backend;
    REQUIRE_MESSAGE(false, "unknown rhi backend requested: " << std::string(name));
    return rhi_backends().front(); // unreachable: REQUIRE throws
}

// The backend's shared device, or nullptr after a LOUD skip.
inline rhi::RhiDevice* acquire(RhiBackend& backend, const char* test_name) {
    if (backend.result.ok())
        return backend.result.device.get();
    ++backend.skips;
    const std::string reason =
        backend.result.error ? backend.result.error->message : std::string("unknown");
    std::fprintf(
        stderr, "rhi SKIP [%s] %s — %s\n", backend.name.c_str(), test_name, reason.c_str());
    MESSAGE("SKIPPED (" << backend.name << " unavailable): " << reason);
    return nullptr;
}

} // namespace midday::testkit

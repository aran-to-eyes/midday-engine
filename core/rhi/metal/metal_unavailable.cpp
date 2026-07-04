// core/rhi/metal/metal_unavailable.cpp — the non-macOS body of the Metal
// factory (CMake selects this TU instead of the .mm backend off Apple).
// The seam surface exists on every platform; unavailability is a structured
// answer, never a missing symbol (the volk/ICD-less precedent).

#include "core/rhi/metal/metal_device.h"

namespace midday::rhi {

DeviceResult create_metal_device(const MetalDeviceOptions& /*options*/) {
    return {.error = base::Error{.code = "rhi.unavailable",
                                 .message = "Metal backend requires macOS (built without Metal)"}};
}

} // namespace midday::rhi

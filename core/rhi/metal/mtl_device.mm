// core/rhi/metal/mtl_device.mm — bring-up, capability report, teardown,
// transient blit plumbing (m0-rhi-metal). Resources live in
// mtl_resources.mm, textures in mtl_textures.mm, recording/submission in
// mtl_commands.mm — the vk_device.cpp file shape.

#include "core/rhi/metal/mtl_internal.h"

#include <memory>
#include <string>
#include <utility>

namespace midday::rhi::mtl {

namespace {

base::Error unavailable(std::string message) {
    return base::Error{.code = "rhi.unavailable", .message = std::move(message)};
}

} // namespace

base::Error mtl_fault(std::string_view what, NSError* error) {
    base::Error fault{.code = "rhi.device_fault", .message = std::string(what) + " failed"};
    if (error != nil) {
        const std::string description = error.localizedDescription.UTF8String;
        fault.message += " (" + description + ")";
        fault.details.set("metal_error", description);
        fault.details.set("metal_code", static_cast<std::int64_t>(error.code));
    }
    return fault;
}

base::Error nsexception_fault(const char* what, NSException* exception) {
    base::Error fault{.code = "rhi.device_fault",
                      .message = std::string(what) + " raised an Objective-C exception"};
    const char* name =
        (exception != nil && exception.name != nil) ? exception.name.UTF8String : "(unknown)";
    const char* reason = (exception != nil && exception.reason != nil) ? exception.reason.UTF8String
                                                                       : "(no reason given)";
    fault.message += std::string(" (") + name + ": " + reason + ")";
    fault.details.set("nsexception_name", name);
    fault.details.set("nsexception_reason", reason);
    return fault;
}

std::optional<base::Error> MetalDevice::initialize(const MetalDeviceOptions& /*options*/) {
    return guarded_call("initialize", [&]() -> std::optional<base::Error> {
        // Deterministic selection on multi-GPU hosts: lowest registryID wins
        // (stable across enumeration order, the type_rank/first-index ethos).
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        if (devices.count == 0)
            return unavailable("no Metal devices on this host");
        id<MTLDevice> best = devices[0];
        for (id<MTLDevice> candidate in devices)
            if (candidate.registryID < best.registryID)
                best = candidate;
        device_ = best;

        queue_ = [device_ newCommandQueue];
        if (queue_ == nil)
            return mtl_fault("MTLDevice newCommandQueue", nil);

        caps_.backend = "metal";
        caps_.device_name = device_.name.UTF8String;
        caps_.driver_info = "Apple Metal";
        if (@available(macOS 14.0, *)) {
            // The architecture name (e.g. "applegpu_g16p") is the closest
            // Metal analogue to a driver build string — recorded for
            // reports; goldens never hash-gate on Metal (D-BUILD-090).
            // Virtual devices may report no architecture: skip, never crash.
            if (device_.architecture != nil && device_.architecture.name != nil)
                caps_.driver_info += std::string(" ") + device_.architecture.name.UTF8String;
        }
        caps_.api_version = "MSL 2.2"; // what the seam compiles against (shadercomp)
        // Honest capability floor by family: every Apple-silicon and
        // Mac2-class device supports 16384; anything older still clears 8192.
        const bool large_textures = [device_ supportsFamily:MTLGPUFamilyApple3] ||
                                    [device_ supportsFamily:MTLGPUFamilyMac2];
        caps_.max_texture_size = large_textures ? 16384 : 8192;
        caps_.software_rasterizer = false;
        caps_.validation_enabled = false; // Metal API validation is env-driven, not a layer
        return std::nullopt;
    });
}

MetalDevice::~MetalDevice() {
    // A list destroyed mid-recording still owns a live encoder; Metal
    // asserts on deallocating an un-ended encoder, so close them first.
    // Teardown never throws: an NSException here would escape a destructor
    // and terminate, so it is swallowed (nothing left to report to).
    lists_.for_each_live([](CommandListEntry& entry) {
        @try {
            if (entry.encoder != nil)
                [entry.encoder endEncoding];
        } @catch (NSException* exception) {
            (void)exception;
        }
    });
    // ARC releases every retained Metal object with the pools (deterministic
    // ascending slot order per pool, the device.h teardown contract).
}

const DeviceCaps& MetalDevice::caps() const {
    return caps_;
}

std::optional<base::Error>
MetalDevice::with_transient_blit(const std::function<void(id<MTLBlitCommandEncoder>)>& fn) {
    return guarded_call("transient blit", [&]() -> std::optional<base::Error> {
        id<MTLCommandBuffer> commands = [queue_ commandBuffer];
        if (commands == nil)
            return mtl_fault("MTLCommandQueue commandBuffer (transient)", nil);
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (blit == nil)
            return mtl_fault("MTLCommandBuffer blitCommandEncoder", nil);
        fn(blit);
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted)
            return mtl_fault("transient blit submit", commands.error);
        return std::nullopt;
    });
}

} // namespace midday::rhi::mtl

namespace midday::rhi {

DeviceResult create_metal_device(const MetalDeviceOptions& options) {
    auto device = std::make_unique<mtl::MetalDevice>();
    if (auto error = device->initialize(options))
        return {.error = std::move(error)};
    return {.device = std::move(device)};
}

} // namespace midday::rhi

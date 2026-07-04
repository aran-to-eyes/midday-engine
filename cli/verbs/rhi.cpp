// `midday rhi probe|render` — the GPU seam's CLI face (m0-rhi-vulkan;
// --backend metal at m0-rhi-metal).
//
// probe: device availability + capability report, on ANY host. Probing is a
//   question, so an ICD-less machine is a successful answer (exit 0,
//   available=false, reason) — CI lanes gate with jq '.available'.
//
// render: the three pinned M0 scenes, synchronously, headless; per scene the
//   DECODED-pixel hash (Aurora D-14) and optionally a PNG (--out-dir, plus
//   driver.txt with the exact driver string — the golden-candidate bundle
//   the golden-software lane uploads). With --goldens <dir> it compares
//   against committed hashes and FAILS when they are absent (bootstrap red,
//   never a stubbed green: exit 1 rhi.golden_missing) or mismatched
//   (rhi.golden_mismatch). --software requires a lavapipe-class device so
//   goldens can never be minted on a surprise GPU.
//
// --backend vulkan|metal selects the seam implementation; the build-macos
// lane renders the same scenes through BOTH and feeds `midday shot compare`
// (tier 2) — the cross-backend proof (MILESTONE_0 item 23). --validation
// and --software are Vulkan concepts and refuse under --backend metal.

#include "cli/verb.h"
#include "core/base/hex.h"
#include "core/rhi/device.h"
#include "core/rhi/goldens.h"
#include "core/rhi/metal/metal_device.h"
#include "core/rhi/scenes.h"
#include "core/rhi/vulkan/vulkan_device.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace midday::cli {
namespace {

VerbOutcome fail(Error error, Json payload = Json::object()) {
    VerbOutcome out;
    out.exit = Exit::Failure;
    out.error = std::move(error);
    out.payload = std::move(payload);
    return out;
}

Json caps_json(const rhi::DeviceCaps& caps) {
    Json out = Json::object();
    out.set("backend", caps.backend);
    out.set("device_name", caps.device_name);
    out.set("driver", caps.driver_info);
    out.set("api_version", caps.api_version);
    out.set("max_texture_size", static_cast<std::int64_t>(caps.max_texture_size));
    out.set("software", caps.software_rasterizer);
    out.set("validation", caps.validation_enabled);
    return out;
}

// Backend selection. Vulkan-only knobs refuse under metal EXPLICITLY (a
// silently ignored --software could mint goldens on the wrong device class).
struct BackendChoice {
    rhi::DeviceResult result{};
    std::optional<VerbOutcome> usage; // set = refuse before any device work
};

BackendChoice open_device(const VerbArgs& args) {
    const std::string& backend = args.get_string("backend");
    if (backend == "vulkan")
        return {.result =
                    rhi::create_vulkan_device({.enable_validation = args.get_bool("validation"),
                                               .require_software = args.get_bool("software")}),
                .usage = std::nullopt};
    if (backend == "metal") {
        if (args.get_bool("validation") || args.get_bool("software")) {
            VerbOutcome out;
            out.exit = Exit::Usage;
            out.error = Error{.code = "usage.invalid_value",
                              .message = "--validation and --software are Vulkan options "
                                         "(not meaningful with --backend metal)"};
            return {.usage = std::move(out)};
        }
        return {.result = rhi::create_metal_device({}), .usage = std::nullopt};
    }
    VerbOutcome out;
    out.exit = Exit::Usage;
    out.error = Error{.code = "usage.invalid_value",
                      .message = "unknown backend '" + backend + "' (available: vulkan, metal)"};
    return {.usage = std::move(out)};
}

VerbOutcome run_probe(const VerbArgs& args) {
    BackendChoice choice = open_device(args);
    if (choice.usage.has_value())
        return std::move(*choice.usage);
    const std::string& backend = args.get_string("backend");
    rhi::DeviceResult& device = choice.result;
    VerbOutcome out;
    out.payload.set("available", device.ok());
    if (device.ok()) {
        out.payload.set("caps", caps_json(device.device->caps()));
        out.human = backend + ": " + device.device->caps().device_name + " (" +
                    device.device->caps().driver_info + ")";
    } else {
        // Unavailability is the probe's ANSWER, not its failure (exit 0).
        out.payload.set("reason", device.error ? device.error->message : "unknown");
        out.payload.set("reason_code", device.error ? device.error->code : "rhi.unavailable");
        out.human = backend + ": unavailable — " +
                    (device.error ? device.error->message : std::string("unknown"));
    }
    return out;
}

VerbOutcome run_render(const VerbArgs& args) {
    const std::string out_dir = args.present("out-dir") ? args.get_string("out-dir") : "";
    const std::string goldens = args.present("goldens") ? args.get_string("goldens") : "";

    BackendChoice choice = open_device(args);
    if (choice.usage.has_value())
        return std::move(*choice.usage);
    rhi::DeviceResult device = std::move(choice.result);
    if (!device.ok())
        return fail(std::move(device.error)
                        .value_or(base::Error{.code = "rhi.unavailable",
                                              .message = "backend returned no device"}));
    const rhi::DeviceCaps& caps = device.device->caps();

    if (!out_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        // The candidate bundle records what it was rendered on: driver.txt
        // IS the DRIVER_PIN.txt the orchestrator commits with the hashes.
        if (auto error = base::write_file(std::filesystem::path(out_dir) / "driver.txt",
                                          caps.driver_info + "\n",
                                          "rhi.image_write"))
            return fail(std::move(*error));
    }

    // Scene selection: one via --scene, or all three.
    std::vector<rhi::SceneId> scenes;
    if (args.present("scene")) {
        const std::optional<rhi::SceneId> parsed = rhi::scene_from_name(args.get_string("scene"));
        if (!parsed.has_value()) {
            VerbOutcome out;
            out.exit = Exit::Usage;
            out.error = Error{.code = "usage.invalid_value",
                              .message = "unknown scene '" + args.get_string("scene") +
                                         "' (available: clear, triangle, textured_quad)"};
            return out;
        }
        scenes.push_back(*parsed);
    } else {
        scenes.assign(rhi::kAllScenes.begin(), rhi::kAllScenes.end());
    }

    // Golden preconditions: comparing hashes minted on another driver class
    // is meaningless — refuse loudly rather than fail confusingly.
    std::optional<std::string> pin;
    if (!goldens.empty()) {
        pin = rhi::read_driver_pin(goldens);
        if (pin.has_value() && *pin != caps.driver_info) {
            Error error{.code = "rhi.golden_driver_mismatch",
                        .message = "goldens are pinned to a different driver class"};
            error.details.set("pinned", *pin);
            error.details.set("active", caps.driver_info);
            return fail(std::move(error));
        }
    }

    Json scene_list = Json::array();
    std::vector<std::string> missing;
    std::vector<std::string> mismatched;
    for (rhi::SceneId scene : scenes) {
        rhi::SceneRender render = rhi::render_scene(*device.device, scene);
        if (render.error.has_value())
            return fail(std::move(*render.error));
        const std::string name(rhi::to_string(scene));
        const std::string hash = base::hex64(rhi::pixel_hash(render.image));

        Json entry = Json::object();
        entry.set("scene", name);
        entry.set("pixel_hash", hash);
        if (!out_dir.empty()) {
            const std::string png = (std::filesystem::path(out_dir) / (name + ".png")).string();
            if (auto error = rhi::write_png(render.image, png))
                return fail(std::move(*error));
            if (auto error = base::write_file(std::filesystem::path(out_dir) / (name + ".hash"),
                                              hash + "\n",
                                              "rhi.image_write"))
                return fail(std::move(*error));
            entry.set("png", png);
        }
        if (!goldens.empty()) {
            const std::optional<std::string> want = rhi::read_golden_hash(goldens, scene);
            if (!want.has_value()) {
                missing.push_back(name);
                entry.set("golden", nullptr);
            } else {
                entry.set("golden", *want);
                entry.set("match", *want == hash);
                if (*want != hash)
                    mismatched.push_back(name);
            }
        }
        scene_list.push(std::move(entry));
    }

    VerbOutcome out;
    out.payload.set("caps", caps_json(caps));
    out.payload.set("scenes", std::move(scene_list));
    if (!out_dir.empty())
        out.payload.set("out_dir", out_dir);

    if (!goldens.empty() && (!pin.has_value() || !missing.empty())) {
        Error error{.code = "rhi.golden_missing",
                    .message = "goldens not minted for this driver class yet — inspect the "
                               "rendered candidates, commit hashes + DRIVER_PIN.txt, re-run"};
        if (!pin.has_value())
            error.details.set("missing_pin", true);
        Json names = Json::array();
        for (const std::string& name : missing)
            names.push(name);
        error.details.set("missing", std::move(names));
        return fail(std::move(error), std::move(out.payload));
    }
    if (!mismatched.empty()) {
        Error error{.code = "rhi.golden_mismatch",
                    .message = "decoded-pixel hashes diverge from committed goldens"};
        Json names = Json::array();
        for (const std::string& name : mismatched)
            names.push(name);
        error.details.set("mismatched", std::move(names));
        return fail(std::move(error), std::move(out.payload));
    }
    out.human = "rendered " + std::to_string(scenes.size()) + " scene(s) on " + caps.device_name +
                (goldens.empty() ? "" : " — goldens MATCH");
    return out;
}

VerbOutcome rhi_verb(const VerbArgs& args) {
    const std::string& op = args.get_string("op");
    if (op == "probe")
        return run_probe(args);
    if (op == "render")
        return run_render(args);
    VerbOutcome out;
    out.exit = Exit::Usage;
    out.error = Error{.code = "usage.unknown_op",
                      .message = "unknown rhi operation '" + op + "' (available: probe, render)"};
    return out;
}

constexpr FlagSpec kFlags[] = {
    {.name = "backend",
     .type = "name",
     .doc = "seam implementation: vulkan | metal (metal is macOS-only)",
     .default_text = "vulkan"},
    {.name = "validation",
     .type = "bool",
     .doc = "enable the Vulkan validation layer (refuses if not installed)"},
    {.name = "software",
     .type = "bool",
     .doc = "require a software rasterizer (lavapipe class; golden lane sets this)"},
    {.name = "scene",
     .type = "name",
     .doc = "render one scene: clear | triangle | textured_quad (default: all)"},
    {.name = "out-dir",
     .type = "string",
     .doc = "write <scene>.png + <scene>.hash + driver.txt here (render)"},
    {.name = "goldens",
     .type = "string",
     .doc = "compare decoded-pixel hashes against this golden dir (render)"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "op", .type = "string", .doc = "operation: probe | render"},
};

} // namespace

const VerbSpec& rhi_spec() {
    static constexpr VerbSpec kSpec{
        .name = "rhi",
        .summary = "GPU seam tools (probe: device availability/caps; render: M0 scenes to PNG "
                   "+ decoded-pixel hashes)",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &rhi_verb,
    };
    return kSpec;
}

} // namespace midday::cli

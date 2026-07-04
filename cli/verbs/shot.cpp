// `midday shot compare` — the golden-frame verification pillar's comparison
// face (m0-golden-compare). Two decoded images, two verdict tiers:
//
//   tier 1  hash_equal      decoded-pixel XXH3 equality (Aurora D-14) —
//                           the gate WITHIN a pinned driver class
//   tier 2  tolerance.pass  explicit-threshold metrics (core/rhi/compare.h) —
//                           the gate ACROSS drivers/backends
//
// pass = hash_equal || tolerance.pass, and THE CALLER CHOOSES WHICH TIER
// GATES: pinned-driver lanes jq on .hash_equal, cross-backend lanes on
// .pass. Hash-fail + tolerance-pass (the driver-noise case) is therefore
// exit 0 with hash_equal:false clearly reported — never a process failure.
//
// Exit codes: 0 pass (either tier) · 1 tolerance fail (structural mismatch,
// error shot.mismatch) · 2 usage · 3 io/validation (unreadable/undecodable
// input, unwritable --diff). m3-shot adds capture ops next to compare.

#include "cli/verb.h"
#include "core/base/hex.h"
#include "core/rhi/compare.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace midday::cli {
namespace {

VerbOutcome refuse(Exit exit, Error error) {
    VerbOutcome out;
    out.exit = exit;
    out.error = std::move(error);
    return out;
}

std::optional<VerbOutcome> validate_thresholds(const rhi::ToleranceThresholds& thresholds) {
    const auto usage = [](std::string message) {
        return refuse(Exit::Usage,
                      Error{.code = "usage.invalid_value", .message = std::move(message)});
    };
    if (thresholds.channel_tolerance < 0 || thresholds.channel_tolerance > 255)
        return usage("--tolerance must be a channel delta in [0, 255]");
    if (thresholds.max_pct_over < 0.0 || thresholds.max_pct_over > 100.0)
        return usage("--max-pct-over must be a percentage in [0, 100]");
    if (thresholds.max_mean_delta < 0.0)
        return usage("--max-mean must be >= 0");
    return std::nullopt;
}

Json tolerance_json(const rhi::ToleranceVerdict& tol, const rhi::ToleranceThresholds& thresholds) {
    Json thresholds_json = Json::object();
    thresholds_json.set("channel_tolerance",
                        static_cast<std::int64_t>(thresholds.channel_tolerance));
    thresholds_json.set("max_pct_over", thresholds.max_pct_over);
    thresholds_json.set("max_mean_delta", thresholds.max_mean_delta);

    Json out = Json::object();
    out.set("dims_match", tol.dims_match);
    out.set("width", static_cast<std::int64_t>(tol.width));
    out.set("height", static_cast<std::int64_t>(tol.height));
    out.set("max_channel_delta", static_cast<std::int64_t>(tol.max_channel_delta));
    out.set("mean_channel_delta", tol.mean_channel_delta);
    out.set("pixels_over", static_cast<std::int64_t>(tol.pixels_over));
    out.set("pct_pixels_over", tol.pct_pixels_over);
    out.set("thresholds", std::move(thresholds_json));
    out.set("pass", tol.pass);
    return out;
}

VerbOutcome run_compare(const VerbArgs& args) {
    const rhi::ToleranceThresholds thresholds{.channel_tolerance =
                                                  static_cast<int>(args.get_int("tolerance")),
                                              .max_pct_over = args.get_float("max-pct-over"),
                                              .max_mean_delta = args.get_float("max-mean")};
    if (std::optional<VerbOutcome> bad = validate_thresholds(thresholds))
        return *std::move(bad);

    const std::string& path_a = args.get_string("a");
    const std::string& path_b = args.get_string("b");
    rhi::ImageReadResult a = rhi::read_png(path_a);
    if (a.error.has_value())
        return refuse(Exit::Validation, *std::move(a.error));
    rhi::ImageReadResult b = rhi::read_png(path_b);
    if (b.error.has_value())
        return refuse(Exit::Validation, *std::move(b.error));

    const rhi::CompareVerdict verdict = rhi::compare_images(a.image, b.image, thresholds);

    VerbOutcome out;
    out.payload.set("a", path_a);
    out.payload.set("b", path_b);
    out.payload.set("pixel_hash_a", base::hex64(verdict.hash_a));
    out.payload.set("pixel_hash_b", base::hex64(verdict.hash_b));
    out.payload.set("hash_equal", verdict.hash_equal);
    out.payload.set("tolerance", tolerance_json(verdict.tolerance, thresholds));
    out.payload.set("pass", verdict.pass);

    // The diff artifact is emitted whenever requested — pass or fail — so a
    // lane can always archive it; an identical pair's diff is all black.
    if (args.present("diff")) {
        const std::string& diff_path = args.get_string("diff");
        if (auto error = rhi::write_png(rhi::diff_image(a.image, b.image), diff_path)) {
            VerbOutcome failed = refuse(Exit::Validation, *std::move(error));
            failed.payload = std::move(out.payload);
            return failed;
        }
        out.payload.set("diff", diff_path);
    }

    const rhi::ToleranceVerdict& tol = verdict.tolerance;
    const std::string tier2 = "max " + std::to_string(tol.max_channel_delta) + ", " +
                              std::to_string(tol.pixels_over) + " px over " +
                              std::to_string(thresholds.channel_tolerance);
    if (!verdict.pass) {
        Error error{.code = "shot.mismatch",
                    .message = tol.dims_match ? "images differ beyond tolerance (" + tier2 + ")"
                                              : "image dimensions differ"};
        VerbOutcome failed = refuse(Exit::Failure, std::move(error));
        failed.payload = std::move(out.payload);
        return failed;
    }
    out.human = verdict.hash_equal ? "hash MATCH (tier 1): identical decoded pixels"
                                   : "hash differ, tolerance PASS (tier 2: " + tier2 + ")";
    return out;
}

VerbOutcome shot_verb(const VerbArgs& args) {
    const std::string& op = args.get_string("op");
    if (op == "compare")
        return run_compare(args);
    return refuse(Exit::Usage,
                  Error{.code = "usage.unknown_op",
                        .message = "unknown shot operation '" + op + "' (available: compare)"});
}

constexpr FlagSpec kFlags[] = {
    {.name = "tolerance",
     .type = "int",
     .doc = "per-channel delta a pixel may carry without counting as over (0-255)",
     .default_text = "2"},
    {.name = "max-pct-over",
     .type = "float",
     .doc = "percent of pixels allowed over --tolerance before tier 2 fails",
     .default_text = "0"},
    {.name = "max-mean",
     .type = "float",
     .doc = "mean absolute channel delta budget (perceptual drift bound)",
     .default_text = "1"},
    {.name = "diff",
     .type = "string",
     .doc = "write an amplified per-pixel delta image (x8, saturated) to this PNG path"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "op", .type = "string", .doc = "operation: compare"},
    {.name = "a", .type = "string", .doc = "first PNG (golden/reference)"},
    {.name = "b", .type = "string", .doc = "second PNG (candidate)"},
};

} // namespace

const VerbSpec& shot_spec() {
    static constexpr VerbSpec kSpec{
        .name = "shot",
        .summary = "screenshot tools (compare: two-tier golden comparison — decoded-pixel "
                   "hash + explicit-threshold tolerance, optional diff image)",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &shot_verb,
    };
    return kSpec;
}

} // namespace midday::cli

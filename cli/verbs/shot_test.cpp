// `midday shot compare` exit tests (compare.verb.*) — the MILESTONE_0
// item-24 contract driven through the real verb: identical -> both tiers
// pass (exit 0); 1-LSB noise -> hash FAIL + tolerance PASS (exit 0 — the
// caller chooses which tier gates); structural -> both FAIL, exit 1, diff
// image emitted; io/validation -> exit 3; usage -> exit 2.

#include "cli/verb.h"
#include "cli/verbs/test_support.h"
#include "core/base/file_io.h"
#include "core/rhi/compare.h"
#include "testkit/compare_fixtures.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::cli;
using midday::cli::testsupport::field;
using midday::testkit::unwrap;

namespace {

VerbOutcome invoke(const std::vector<std::string>& tokens) {
    return testsupport::invoke(shot_spec(), tokens);
}

struct Fixtures {
    testkit::TempDir dir{"shot-compare"};
    std::string base, identical, noise, shifted;

    Fixtures() {
        REQUIRE_FALSE(testkit::write_compare_fixtures(dir.file("goldens")).has_value());
        const auto at = [this](const char* name) {
            return (std::filesystem::path(dir.file("goldens")) / name).string();
        };
        base = at("base.png");
        identical = at("identical.png");
        noise = at("noise.png");
        shifted = at("shifted.png");
    }
};

} // namespace

TEST_CASE("compare.verb.identical_pair_exits_0_with_both_tiers_passing") {
    const Fixtures fx;
    const VerbOutcome out = invoke({"compare", fx.base, fx.identical});
    REQUIRE_FALSE(out.error.has_value());
    CHECK(out.exit == Exit::Ok);
    CHECK(field(out.payload, "hash_equal").dump() == "true");
    CHECK(field(out.payload, "pass").dump() == "true");
    const Json& tolerance = field(out.payload, "tolerance");
    CHECK(field(tolerance, "pass").dump() == "true");
    CHECK(field(tolerance, "dims_match").dump() == "true");
    // The identical fixtures are different FILES with identical pixels:
    // equal tier-1 hashes over decoded pixels (Aurora D-14 at the CLI).
    CHECK(field(out.payload, "pixel_hash_a").dump() == field(out.payload, "pixel_hash_b").dump());
}

TEST_CASE("compare.verb.noise_pair_is_a_tier2_pass_with_hash_fail_reported") {
    const Fixtures fx;
    const VerbOutcome out = invoke({"compare", fx.base, fx.noise});
    // THE noise case: tier 1 fails, tier 2 passes -> exit 0 with
    // hash_equal:false clearly reported. The caller picks the gating tier
    // (pinned-driver lanes gate on hash_equal, cross-backend lanes on pass).
    REQUIRE_FALSE(out.error.has_value());
    CHECK(out.exit == Exit::Ok);
    CHECK(field(out.payload, "hash_equal").dump() == "false");
    CHECK(field(out.payload, "pass").dump() == "true");
    const Json& tolerance = field(out.payload, "tolerance");
    CHECK(field(tolerance, "pass").dump() == "true");
    CHECK(field(tolerance, "max_channel_delta").dump() == "1");
    CHECK(field(tolerance, "pixels_over").dump() == "0");
}

TEST_CASE("compare.verb.structural_pair_exits_1_and_emits_the_diff_image") {
    const Fixtures fx;
    const std::string diff_path = fx.dir.file("diff.png");
    const VerbOutcome out = invoke({"compare", fx.base, fx.shifted, "--diff", diff_path});
    CHECK(out.exit == Exit::Failure);
    CHECK(unwrap(out.error).code == "shot.mismatch");
    CHECK(field(out.payload, "hash_equal").dump() == "false");
    CHECK(field(out.payload, "pass").dump() == "false");
    const Json& tolerance = field(out.payload, "tolerance");
    CHECK(field(tolerance, "pass").dump() == "false");
    CHECK(field(out.payload, "diff").dump() == Json(diff_path).dump());

    // The emitted diff is a decodable PNG on the union canvas; the moved
    // square must light up (some saturated channel) and untouched
    // background must stay black.
    const rhi::ImageReadResult diff = rhi::read_png(diff_path);
    REQUIRE(diff.ok());
    CHECK(diff.image.width == testkit::kCompareFixtureSize);
    CHECK(diff.image.height == testkit::kCompareFixtureSize);
    CHECK(diff.image.at(4, 4)[0] == 255); // square lost here
    CHECK(diff.image.at(20, 20) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
}

TEST_CASE("compare.verb.thresholds_are_cli_surface") {
    const Fixtures fx;
    // Structural pair passes tier 2 when the caller explicitly opens the
    // budgets — thresholds are the caller's contract, never hidden policy.
    const VerbOutcome out =
        invoke({"compare", fx.base, fx.shifted, "--tolerance", "255", "--max-mean", "1024"});
    CHECK(out.exit == Exit::Ok);
    CHECK(field(out.payload, "hash_equal").dump() == "false");
    CHECK(field(out.payload, "pass").dump() == "true");

    // Out-of-range thresholds are usage errors (exit 2), refused before IO.
    const VerbOutcome bad = invoke({"compare", fx.base, fx.noise, "--tolerance", "256"});
    CHECK(bad.exit == Exit::Usage);
    CHECK(unwrap(bad.error).code == "usage.invalid_value");
}

TEST_CASE("compare.verb.io_and_validation_failures_exit_3") {
    const Fixtures fx;
    // Unreadable input.
    const VerbOutcome missing = invoke({"compare", fx.base, fx.dir.file("absent.png")});
    CHECK(missing.exit == Exit::Validation);
    CHECK(unwrap(missing.error).code == "rhi.image_read");

    // Undecodable input.
    const std::string garbage = fx.dir.file("garbage.png");
    REQUIRE_FALSE(base::write_file(garbage, "not a png", "test.io").has_value());
    const VerbOutcome broken = invoke({"compare", fx.base, garbage});
    CHECK(broken.exit == Exit::Validation);
    CHECK(unwrap(broken.error).code == "rhi.image_read");

    // Unwritable diff destination.
    const VerbOutcome unwritable =
        invoke({"compare", fx.base, fx.shifted, "--diff", fx.dir.file("no/such/dir/diff.png")});
    CHECK(unwritable.exit == Exit::Validation);
    CHECK(unwrap(unwritable.error).code == "rhi.image_write");
}

TEST_CASE("compare.verb.unknown_op_is_usage") {
    const Fixtures fx;
    const VerbOutcome out = invoke({"capture", fx.base, fx.identical});
    CHECK(out.exit == Exit::Usage);
    CHECK(unwrap(out.error).code == "usage.unknown_op");
}

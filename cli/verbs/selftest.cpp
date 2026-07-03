// `midday selftest` — runs the doctest registry embedded in the engine binary
// (spec section 14: "unit tests compiled into the binary, doctest pattern").
// This translation unit owns the doctest implementation for the whole binary.

#define DOCTEST_CONFIG_IMPLEMENT
#include "cli/verb.h"
#include "core/base/hex.h"
#include "core/math/fixture.h"
#include "doctest/doctest.h"

#include <sstream>
#include <string>

namespace midday::cli {
namespace {

// Captures run totals; doctest's Context::run() only reports pass/fail.
struct RunTotals {
    int cases = 0;
    int cases_failed = 0;
    int asserts = 0;
    int asserts_failed = 0;
};

RunTotals g_totals; // selftest runs at most once per process

struct CountingListener : doctest::IReporter {
    explicit CountingListener(const doctest::ContextOptions&) {}

    void test_run_end(const doctest::TestRunStats& stats) override {
        g_totals.cases = static_cast<int>(stats.numTestCasesPassingFilters);
        g_totals.cases_failed = static_cast<int>(stats.numTestCasesFailed);
        g_totals.asserts = static_cast<int>(stats.numAsserts);
        g_totals.asserts_failed = static_cast<int>(stats.numAssertsFailed);
    }

    // Uninteresting hooks: this listener only counts.
    void report_query(const doctest::QueryData&) override {}

    void test_run_start() override {}

    void test_case_start(const doctest::TestCaseData&) override {}

    void test_case_reenter(const doctest::TestCaseData&) override {}

    void test_case_end(const doctest::CurrentTestCaseStats&) override {}

    void test_case_exception(const doctest::TestCaseException&) override {}

    void subcase_start(const doctest::SubcaseSignature&) override {}

    void subcase_end() override {}

    void log_assert(const doctest::AssertData&) override {}

    void log_message(const doctest::MessageData&) override {}

    void test_case_skipped(const doctest::TestCaseData&) override {}
};

REGISTER_LISTENER("midday-count", 1, CountingListener);

} // namespace

VerbOutcome verb_selftest(const VerbArgs& args) {
    std::string filter;
    for (size_t i = 0; i < args.rest.size(); ++i) {
        std::string_view arg = args.rest[i];
        if (arg == "--filter" && i + 1 < args.rest.size()) {
            filter = std::string(args.rest[++i]);
        } else if (arg.starts_with("--filter=")) {
            filter = std::string(arg.substr(9));
        }
    }

    doctest::Context context;
    context.setOption("no-intro", true);
    context.setOption("no-version", true);
    if (!filter.empty())
        context.addFilter("test-case", filter.c_str());

    // In JSON mode the envelope is the only thing on stdout: doctest's
    // console report is captured and surfaced through error.details on failure.
    std::ostringstream captured;
    if (args.json) {
        context.setOption("no-colors", true);
        context.setCout(&captured);
    }

    const int failed = context.run();

    VerbOutcome out;
    out.payload.set("cases", g_totals.cases);
    out.payload.set("cases_failed", g_totals.cases_failed);
    out.payload.set("asserts", g_totals.asserts);
    out.payload.set("asserts_failed", g_totals.asserts_failed);
    if (!filter.empty())
        out.payload.set("filter", filter);
    // The m0-math-stdlib determinism fixture digest: CI's determinism lane
    // byte-compares this field across independent runs/hosts.
    out.payload.set("math_fixture_hash",
                    midday::base::hex64(midday::math::determinism_fixture_hash()));

    if (failed != 0) {
        out.exit = Exit::Failure;
        Error error;
        error.code = "selftest.failed";
        error.message = std::to_string(g_totals.cases_failed) + " of " +
                        std::to_string(g_totals.cases) + " test cases failed";
        if (args.json)
            error.details.set("report", captured.str());
        out.error = std::move(error);
    }
    out.human = "selftest: " + std::to_string(g_totals.cases - g_totals.cases_failed) + "/" +
                std::to_string(g_totals.cases) + " cases passed";
    return out;
}

} // namespace midday::cli

// codegen.selfhost.* doctests — the byte-equivalence harness (written
// FIRST, test-first): the self-hosted TS-on-QuickJS generator must produce
// the SAME FOUR ARTIFACTS byte-identically to the native bootstrap for the
// whole bootstrap corpus (synthetic golden + number-edge + live document +
// CLI envelope form) before it is authoritative (MILESTONE_0 "Codegen
// Bootstrap"). Validation-error codes must match the bootstrap classes too.

#include "api/engine_api.h"
#include "cli/help.h"
#include "cli/verb.h"
#include "testkit/doctest.h"
#include "testkit/codegen_corpus.h"
#include "testkit/doctest_unwrap.h"
#include "tools/codegen_bootstrap/codegen.h"
#include "ts/codegen/selfhost.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace api = midday::api;
namespace base = midday::base;
namespace cli = midday::cli;
namespace codegen = midday::codegen;
namespace selfhost = midday::selfhost;
namespace testkit = midday::testkit;

using base::Json;

namespace {

// Shared content-hash cache: sound by construction (keys cover toolchain +
// source bytes, D-BUILD-063), so selftest reuses of the default dir only
// skip re-transpiles, never observe stale artifacts.
selfhost::Config corpus_config() {
    selfhost::Config config;
    config.origin = "<corpus>";
    return config;
}

// First differing byte as "offset N (line L): 'native...' vs 'selfhost...'"
// so an equivalence failure reads as a location, not a 5 KB string dump.
std::string first_diff(std::string_view native, std::string_view ts) {
    const std::size_t limit = native.size() < ts.size() ? native.size() : ts.size();
    std::size_t offset = 0;
    while (offset < limit && native[offset] == ts[offset])
        ++offset;
    if (offset == limit && native.size() == ts.size())
        return "equal";
    std::size_t line = 1;
    for (std::size_t i = 0; i < offset; ++i)
        line += native[i] == '\n' ? 1 : 0;
    const auto excerpt = [offset](std::string_view text) {
        return std::string(text.substr(offset, 40));
    };
    return "offset " + std::to_string(offset) + " (line " + std::to_string(line) + "): '" +
           excerpt(native) + "' vs '" + excerpt(ts) + "'";
}

void check_pair(const char* name, std::string_view native, std::string_view ts) {
    CHECK_MESSAGE(native == ts, name << ": " << first_diff(native, ts));
}

// The equivalence claim itself: both generators on the same bytes.
void check_equivalence(std::string_view input) {
    codegen::LoadResult loaded = codegen::load_document(input, "<corpus>");
    REQUIRE_MESSAGE(!loaded.error.has_value(),
                    (loaded.error ? loaded.error->message : std::string()));
    const codegen::Outputs native = codegen::generate(loaded.document);

    selfhost::RunResult ts = selfhost::run_generator(input, corpus_config());
    REQUIRE_MESSAGE(!ts.error.has_value(), (ts.error ? ts.error->message : std::string()));

    check_pair("engine.d.ts", native.dts, ts.files.dts);
    check_pair("schema_manifest.json", native.manifest, ts.files.manifest);
    check_pair("api_docs.md", native.docs, ts.files.docs);
    check_pair("bindings_spec.json", native.bindings, ts.files.bindings);
    CHECK(ts.files.api_compat_hash == loaded.document.find("api_compat_hash")->as_string());
}

base::Error selfhost_error(std::string_view input) {
    selfhost::RunResult result = selfhost::run_generator(input, corpus_config());
    return midday::testkit::unwrap(result.error);
}

Json live_document() {
    const api::BootRegistry boot;
    Json schemas = Json::array();
    for (const cli::VerbSpec* spec : cli::verbs())
        schemas.push(cli::verb_schema(*spec));
    return api::build_document(boot.registry, schemas, "test");
}

} // namespace

TEST_CASE("codegen.selfhost.synthetic: byte-equal to bootstrap on the golden corpus") {
    check_equivalence(testkit::kCodegenSyntheticDocument);
}

TEST_CASE("codegen.selfhost.numbers: byte-equal on the number-formatting edge corpus") {
    check_equivalence(testkit::kCodegenNumberDocument);

    // Spot-pin the canonical spellings so this cannot pass by both sides
    // agreeing on WRONG bytes (the native writer is byte-pin-corpus-guarded;
    // these mirror those pins at the artifact level).
    selfhost::RunResult ts =
        selfhost::run_generator(testkit::kCodegenNumberDocument, corpus_config());
    REQUIRE_FALSE(ts.error.has_value());
    const std::string& bindings = ts.files.bindings;
    for (const std::string_view needle : {std::string_view("\"default\":2.5e-08"),
                                          std::string_view("\"default\":1e-07"),
                                          std::string_view("\"default\":1e-04"),
                                          std::string_view("\"default\":1e-06"),
                                          std::string_view("\"default\":1e+21"),
                                          std::string_view("\"default\":100}"),
                                          std::string_view("\"default\":-0}"),
                                          std::string_view("\"default\":5e-324"),
                                          std::string_view("\"default\":9007199254740993"),
                                          std::string_view("\"default\":9223372036854775807"),
                                          std::string_view("\"default\":-9223372036854775808"),
                                          std::string_view("\"default\":9223372036854775808"),
                                          std::string_view("\"default\":0.30000000000000004"),
                                          std::string_view("1.7976931348623157e+308"),
                                          std::string_view("[0.25,-0.125,12345.6789]")})
        CHECK_MESSAGE(bindings.find(needle) != std::string::npos, needle);
}

TEST_CASE("codegen.selfhost.live: byte-equal on the live document and its CLI envelope form") {
    const Json document = live_document();
    check_equivalence(document.dump());

    // `midday api dump --json` envelope: unwrap parity (api/CODEGEN.md).
    Json envelope = Json::object();
    envelope.set("ok", true);
    envelope.set("verb", "api");
    envelope.set("exit_code", 0);
    envelope.set("api", document);
    check_equivalence(envelope.dump());
}

TEST_CASE("codegen.selfhost.errors: validation failures carry the bootstrap error codes") {
    CHECK(selfhost_error("{ nope").code == "json.parse");
    CHECK(selfhost_error(R"({"format_version": 99})").code == "api.malformed");

    std::string bad(testkit::kCodegenSyntheticDocument);
    const std::string needle = "\"entity_ref\"";
    bad.replace(bad.find(needle), needle.size(), "\"flarb\"");
    const base::Error unknown_type = selfhost_error(bad);
    CHECK(unknown_type.code == "codegen.unknown_type");
    const Json* offending = unknown_type.details.find("type");
    REQUIRE(offending != nullptr);
    CHECK(offending->as_string() == "flarb");

    std::string no_payload(testkit::kCodegenSyntheticDocument);
    const std::string payload_key = "\"payload\"";
    no_payload.replace(no_payload.find(payload_key), payload_key.size(), "\"payloax\"");
    CHECK(selfhost_error(no_payload).code == "codegen.malformed");

    // Two events pascal-casing to the same interface name collide.
    const base::Error collision = selfhost_error(R"json({
 "format_version": 1, "engine_version": "t", "api_compat_hash": "00000000000000aa",
 "classes": [],
 "events": [
  {"name": "a.b", "payload": [], "compat_hash": "00000000000000ab"},
  {"name": "a_b", "payload": [], "compat_hash": "00000000000000ac"}],
 "functions": [], "verbs": []
})json");
    CHECK(collision.code == "codegen.duplicate_symbol");
    const Json* symbol = collision.details.find("symbol");
    REQUIRE(symbol != nullptr);
    CHECK(symbol->as_string() == "ABEvent");
}

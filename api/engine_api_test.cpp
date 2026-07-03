// api.* selftests — written first (test-first): byte determinism of the
// document, the hash contract (docs out, signatures + order in), diff
// semantics on real signature changes, and the `midday api` verb end to end
// through the CLI framework (dump -> mutate fixture -> diff).

#include "api/engine_api.h"
#include "cli/help.h"
#include "cli/verb.h"
#include "core/base/file_io.h"
#include "core/base/hex.h"
#include "core/base/name.h"
#include "core/reflect/type_model.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <initializer_list>
#include <ostream> // MSVC: doctest stringifies string_view via operator<< (D-BUILD-016)
#include <string>
#include <string_view>

namespace api = midday::api;
namespace base = midday::base;
namespace cli = midday::cli;
namespace reflect = midday::reflect;

using base::Json;
using midday::testkit::TempDir;
using midday::testkit::unwrap;

namespace {

reflect::EventDesc probe_event(std::string_view name,
                               std::string doc,
                               reflect::TypeKind field_kind,
                               std::string field_doc) {
    reflect::EventDesc event;
    event.name = base::Name(name);
    event.doc = std::move(doc);
    event.payload.push_back(reflect::EventFieldDesc{
        base::Name("value"), reflect::TypeDesc::scalar(field_kind), std::move(field_doc)});
    return event;
}

reflect::MethodDesc probe_function(std::string_view name, reflect::TypeKind param_kind) {
    reflect::MethodDesc function;
    function.name = base::Name(name);
    function.params.push_back(
        reflect::ParamDesc{base::Name("x"), reflect::TypeDesc::scalar(param_kind), Json()});
    function.returns = reflect::TypeDesc::scalar(reflect::TypeKind::kFloat);
    return function;
}

Json doc_of(const reflect::Registry& registry) {
    return api::build_document(registry, Json::array(), "test");
}

std::string top_hash(const Json& document) {
    return document.find("api_compat_hash")->as_string();
}

cli::VerbOutcome run_api(std::initializer_list<std::string_view> tokens) {
    cli::Invocation inv;
    inv.verb = "api";
    inv.rest.assign(tokens.begin(), tokens.end());
    cli::ParsedArgs parsed = cli::parse_verb_args(cli::api_spec(), inv);
    REQUIRE(!parsed.usage.has_value());
    return cli::api_spec().run(parsed.args);
}

} // namespace

TEST_CASE("api.document: two independent boots dump byte-identical, shape pinned") {
    Json verb_schemas = Json::array();
    for (const cli::VerbSpec* spec : cli::verbs())
        verb_schemas.push(cli::verb_schema(*spec));

    const api::BootRegistry first;
    const api::BootRegistry second;
    const Json a = api::build_document(first.registry, verb_schemas, "test");
    const Json b = api::build_document(second.registry, verb_schemas, "test");
    CHECK(a.dump() == b.dump()); // two independent runs, never a self-diff

    // Top-level key order is the contract downstream codegen parses.
    const std::string bytes = a.dump();
    CHECK(bytes.starts_with("{\"format_version\":1,\"engine_version\":\"test\","
                            "\"api_compat_hash\":\""));
    const auto pos = [&bytes](std::string_view key) {
        return bytes.find("\"" + std::string(key) + "\":[");
    };
    CHECK(pos("classes") < pos("events"));
    CHECK(pos("events") < pos("functions"));
    CHECK(pos("functions") < pos("verbs"));

    // Census: the full CORE vocabulary and the whole verb manifest are in.
    CHECK(a.find("events")->elements().size() == first.registry.events().size());
    CHECK(a.find("events")->elements()[0].find("name")->as_string() == "trigger.entered");
    CHECK(a.find("functions")->elements().size() == first.registry.functions().size());
    CHECK(!a.find("functions")->elements().empty());
    CHECK(a.find("verbs")->elements().size() == cli::verbs().size());
    CHECK(!api::check_document(a).has_value());

    // Registry entries carry their registration-time compat hashes verbatim.
    const auto* clamp = first.registry.find_function(base::Name("clamp"));
    REQUIRE(clamp != nullptr);
    for (const Json& fn : a.find("functions")->elements())
        if (fn.find("name")->as_string() == "clamp")
            CHECK(fn.find("compat_hash")->as_string() == base::hex64(clamp->desc.compat_hash));
}

TEST_CASE("api.hash: docs are outside every hash, signatures and order are inside") {
    reflect::Registry base_reg;
    base_reg.add_event(probe_event("probe.fired", "doc one", reflect::TypeKind::kInt, "field"));
    reflect::Registry doc_reg;
    doc_reg.add_event(probe_event("probe.fired", "DIFFERENT", reflect::TypeKind::kInt, "other"));
    reflect::Registry sig_reg;
    sig_reg.add_event(probe_event("probe.fired", "doc one", reflect::TypeKind::kFloat, "field"));

    const Json base_doc = doc_of(base_reg);
    const Json docs_changed = doc_of(doc_reg);
    const Json sig_changed = doc_of(sig_reg);
    CHECK(base_doc.dump() != docs_changed.dump());       // docs ARE in the document
    CHECK(top_hash(base_doc) == top_hash(docs_changed)); // ...but OUT of the hash
    CHECK(top_hash(base_doc) != top_hash(sig_changed));  // a type change IS drift
    CHECK(top_hash(api::build_document(base_reg, Json::array(), "other-version")) ==
          top_hash(base_doc)); // engine_version is outside the hash too

    // Entry order is part of the top-level hash (canonical order is contract).
    reflect::Registry fwd;
    fwd.add_function(probe_function("fa", reflect::TypeKind::kFloat));
    fwd.add_function(probe_function("fb", reflect::TypeKind::kFloat));
    reflect::Registry rev;
    rev.add_function(probe_function("fb", reflect::TypeKind::kFloat));
    rev.add_function(probe_function("fa", reflect::TypeKind::kFloat));
    CHECK(top_hash(doc_of(fwd)) != top_hash(doc_of(rev)));

    // Verb hashes: summary/doc edits are not drift; a type change is.
    static constexpr cli::FlagSpec kIntFlag[] = {{.name = "n", .type = "int", .doc = "a number"}};
    static constexpr cli::FlagSpec kIntFlagOtherDoc[] = {
        {.name = "n", .type = "int", .doc = "REWORDED"}};
    static constexpr cli::FlagSpec kFloatFlag[] = {
        {.name = "n", .type = "float", .doc = "a number"}};
    const auto verb_doc = [&base_reg](const cli::VerbSpec& spec) {
        Json schemas = Json::array();
        schemas.push(cli::verb_schema(spec));
        return api::build_document(base_reg, schemas, "test");
    };
    const Json va = verb_doc({.name = "v", .summary = "one", .flags = kIntFlag});
    const Json vb = verb_doc({.name = "v", .summary = "TWO", .flags = kIntFlagOtherDoc});
    const Json vc = verb_doc({.name = "v", .summary = "one", .flags = kFloatFlag});
    const auto verb_hash = [](const Json& doc) {
        return doc.find("verbs")->elements()[0].find("compat_hash")->as_string();
    };
    CHECK(verb_hash(va) == verb_hash(vb));
    CHECK(verb_hash(va) != verb_hash(vc));
    CHECK(top_hash(va) == top_hash(vb));
}

TEST_CASE("api.diff: identical documents, then real signature changes per section") {
    reflect::Registry old_reg;
    old_reg.add_event(probe_event("probe.fired", "d", reflect::TypeKind::kInt, "f"));
    old_reg.add_function(probe_function("keep", reflect::TypeKind::kFloat));
    old_reg.add_function(probe_function("gone", reflect::TypeKind::kFloat));
    const Json old_doc = doc_of(old_reg);

    const api::Diff same = api::diff_documents(old_doc, old_doc);
    CHECK(same.identical);
    CHECK(same.report.find("identical")->as_bool());
    CHECK(same.report.find("added")->elements().empty());
    CHECK(same.report.find("removed")->elements().empty());
    CHECK(same.report.find("changed")->elements().empty());

    reflect::Registry new_reg;
    new_reg.add_event(probe_event("probe.fired", "d", reflect::TypeKind::kFloat, "f")); // changed
    new_reg.add_function(probe_function("keep", reflect::TypeKind::kFloat));            // kept
    new_reg.add_function(probe_function("born", reflect::TypeKind::kInt));              // added
    const Json new_doc = doc_of(new_reg); // "gone" removed

    const api::Diff drift = api::diff_documents(old_doc, new_doc);
    CHECK(!drift.identical);
    REQUIRE(drift.report.find("changed")->elements().size() == 1);
    const Json& changed = drift.report.find("changed")->elements()[0];
    CHECK(changed.find("kind")->as_string() == "event");
    CHECK(changed.find("name")->as_string() == "probe.fired");
    CHECK(changed.find("old_compat_hash")->as_string() != changed.find("compat_hash")->as_string());
    REQUIRE(drift.report.find("added")->elements().size() == 1);
    CHECK(drift.report.find("added")->elements()[0].find("kind")->as_string() == "function");
    CHECK(drift.report.find("added")->elements()[0].find("name")->as_string() == "born");
    REQUIRE(drift.report.find("removed")->elements().size() == 1);
    CHECK(drift.report.find("removed")->elements()[0].find("name")->as_string() == "gone");
}

TEST_CASE("api.verb: dump/diff fixture round-trip through the CLI framework") {
    const cli::VerbOutcome dumped = run_api({"dump"});
    CHECK(dumped.exit == cli::Exit::Ok);
    const Json* document = dumped.payload.find("api");
    REQUIRE(document != nullptr);
    CHECK(dumped.human == document->dump()); // human mode prints the document bytes
    CHECK(document->dump() == run_api({"dump"}).payload.find("api")->dump());
    CHECK(!api::check_document(*document).has_value());

    TempDir dir("api-fixture");

    // --out writes exactly the printed bytes plus the trailing newline.
    const std::string out_path = dir.file("dumped.json");
    const cli::VerbOutcome wrote = run_api({"dump", "--out", out_path});
    CHECK(wrote.exit == cli::Exit::Ok);
    CHECK(wrote.payload.find("out")->as_string() == out_path);
    base::ReadFileResult written = base::read_file(out_path, "api.io");
    REQUIRE(!written.error.has_value());
    CHECK(written.bytes == document->dump() + "\n");

    // diff against the just-dumped fixture: identical, exit 0.
    const cli::VerbOutcome same = run_api({"diff", out_path});
    CHECK(same.exit == cli::Exit::Ok);
    CHECK(same.payload.find("identical")->as_bool());

    // Mutate one function's compat hash in a copy: diff exits 1 and reports
    // the changed entry with both hashes.
    const Json& probe = document->find("functions")->elements()[0];
    const std::string real_hash = probe.find("compat_hash")->as_string();
    const std::string fake_hash = "0123456789abcdef";
    REQUIRE(real_hash != fake_hash);
    std::string mutated = document->dump();
    mutated.replace(mutated.find(real_hash), real_hash.size(), fake_hash);
    const std::string drift_path = dir.file("drifted.json");
    REQUIRE(!base::write_file(drift_path, mutated + "\n", "api.io").has_value());

    const cli::VerbOutcome drift = run_api({"diff", drift_path});
    CHECK(drift.exit == cli::Exit::Failure);
    CHECK(unwrap(drift.error).code == "api.drift");
    CHECK(!drift.payload.find("identical")->as_bool());
    REQUIRE(drift.payload.find("changed")->elements().size() == 1);
    const Json& changed = drift.payload.find("changed")->elements()[0];
    CHECK(changed.find("kind")->as_string() == "function");
    CHECK(changed.find("name")->as_string() == probe.find("name")->as_string());
    CHECK(changed.find("old_compat_hash")->as_string() == fake_hash);
    CHECK(changed.find("compat_hash")->as_string() == real_hash);

    // Bad baselines are the validation class (exit 3), structured.
    const std::string junk_path = dir.file("junk.json");
    REQUIRE(!base::write_file(junk_path, "{\"nope\":1}\n", "api.io").has_value());
    const cli::VerbOutcome malformed = run_api({"diff", junk_path});
    CHECK(malformed.exit == cli::Exit::Validation);
    CHECK(unwrap(malformed.error).code == "api.malformed");

    const cli::VerbOutcome unreadable = run_api({"diff", dir.file("absent.json")});
    CHECK(unreadable.exit == cli::Exit::Validation);
    CHECK(unwrap(unreadable.error).code == "api.io");

    const std::string garbled_path = dir.file("garbled.json");
    REQUIRE(!base::write_file(garbled_path, "{not json", "api.io").has_value());
    const cli::VerbOutcome garbled = run_api({"diff", garbled_path});
    CHECK(garbled.exit == cli::Exit::Validation);
    CHECK(unwrap(garbled.error).code == "json.parse");
}

TEST_CASE("api.verb: action usage errors are structured and exit 2") {
    const cli::VerbOutcome unknown = run_api({"frobnicate"});
    CHECK(unknown.exit == cli::Exit::Usage);
    CHECK(unwrap(unknown.error).code == "usage.unknown_action");
    CHECK(unwrap(unknown.error).details.find("known")->dump() == "[\"dump\",\"diff\",\"codegen\"]");

    const cli::VerbOutcome missing = run_api({"diff"});
    CHECK(missing.exit == cli::Exit::Usage);
    CHECK(unwrap(missing.error).code == "usage.missing_argument");

    const cli::VerbOutcome extra = run_api({"dump", "old.json"});
    CHECK(extra.exit == cli::Exit::Usage);
    CHECK(unwrap(extra.error).code == "usage.unexpected_argument");

    const cli::VerbOutcome misflag = run_api({"diff", "old.json", "--out", "x"});
    CHECK(misflag.exit == cli::Exit::Usage);
    CHECK(unwrap(misflag.error).code == "usage.unexpected_flag");
}

// Tests for the CLI JSON envelope — written FIRST (test-first), the envelope
// implementation in cli/envelope.* exists to make these pass. JSON itself is
// core/base/json.* (tested under core.json.*); these cases cover only what
// the CLI adds: the envelope layout and its invariants.
//
// The envelope contract is defined once, in formats/cli_envelope.schema.json:
//   required: ok (bool), verb (string), exit_code (0|1|2|3)
//   ok == (exit_code == 0); error{code,message[,details]} required iff !ok
//   verb payload fields live at the top level next to the envelope fields.

#include "cli/envelope.h"
#include "cli/verb.h"
#include "doctest/doctest.h"

using midday::cli::Error;
using midday::cli::Exit;
using midday::cli::Json;

TEST_CASE("cli.envelope: success envelope has ok/verb/exit_code first, payload merged after") {
    Json payload = Json::object();
    payload.set("name", "midday");
    payload.set("version", "0.1.0");
    Json env = midday::cli::make_envelope("version", Exit::Ok, payload, nullptr);
    CHECK(env.dump() == "{\"ok\":true,\"verb\":\"version\",\"exit_code\":0,"
                        "\"name\":\"midday\",\"version\":\"0.1.0\"}");
}

TEST_CASE("cli.envelope: failure envelope carries error{code,message} and nonzero exit") {
    Error err;
    err.code = "usage.unknown_verb";
    err.message = "unknown verb 'frobnicate'";
    Json env = midday::cli::make_envelope("frobnicate", Exit::Usage, Json::object(), &err);
    const Json* ok = env.find("ok");
    const Json* exit_code = env.find("exit_code");
    const Json* error = env.find("error");
    REQUIRE(ok != nullptr);
    REQUIRE(exit_code != nullptr);
    REQUIRE(error != nullptr);
    CHECK(ok->dump() == "false");
    CHECK(exit_code->dump() == "2");
    REQUIRE(error->find("code") != nullptr);
    REQUIRE(error->find("message") != nullptr);
    CHECK(error->find("code")->dump() == "\"usage.unknown_verb\"");
}

TEST_CASE("cli.envelope: envelope invariants hold even for careless callers") {
    // ok must equal (exit_code == 0): a failure exit without an Error still
    // yields a schema-valid envelope with a synthesized error object.
    Json env = midday::cli::make_envelope("broken", Exit::Failure, Json::object(), nullptr);
    CHECK(env.find("ok")->dump() == "false");
    const Json* error = env.find("error");
    REQUIRE(error != nullptr);
    REQUIRE(error->find("code") != nullptr);
    REQUIRE(error->find("message") != nullptr);

    // Payload fields may not shadow envelope fields; envelope wins.
    Json payload = Json::object();
    payload.set("ok", false);
    payload.set("name", "midday");
    Json env2 = midday::cli::make_envelope("version", Exit::Ok, payload, nullptr);
    CHECK(env2.find("ok")->dump() == "true");
    CHECK(env2.find("name")->dump() == "\"midday\"");
}

TEST_CASE("cli.version: payload declares name midday and a version") {
    midday::cli::VerbArgs args;
    args.json = true;
    midday::cli::VerbOutcome out = midday::cli::version_spec().run(args);
    CHECK(out.exit == Exit::Ok);
    REQUIRE(out.payload.find("name") != nullptr);
    CHECK(out.payload.find("name")->dump() == "\"midday\"");
    REQUIRE(out.payload.find("version") != nullptr);
}

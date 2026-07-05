// loader.input — `*.input.yaml`: action -> device bindings, virtual-stick
// composites, project-wide conflict detection, and the `*.input_profile.yaml`
// rebinding overlay (m1-input-actions). Structure mirrors events_load_test.cpp
// deliberately (same fixture shape, same refusal-carries-file:line:col
// discipline).

#include "core/base/file_io.h"
#include "core/loader/loader.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <filesystem>
#include <optional>
#include <string>

using namespace midday;
using namespace midday::loader;
using midday::testkit::unwrap;

namespace {

std::string generic(const std::string& native) {
    return std::filesystem::path(native).generic_string();
}

struct InputFixture {
    testkit::TempDir dir{"loader-input"};
    ActionMapDecl decl;

    std::optional<base::Error> load(const std::string& text) {
        const std::string path = dir.file("game.input.yaml");
        REQUIRE_FALSE(base::write_file(path, text, "test.io").has_value());
        return load_input_file(path, decl);
    }
};

} // namespace

TEST_CASE("loader.input: actions, bindings, and sticks load") {
    InputFixture fix;
    auto error = fix.load("format: 1\n"
                          "actions:\n"
                          "  jump:\n"
                          "    bindings:\n"
                          "      - {device: keyboard, control: space}\n"
                          "      - {device: gamepad, control: button_south}\n"
                          "  move_up: {bindings: [{device: keyboard, control: w}]}\n"
                          "  move_down: {bindings: [{device: keyboard, control: s}]}\n"
                          "  move_left: {bindings: [{device: keyboard, control: a}]}\n"
                          "  move_right: {bindings: [{device: keyboard, control: d}]}\n"
                          "  reserved: {}\n"
                          "sticks:\n"
                          "  move:\n"
                          "    up: move_up\n"
                          "    down: move_down\n"
                          "    left: move_left\n"
                          "    right: move_right\n"
                          "    deadzone: 0.3\n");
    REQUIRE_FALSE(error.has_value());
    REQUIRE(fix.decl.actions.size() == 6);
    CHECK(fix.decl.has_action("jump"));
    CHECK_FALSE(fix.decl.has_action("nope"));

    const ActionDesc* jump = fix.decl.find_action("jump");
    REQUIRE(jump != nullptr);
    REQUIRE(jump->bindings.size() == 2);
    CHECK(jump->bindings[0].device == DeviceKind::kKeyboard);
    CHECK(jump->bindings[0].control == "space");
    CHECK(jump->bindings[1].device == DeviceKind::kGamepad);
    CHECK(jump->bindings[1].control == "button_south");

    const ActionDesc* reserved = fix.decl.find_action("reserved");
    REQUIRE(reserved != nullptr);
    CHECK(reserved->bindings.empty()); // "name: {}" declares a binding-less action

    REQUIRE(fix.decl.sticks.size() == 1);
    const StickDesc& move = fix.decl.sticks[0];
    CHECK(move.name == "move");
    CHECK(move.up == "move_up");
    CHECK(move.down == "move_down");
    CHECK(move.left == "move_left");
    CHECK(move.right == "move_right");
    CHECK(move.deadzone == doctest::Approx(0.3));
}

TEST_CASE("loader.input: stick deadzone defaults to 0.2 when omitted") {
    InputFixture fix;
    auto error = fix.load("format: 1\n"
                          "actions:\n"
                          "  a: {bindings: [{device: keyboard, control: up}]}\n"
                          "  b: {bindings: [{device: keyboard, control: down}]}\n"
                          "  c: {bindings: [{device: keyboard, control: left}]}\n"
                          "  d: {bindings: [{device: keyboard, control: right}]}\n"
                          "sticks:\n"
                          "  s: {up: a, down: b, left: c, right: d}\n");
    REQUIRE_FALSE(error.has_value());
    CHECK(fix.decl.sticks[0].deadzone == doctest::Approx(0.2));
}

TEST_CASE("loader.input: the format gate is mandatory and versioned") {
    InputFixture fix;
    auto missing = fix.load("actions: {a: {}}\n");
    REQUIRE(missing.has_value());
    CHECK(unwrap(missing).code == "loader.bad_format");

    auto future = fix.load("format: 2\nactions: {a: {}}\n");
    REQUIRE(future.has_value());
    CHECK(unwrap(future).code == "loader.bad_format");
}

TEST_CASE("loader.input: strict refusals carry file:line:col") {
    InputFixture fix;
    auto unknown = fix.load("format: 1\nactions: {a: {}}\nbogus: 1\n");
    REQUIRE(unknown.has_value());
    CHECK(unwrap(unknown).code == "loader.unknown_key");
    CHECK(unwrap(unknown).details.find("line")->as_int() == 3);

    auto bad_device = fix.load("format: 1\n"
                               "actions:\n"
                               "  a: {bindings: [{device: joystick, control: x}]}\n");
    REQUIRE(bad_device.has_value());
    CHECK(unwrap(bad_device).code == "loader.bad_value");
    CHECK(unwrap(bad_device).message.find("unknown device 'joystick'") != std::string::npos);

    auto empty_name = fix.load("format: 1\nactions: {\"\": {}}\n");
    REQUIRE(empty_name.has_value());
    CHECK(unwrap(empty_name).code == "loader.bad_value");
}

TEST_CASE("loader.input: duplicate action names refuse, even across files") {
    InputFixture fix;
    REQUIRE_FALSE(fix.load("format: 1\nactions: {jump: {}}\n").has_value());

    testkit::TempDir dir2{"loader-input-2"};
    const std::string path2 = dir2.file("more.input.yaml");
    REQUIRE_FALSE(
        base::write_file(path2, "format: 1\nactions: {jump: {}}\n", "test.io").has_value());
    auto duplicate = load_input_file(path2, fix.decl);
    REQUIRE(duplicate.has_value());
    CHECK(unwrap(duplicate).code == "loader.duplicate");
    CHECK(unwrap(duplicate).message.find("jump") != std::string::npos);
}

TEST_CASE("loader.input: repeating the same binding within one action refuses") {
    InputFixture fix;
    auto error = fix.load("format: 1\n"
                          "actions:\n"
                          "  jump:\n"
                          "    bindings:\n"
                          "      - {device: keyboard, control: space}\n"
                          "      - {device: keyboard, control: space}\n");
    REQUIRE(error.has_value());
    CHECK(unwrap(error).code == "loader.duplicate");
}

TEST_CASE("loader.input: two DIFFERENT actions binding the same control conflict") {
    InputFixture fix;
    auto error = fix.load("format: 1\n"
                          "actions:\n"
                          "  jump: {bindings: [{device: keyboard, control: space}]}\n"
                          "  crouch: {bindings: [{device: keyboard, control: space}]}\n");
    REQUIRE(error.has_value());
    CHECK(unwrap(error).code == "input.conflict");
    CHECK(unwrap(error).message.find("jump") != std::string::npos);
    CHECK(unwrap(error).message.find("crouch") != std::string::npos);
    CHECK(unwrap(error).details.find("action_a")->as_string() == "jump");
    CHECK(unwrap(error).details.find("action_b")->as_string() == "crouch");
}

TEST_CASE("loader.input: a stick referencing an undeclared action refuses loader.bad_ref") {
    InputFixture fix;
    auto error = fix.load("format: 1\n"
                          "actions:\n"
                          "  a: {bindings: [{device: keyboard, control: up}]}\n"
                          "sticks:\n"
                          "  s: {up: a, down: nope, left: a, right: a}\n");
    REQUIRE(error.has_value());
    CHECK(unwrap(error).code == "loader.bad_ref");
    CHECK(unwrap(error).message.find("nope") != std::string::npos);
}

TEST_CASE("loader.input: an out-of-range deadzone refuses") {
    InputFixture fix;
    auto error = fix.load("format: 1\n"
                          "actions:\n"
                          "  a: {bindings: [{device: keyboard, control: up}]}\n"
                          "sticks:\n"
                          "  s: {up: a, down: a, left: a, right: a, deadzone: 1.0}\n");
    REQUIRE(error.has_value());
    CHECK(unwrap(error).code == "loader.bad_value");
    CHECK(unwrap(error).message.find("deadzone") != std::string::npos);
}

TEST_CASE("loader.input: load_project_input merges every *.input.yaml under a root") {
    testkit::TempDir dir{"loader-project-input"};
    REQUIRE_FALSE(base::write_file(dir.file("a.input.yaml"),
                                   "format: 1\nactions: {jump: {bindings: "
                                   "[{device: keyboard, control: space}]}}\n",
                                   "test.io")
                      .has_value());
    REQUIRE_FALSE(
        base::write_file(
            dir.file("b.input.yaml"),
            "format: 1\nactions: {crouch: {bindings: [{device: keyboard, control: c}]}}\n",
            "test.io")
            .has_value());

    ProjectInputResult merged = load_project_input(dir.path.string());
    REQUIRE_FALSE(merged.error.has_value());
    REQUIRE(merged.files.size() == 2);
    CHECK(merged.files[0] == generic(dir.file("a.input.yaml")));
    CHECK(merged.files[1] == generic(dir.file("b.input.yaml")));
    CHECK(merged.decl.actions.size() == 2);
    CHECK(merged.decl.has_action("jump"));
    CHECK(merged.decl.has_action("crouch"));
}

TEST_CASE("loader.input: load_project_input catches a cross-file conflict") {
    testkit::TempDir dir{"loader-project-input-conflict"};
    REQUIRE_FALSE(base::write_file(dir.file("a.input.yaml"),
                                   "format: 1\nactions: {jump: {bindings: "
                                   "[{device: keyboard, control: space}]}}\n",
                                   "test.io")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("b.input.yaml"),
                                   "format: 1\nactions: {crouch: {bindings: "
                                   "[{device: keyboard, control: space}]}}\n",
                                   "test.io")
                      .has_value());

    ProjectInputResult merged = load_project_input(dir.path.string());
    REQUIRE(merged.error.has_value());
    CHECK(unwrap(merged.error).code == "input.conflict");
    CHECK(unwrap(merged.error).message.find(generic(dir.file("b.input.yaml"))) !=
          std::string::npos);
}

TEST_CASE("loader.input: load_project_input refuses a non-directory root") {
    testkit::TempDir dir{"loader-project-input-notdir"};
    const std::string not_a_dir = dir.file("plain.input.yaml");
    REQUIRE_FALSE(
        base::write_file(not_a_dir, "format: 1\nactions: {a: {}}\n", "test.io").has_value());
    ProjectInputResult result = load_project_input(not_a_dir);
    REQUIRE(result.error.has_value());
    CHECK(unwrap(result.error).code == "loader.io");
}

TEST_CASE("loader.input_profile: an overlay loads the SAME action/binding grammar") {
    testkit::TempDir dir{"loader-input-profile"};
    const std::string path = dir.file("player.input_profile.yaml");
    REQUIRE_FALSE(base::write_file(path,
                                   "format: 1\n"
                                   "overlay:\n"
                                   "  jump: {bindings: [{device: keyboard, control: enter}]}\n",
                                   "test.io")
                      .has_value());
    ActionMapDecl overlay;
    REQUIRE_FALSE(load_input_profile_file(path, overlay).has_value());
    REQUIRE(overlay.actions.size() == 1);
    CHECK(overlay.actions[0].name == "jump");
    CHECK(overlay.actions[0].bindings[0].control == "enter");
    CHECK(overlay.sticks.empty()); // overlays never declare sticks
}

TEST_CASE("loader.input_profile: `sticks:` is not an allowed overlay key") {
    testkit::TempDir dir{"loader-input-profile-sticks"};
    const std::string path = dir.file("player.input_profile.yaml");
    REQUIRE_FALSE(base::write_file(path, "format: 1\nsticks: {move: {}}\n", "test.io").has_value());
    ActionMapDecl overlay;
    auto error = load_input_profile_file(path, overlay);
    REQUIRE(error.has_value());
    CHECK(unwrap(error).code == "loader.unknown_key");
}

namespace {

ActionMapDecl base_map_with_wasd_and_jump() {
    ActionMapDecl base;
    base.actions.push_back(ActionDesc{"jump", {BindingDesc{DeviceKind::kKeyboard, "space"}}});
    base.actions.push_back(ActionDesc{"crouch", {BindingDesc{DeviceKind::kKeyboard, "c"}}});
    return base;
}

} // namespace

TEST_CASE("loader.apply_overlay: rebinds REPLACE the base action's bindings wholesale") {
    ActionMapDecl base = base_map_with_wasd_and_jump();
    ActionMapDecl overlay;
    overlay.actions.push_back(
        ActionDesc{"jump", {BindingDesc{DeviceKind::kGamepad, "button_south"}}});

    ApplyOverlayResult result = apply_overlay(base, overlay);
    REQUIRE_FALSE(result.error.has_value());
    const ActionDesc* jump = result.map.find_action("jump");
    REQUIRE(jump != nullptr);
    REQUIRE(jump->bindings.size() == 1);
    CHECK(jump->bindings[0].device == DeviceKind::kGamepad);
    CHECK(jump->bindings[0].control == "button_south");
    // Untouched action keeps its base binding.
    const ActionDesc* crouch = result.map.find_action("crouch");
    REQUIRE(crouch != nullptr);
    CHECK(crouch->bindings[0].control == "c");
}

TEST_CASE("loader.apply_overlay: rebinding an undeclared action refuses loader.bad_ref") {
    ActionMapDecl base = base_map_with_wasd_and_jump();
    ActionMapDecl overlay;
    overlay.actions.push_back(ActionDesc{"nope", {}});

    ApplyOverlayResult result = apply_overlay(base, overlay);
    REQUIRE(result.error.has_value());
    CHECK(unwrap(result.error).code == "loader.bad_ref");
}

TEST_CASE(
    "loader.apply_overlay: a rebind that collides with another action refuses input.conflict") {
    ActionMapDecl base = base_map_with_wasd_and_jump();
    ActionMapDecl overlay;
    // Rebind jump onto crouch's control -> a NEW collision the base never had.
    overlay.actions.push_back(ActionDesc{"jump", {BindingDesc{DeviceKind::kKeyboard, "c"}}});

    ApplyOverlayResult result = apply_overlay(base, overlay);
    REQUIRE(result.error.has_value());
    CHECK(unwrap(result.error).code == "input.conflict");
}

TEST_CASE("loader.find_conflict: no conflict among disjoint bindings") {
    ActionMapDecl base = base_map_with_wasd_and_jump();
    CHECK_FALSE(find_conflict(base.actions).has_value());
}

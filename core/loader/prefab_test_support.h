// core/loader/prefab_test_support.h — shared fixture content for
// m1-prefab-spawn's tests ONLY (core/loader/prefab_spawn_test.cpp AND
// ts/runtime/world_host_test.cpp — the statechart::test::ChartFixture
// precedent for a cross-module test-support header). Never compiled into a
// library.

#pragma once

#include "core/base/file_io.h"
#include "testkit/doctest.h"
#include "testkit/temp_dir.h"

#include <filesystem>
#include <string>

namespace midday::loader::test {

// One trivial prefab: a single-region, single-state machine, no components —
// enough to prove the enter chain (Statechart::instantiate always enters the
// initial chain) without dragging in physics/rendering, which are out of
// this node's scope (see core/loader/prefab_spawn.h header note). Returns
// the entity file's path.
inline std::string write_goblin_prefab(const testkit::TempDir& dir) {
    REQUIRE_FALSE(base::write_file(dir.file("goblin.machine.yaml"),
                                   "format: 1\n"
                                   "machine: goblin\n"
                                   "regions:\n"
                                   "  main:\n"
                                   "    initial: Idle\n"
                                   "    states:\n"
                                   "      Idle: {}\n",
                                   "t")
                      .has_value());
    // Forward-slash form: this path is embedded verbatim into a TS string
    // literal (world.spawn('<path>')) where a native Windows backslash is a JS
    // escape and mangles the path; '/' is valid for file I/O on every platform
    // (D-BUILD-113: native-vs-generic broke the Windows lane before).
    const std::string entity_path =
        std::filesystem::path(dir.file("goblin.entity.yaml")).generic_string();
    REQUIRE_FALSE(base::write_file(entity_path,
                                   "format: 1\n"
                                   "entity: Goblin\n"
                                   "machines:\n"
                                   "  - instance: {path: goblin.machine.yaml}\n",
                                   "t")
                      .has_value());
    return entity_path;
}

} // namespace midday::loader::test

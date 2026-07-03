// core/journal/test_support.h — shared fixtures for the journal.* selftests
// ONLY (compiled into midday_journal_tests, never into the library). Bundles
// are written under the system temp directory — selftest never does
// cwd-dependent file IO (D-BUILD-013).

#pragma once

#include "core/base/file_io.h"
#include "core/journal/writer.h"
#include "doctest/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace midday::journal::test {

// Assert-and-access for optional results (shared testkit helper).
using testkit::unwrap;

// A unique, self-deleting bundle-parent directory per test (shared testkit
// helper plus the journal's bundle-path spelling).
struct TempDir : testkit::TempDir {
    explicit TempDir(const std::string& label) : testkit::TempDir("journal-" + label) {}

    [[nodiscard]] std::string bundle(const std::string& name) const { return file(name + ".mrj"); }
};

// The canonical test config: fully pinned, so bundle bytes are identical on
// every platform (the greppable fixture in testkit/ is born from this).
inline WriterConfig pinned_config() {
    WriterConfig config;
    config.engine_version = "0.0.0-fixture";
    config.api_compat_hash = "0000000000000000";
    config.seed = 7;
    config.platform = "fixture-neutral"; // info-only, pinned for byte-compares
    return config;
}

// Read a whole file; empty string when unreadable (tests assert on content).
inline std::string slurp(const std::filesystem::path& path) {
    FILE* file = base::open_file(path, "rb");
    if (file == nullptr)
        return {};
    std::string bytes;
    char chunk[16384];
    while (true) {
        const std::size_t got = std::fread(chunk, 1, sizeof chunk, file);
        bytes.append(chunk, got);
        if (got < sizeof chunk)
            break; // EOF or error: either way the loop is done
    }
    std::fclose(file);
    return bytes;
}

// Overwrite a file (used to tamper bundles for corruption tests).
inline bool spew(const std::filesystem::path& path, const std::string& bytes) {
    FILE* file = base::open_file(path, "wb");
    if (file == nullptr)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return (std::fclose(file) == 0) && ok;
}

} // namespace midday::journal::test

// core/journal/test_support.h — shared fixtures for the journal.* selftests
// ONLY (compiled into midday_journal_tests, never into the library). Bundles
// are written under the system temp directory — selftest never does
// cwd-dependent file IO (D-BUILD-013).

#pragma once

#include "core/journal/file_io.h"
#include "core/journal/writer.h"
#include "doctest/doctest.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace midday::journal::test {

// Assert-and-access for optional results: REQUIRE aborts the test case when
// empty; the (unreachable) abort() makes every subsequent access provably
// checked for the bugprone-unchecked-optional-access dataflow.
template <typename T> T& unwrap(std::optional<T>& opt) {
    REQUIRE(opt.has_value());
    if (!opt.has_value())
        std::abort(); // unreachable: REQUIRE threw
    return *opt;
}

template <typename T> const T& unwrap(const std::optional<T>& opt) {
    REQUIRE(opt.has_value());
    if (!opt.has_value())
        std::abort(); // unreachable: REQUIRE threw
    return *opt;
}

// A unique, self-deleting bundle-parent directory per test.
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& label) {
        static std::atomic<int> counter{0};
        // ASLR of a static's address distinguishes concurrent test processes.
        const auto process_tag =
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&counter));
        const std::string name = "midday-journal-" + label + "-" + std::to_string(process_tag) +
                                 "-" + std::to_string(counter.fetch_add(1));
        path = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    [[nodiscard]] std::string bundle(const std::string& name) const {
        return (path / (name + ".mrj")).string();
    }
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
    FILE* file = detail::open_file(path, "rb");
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
    FILE* file = detail::open_file(path, "wb");
    if (file == nullptr)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return (std::fclose(file) == 0) && ok;
}

} // namespace midday::journal::test

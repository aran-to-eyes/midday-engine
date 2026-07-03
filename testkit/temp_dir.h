// testkit/temp_dir.h — a unique, self-deleting temp directory for selftests
// (hoisted from core/journal/test_support.h at m0-api-json, second-consumer
// rule). Selftest never does cwd-dependent file IO (D-BUILD-013): every test
// that touches disk works under one of these.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace midday::testkit {

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& label) {
        static std::atomic<int> counter{0};
        // ASLR of a static's address distinguishes concurrent test processes.
        const auto process_tag =
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&counter));
        const std::string name = "midday-" + label + "-" + std::to_string(process_tag) + "-" +
                                 std::to_string(counter.fetch_add(1));
        path = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    // Path of a file inside the directory, as a string (verb-argument-ready).
    [[nodiscard]] std::string file(const std::string& name) const { return (path / name).string(); }
};

} // namespace midday::testkit

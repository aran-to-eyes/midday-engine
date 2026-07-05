// core/base/file_io.h — the tree's single stdio file seam (header-only).
// Hoisted from core/journal at m0-api-json on the second-consumer rule (the
// hex.h / doctest_unwrap.h precedent): the api verbs read and write
// engine_api.json through exactly the code path journal bundles use.
//
// open_file() is the only fopen in first-party code: on Windows it goes
// through _wfopen_s over the path's native wide form — which is both the
// correct Unicode-path behavior (path.string() would round-trip through the
// ANSI code page) and the contract MSVC's C4996 exists to enforce. Everywhere
// else it is std::fopen on the native narrow path. Always binary mode —
// deterministic bytes must never meet CRLF translation.
//
// Callers pass their subsystem's stable error code ("journal.io", "api.io");
// messages carry the path and the failing operation.

#pragma once

#include "core/base/error.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace midday::base {

inline FILE* open_file(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    wchar_t wmode[8] = {};
    for (std::size_t i = 0; mode[i] != '\0' && i + 1 < std::size(wmode); ++i)
        wmode[i] = static_cast<wchar_t>(mode[i]);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), wmode) != 0)
        return nullptr;
    return file;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

inline Error file_error(std::string_view code, std::string message) {
    Error error;
    error.code = std::string(code);
    error.message = std::move(message);
    return error;
}

inline std::optional<Error>
write_file(const std::filesystem::path& path, std::string_view bytes, std::string_view error_code) {
    FILE* file = open_file(path, "wb");
    if (file == nullptr)
        return file_error(error_code, "cannot open " + path.string() + " for writing");
    const bool ok =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (!ok || !closed)
        return file_error(error_code, "short write to " + path.string());
    return std::nullopt;
}

struct ReadFileResult {
    std::string bytes;
    std::optional<Error> error;
};

inline ReadFileResult read_file(const std::filesystem::path& path, std::string_view error_code) {
    FILE* file = open_file(path, "rb");
    if (file == nullptr)
        return {{}, file_error(error_code, "cannot open " + path.string() + " for reading")};
    std::string bytes;
    char chunk[65536];
    while (true) {
        const std::size_t got = std::fread(chunk, 1, sizeof chunk, file);
        bytes.append(chunk, got);
        if (got < sizeof chunk)
            break; // EOF or error; ferror() decides below
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed)
        return {{}, file_error(error_code, "read failed on " + path.string())};
    return {std::move(bytes), std::nullopt};
}

} // namespace midday::base

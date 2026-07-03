// core/journal/file_io.h — the journal's single stdio seam (internal).
//
// open_file() is the only fopen in the module: on Windows it goes through
// _wfopen_s over the path's native wide form — which is both the correct
// Unicode-path behavior (path.string() would round-trip through the ANSI
// code page) and the contract MSVC's C4996 exists to enforce. Everywhere
// else it is std::fopen on the native narrow path.

#pragma once

#include "core/base/error.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace midday::journal::detail {

namespace fs = std::filesystem;

inline base::Error io_error(std::string message) {
    base::Error error;
    error.code = "journal.io";
    error.message = std::move(message);
    return error;
}

inline FILE* open_file(const fs::path& path, const char* mode) {
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

inline std::optional<base::Error> write_file(const fs::path& path, std::string_view bytes) {
    FILE* file = open_file(path, "wb");
    if (file == nullptr)
        return io_error("cannot open " + path.string() + " for writing");
    const bool ok =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (!ok || !closed)
        return io_error("short write to " + path.string());
    return std::nullopt;
}

struct ReadFileResult {
    std::string bytes;
    std::optional<base::Error> error;
};

inline ReadFileResult read_file(const fs::path& path) {
    FILE* file = open_file(path, "rb");
    if (file == nullptr)
        return {{}, io_error("cannot open " + path.string() + " for reading")};
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
        return {{}, io_error("read failed on " + path.string())};
    return {std::move(bytes), std::nullopt};
}

} // namespace midday::journal::detail

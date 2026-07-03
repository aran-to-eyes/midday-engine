#include "core/base/name.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace midday::base {
namespace {

// Remap target for the astronomically unlikely nonzero input hashing to 0
// (id 0 is the reserved empty sentinel). Fixed constant: still deterministic.
constexpr std::uint64_t kZeroRemap = 0x9E3779B97F4A7C15ULL;

// The process-wide intern table. Guarded by a mutex (cheap: intern is a
// cold path; comparisons never touch the table). The map is NEVER iterated —
// unordered iteration order must not leak anywhere (spec section 4.3).
struct InternTable {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::unique_ptr<const std::string>> entries;
};

InternTable& table() {
    static InternTable instance;
    return instance;
}

} // namespace

std::uint64_t Name::hash_of(std::string_view text) {
    if (text.empty())
        return 0;
    const std::uint64_t hash = XXH3_64bits(text.data(), text.size());
    return hash != 0 ? hash : kZeroRemap;
}

Name::Name(std::string_view text) {
    if (text.empty())
        return; // the empty name: id 0, no storage
    id_ = hash_of(text);

    InternTable& interned = table();
    const std::lock_guard<std::mutex> lock(interned.mutex);
    auto [it, inserted] = interned.entries.try_emplace(id_);
    if (inserted) {
        it->second = std::make_unique<const std::string>(text);
    } else if (*it->second != text) {
        // A 64-bit content-hash collision breaks the id<->content bijection
        // that journals and goldens depend on. Fail loudly and immediately.
        std::fprintf(stderr,
                     "midday: fatal: Name hash collision: \"%s\" vs \"%.*s\" (id %llu)\n",
                     it->second->c_str(),
                     static_cast<int>(text.size()),
                     text.data(),
                     static_cast<unsigned long long>(id_));
        std::abort();
    }
    str_ = it->second.get();
}

} // namespace midday::base
